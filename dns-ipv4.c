#include "cache.h"
#include "dns-ipv4.h"

int dns_resolve(const char *host, const char *port, int flags,
		resolver_result *res)
{
	char *ep;
	struct hostent *he;
	unsigned int nport;

	he = gethostbyname(host);
	if (!he && (flags & RESOLVE_FAIL_QUIETLY)) {
		if (!h_errno)
			die("BUG: gethostbyname failed but h_errno == 0");
		return h_errno;
	}
	if (!he)
		die("Unable to look up %s (%s)", host, hstrerror(h_errno));

	if (!port) {
		nport = 0;
		goto done;
	}
	nport = strtoul(port, &ep, 10);
	if ( ep == port || *ep ) {
		/* Not numeric */
		struct servent *se = getservbyname(port,"tcp");
		if ( !se )
			die("Unknown port %s", port);
		nport = se->s_port;
	}
done:
	res->he = he;
	res->port = nport;
	return 0;
}
