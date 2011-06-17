#ifndef DNS_IPV4_H
#define DNS_IPV4_H

#define ADDRBUFLEN 64	/* 46 for an ipv6 address, plus a little extra */

struct ipv4_address {
	char **ap;
	struct sockaddr_in sa;
};

struct ipv4_addrinfo {
	struct hostent *he;
	unsigned int port;
};

typedef struct ipv4_addrinfo resolver_result;
typedef struct ipv4_address resolved_address;

enum {
	RESOLVE_CANONNAME = 1,
	/*
	 * Quietly return an error code instead of exiting on error.
	 * Callers can use dns_strerror() to get an error string.
	 */
	RESOLVE_FAIL_QUIETLY = 2
};
extern int dns_resolve(const char *host, const char *port, int flags,
			resolver_result *res);

static inline const char *dns_name(const resolved_address *addr)
{
	return inet_ntoa(*(struct in_addr *)&addr->sa.sin_addr);
}

static inline char *dns_ip_address(const resolved_address *addr,
					const resolver_result *ai)
{
	char addrbuf[ADDRBUFLEN];
	if (!inet_ntop(ai->he->h_addrtype, &addr->sa.sin_addr,
		  addrbuf, sizeof(addrbuf)))
		return NULL;
	return xstrdup(addrbuf);
}

static inline int dns_fill_sockaddr_(char *ap,
		const struct ipv4_addrinfo *ai, struct sockaddr_in *sa)
{
	if (!ap)	/* done. */
		return -1;

	memset(sa, 0, sizeof(*sa));
	sa->sin_family = ai->he->h_addrtype;
	sa->sin_port = htons(ai->port);
	memcpy(&sa->sin_addr, ap, ai->he->h_length);
	return 0;
}

#define for_each_address(addr, ai) \
	for ((addr).ap = (ai).he->h_addr_list; \
	     !dns_fill_sockaddr_(*(addr).ap, &(ai), &(addr).sa); \
	     (addr).ap++)

#define dns_family(addr, ai) ((ai).he->h_addrtype)
#define dns_socktype(addr, ai) SOCK_STREAM
#define dns_protocol(addr, ai) 0
#define dns_addr(addr, ai) ((struct sockaddr *) &(addr).sa)
#define dns_addrlen(addr, ai) sizeof((addr).sa)
#define dns_canonname(addr, ai) ((ai).he->h_name)

#define dns_strerror(n) hstrerror(n)
#define dns_free(ai) do { /* nothing */ } while (0)

#endif
