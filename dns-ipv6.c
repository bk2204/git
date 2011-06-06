#include "cache.h"
#include "dns-ipv6.h"

#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 256
#endif

const char *dns_name(const resolved_address *i)
{
	const struct addrinfo *ai = *i;
	static char addr[NI_MAXHOST];
	if (getnameinfo(ai->ai_addr, ai->ai_addrlen, addr, sizeof(addr), NULL, 0,
			NI_NUMERICHOST) != 0)
		strcpy(addr, "(unknown)");

	return addr;
}

char *dns_ip_address(const resolved_address *i, const resolver_result *ai0)
{
	const struct addrinfo *ai = *i;
	char addrbuf[HOST_NAME_MAX + 1];
	struct sockaddr_in *sin_addr;

	sin_addr = (void *)ai->ai_addr;
	inet_ntop(AF_INET, &sin_addr->sin_addr, addrbuf, sizeof(addrbuf));
	return xstrdup(addrbuf);
}

int dns_resolve(const char *host, const char *port, int flags,
		resolver_result *res)
{
	struct addrinfo hints;
	int gai;

	memset(&hints, 0, sizeof(hints));
	if (flags & RESOLVE_CANONNAME)
		hints.ai_flags = AI_CANONNAME;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	gai = getaddrinfo(host, port, &hints, res);
	if (gai && (flags & RESOLVE_FAIL_QUIETLY))
		return gai;
	if (gai)
		die("Unable to look up %s (port %s) (%s)", host, port, gai_strerror(gai));

	return 0;
}
