#ifndef DNS_IPV6_H
#define DNS_IPV6_H

typedef struct addrinfo *resolver_result;
typedef const struct addrinfo *resolved_address;

enum {
	RESOLVE_CANONNAME = 1,
	RESOLVE_FAIL_QUIETLY = 2
};
extern int dns_resolve(const char *host, const char *port, int flags,
			resolver_result *res);
/* result is in static buffer */
extern const char *dns_name(const resolved_address *i);
/* result is in malloc'ed buffer */
extern char *dns_ip_address(const resolved_address *i,
				const resolver_result *ai);

#define for_each_address(i, ai) \
	for (i = ai; i; i = (i)->ai_next)

#define dns_family(i, ai) ((i)->ai_family)
#define dns_socktype(i, ai) ((i)->ai_socktype)
#define dns_protocol(i, ai) ((i)->ai_protocol)
#define dns_addr(i, ai) ((i)->ai_addr)
#define dns_addrlen(i, ai) ((i)->ai_addrlen)
#define dns_canonname(i, ai) ((i)->ai_canonname)

#define dns_strerror(gai) gai_strerror(gai)
#define dns_free(ai) freeaddrinfo(ai)

#endif
