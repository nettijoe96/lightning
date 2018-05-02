#include <arpa/inet.h>
#include <assert.h>
#include <ccan/tal/str/str.h>
#include <common/type_to_string.h>
#include <common/utils.h>
#include <common/wireaddr.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <wire/wire.h>

/* Returns false if we didn't parse it, and *cursor == NULL if malformed. */
bool fromwire_wireaddr(const u8 **cursor, size_t *max, struct wireaddr *addr)
{
	addr->type = fromwire_u8(cursor, max);

	switch (addr->type) {
	case ADDR_TYPE_IPV4:
		addr->addrlen = 4;
		break;
	case ADDR_TYPE_IPV6:
		addr->addrlen = 16;
		break;
	case ADDR_TYPE_PADDING:
		addr->addrlen = ((u8)*max) - 2;
		break;
	default:
		return false;
	}
	fromwire(cursor, max, addr->addr, addr->addrlen);
	addr->port = fromwire_u16(cursor, max);

	return *cursor != NULL;
}

void towire_wireaddr(u8 **pptr, const struct wireaddr *addr)
{
	if (!addr) {
		towire_u8(pptr, ADDR_TYPE_PADDING);
		return;
	}
	towire_u8(pptr, addr->type);
	towire(pptr, addr->addr, addr->addrlen);
	towire_u16(pptr, addr->port);
}

void towire_wireaddr_or_sockname(u8 **pptr,
				 const struct wireaddr_or_sockname *addr)
{
	towire_bool(pptr, addr->is_sockname);
	if (addr->is_sockname)
		towire_u8_array(pptr, (const u8 *)addr->u.sockname,
				sizeof(addr->u.sockname));
	else
		towire_wireaddr(pptr, &addr->u.wireaddr);
}

bool fromwire_wireaddr_or_sockname(const u8 **cursor, size_t *max,
				   struct wireaddr_or_sockname *addr)
{
	addr->is_sockname = fromwire_bool(cursor, max);
	if (addr->is_sockname) {
		fromwire_u8_array(cursor, max, (u8 *)addr->u.sockname,
				  sizeof(addr->u.sockname));
		/* Must be NUL terminated */
		if (!memchr(addr->u.sockname, 0, sizeof(addr->u.sockname)))
			fromwire_fail(cursor, max);
		return *cursor != NULL;
	} else
		return fromwire_wireaddr(cursor, max, &addr->u.wireaddr);
}

char *fmt_wireaddr(const tal_t *ctx, const struct wireaddr *a)
{
	char addrstr[INET6_ADDRSTRLEN];
	char *ret, *hex;

	switch (a->type) {
	case ADDR_TYPE_IPV4:
		if (!inet_ntop(AF_INET, a->addr, addrstr, INET_ADDRSTRLEN))
			return "Unprintable-ipv4-address";
		return tal_fmt(ctx, "%s:%u", addrstr, a->port);
	case ADDR_TYPE_IPV6:
		if (!inet_ntop(AF_INET6, a->addr, addrstr, INET6_ADDRSTRLEN))
			return "Unprintable-ipv6-address";
		return tal_fmt(ctx, "[%s]:%u", addrstr, a->port);
	case ADDR_TYPE_PADDING:
		break;
	}

	hex = tal_hexstr(ctx, a->addr, a->addrlen);
	ret = tal_fmt(ctx, "Unknown type %u %s:%u", a->type, hex, a->port);
	tal_free(hex);
	return ret;
}
REGISTER_TYPE_TO_STRING(wireaddr, fmt_wireaddr);

char *fmt_wireaddr_or_sockname(const tal_t *ctx,
			       const struct wireaddr_or_sockname *a)
{
	if (a->is_sockname)
		return tal_fmt(ctx, "%s", a->u.sockname);
	return fmt_wireaddr(ctx, &a->u.wireaddr);
}
REGISTER_TYPE_TO_STRING(wireaddr_or_sockname, fmt_wireaddr_or_sockname);

/* Valid forms:
 *
 * [anything]:<number>
 * anything-without-colons-or-left-brace:<number>
 * anything-without-colons
 * string-with-multiple-colons
 *
 * Returns false if it wasn't one of these forms.  If it returns true,
 * it only overwrites *port if it was specified by <number> above.
 */
static bool separate_address_and_port(const tal_t *ctx, const char *arg,
				      char **addr, u16 *port)
{
	char *portcolon;

	if (strstarts(arg, "[")) {
		char *end = strchr(arg, ']');
		if (!end)
			return false;
		/* Copy inside [] */
		*addr = tal_strndup(ctx, arg + 1, end - arg - 1);
		portcolon = strchr(end+1, ':');
	} else {
		portcolon = strchr(arg, ':');
		if (portcolon) {
			/* Disregard if there's more than one : or if it's at
			   the start or end */
			if (portcolon != strrchr(arg, ':')
			    || portcolon == arg
			    || portcolon[1] == '\0')
				portcolon = NULL;
		}
		if (portcolon)
			*addr = tal_strndup(ctx, arg, portcolon - arg);
		else
			*addr = tal_strdup(ctx, arg);
	}

	if (portcolon) {
		char *endp;
		*port = strtol(portcolon + 1, &endp, 10);
		return *port != 0 && *endp == '\0';
	}
	return true;
}

bool wireaddr_from_hostname(struct wireaddr *addr, const char *hostname,
			    const u16 port, const char **err_msg)
{
	struct sockaddr_in6 *sa6;
	struct sockaddr_in *sa4;
	struct addrinfo *addrinfo;
	struct addrinfo hints;
	int gai_err;
	bool res = false;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = 0;
	hints.ai_flags = AI_ADDRCONFIG;
	gai_err = getaddrinfo(hostname, tal_fmt(tmpctx, "%d", port),
			      &hints, &addrinfo);
	if (gai_err != 0) {
		if (err_msg)
			*err_msg = gai_strerror(gai_err);
		return false;
	}
	/* Use only the first found address */
	if (addrinfo->ai_family == AF_INET) {
		addr->type = ADDR_TYPE_IPV4;
		addr->addrlen = 4;
		addr->port = port;
		sa4 = (struct sockaddr_in *) addrinfo->ai_addr;
		memcpy(&addr->addr, &sa4->sin_addr, addr->addrlen);
		res = true;
	} else if (addrinfo->ai_family == AF_INET6) {
		addr->type = ADDR_TYPE_IPV6;
		addr->addrlen = 16;
		addr->port = port;
		sa6 = (struct sockaddr_in6 *) addrinfo->ai_addr;
		memcpy(&addr->addr, &sa6->sin6_addr, addr->addrlen);
		res = true;
	}

	/* Clean up */
	freeaddrinfo(addrinfo);
	return res;
}

bool parse_wireaddr(const char *arg, struct wireaddr *addr, u16 defport,
		    const char **err_msg)
{
	struct in6_addr v6;
	struct in_addr v4;
	u16 port;
	char *ip;
	bool res;

	res = false;
	port = defport;
	if (err_msg)
		*err_msg = NULL;

	if (!separate_address_and_port(tmpctx, arg, &ip, &port))
		goto finish;

	if (streq(ip, "localhost"))
		ip = "127.0.0.1";
	else if (streq(ip, "ip6-localhost"))
		ip = "::1";

	memset(&addr->addr, 0, sizeof(addr->addr));

	if (inet_pton(AF_INET, ip, &v4) == 1) {
		addr->type = ADDR_TYPE_IPV4;
		addr->addrlen = 4;
		addr->port = port;
		memcpy(&addr->addr, &v4, addr->addrlen);
		res = true;
	} else if (inet_pton(AF_INET6, ip, &v6) == 1) {
		addr->type = ADDR_TYPE_IPV6;
		addr->addrlen = 16;
		addr->port = port;
		memcpy(&addr->addr, &v6, addr->addrlen);
		res = true;
	}

	/* Resolve with getaddrinfo */
	if (!res)
		res = wireaddr_from_hostname(addr, ip, port, err_msg);

finish:
	if (!res && err_msg && !*err_msg)
		*err_msg = "Error parsing hostname";
	return res;
}

bool parse_wireaddr_or_sockname(const char *arg, struct wireaddr_or_sockname *addr, u16 port, const char **err_msg)
{
	/* Addresses starting with '/' are local socket paths */
	if (arg[0] == '/') {
		addr->is_sockname = true;

		/* Check if the path is too long */
		if (strlen(arg) >= sizeof(addr->u.sockname)) {
			if (err_msg)
				*err_msg = "Socket name too long";
			return false;
		}
		strcpy(addr->u.sockname, arg);
		return true;
	}

	addr->is_sockname = false;
	return parse_wireaddr(arg, &addr->u.wireaddr, port, err_msg);
}
