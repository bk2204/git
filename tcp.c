#include "cache.h"
#include "run-command.h"

#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 256
#endif

#define STR_(s)	# s
#define STR(s)	STR_(s)

static void get_host_and_port(char **host, const char **port)
{
	char *colon, *end;

	if (*host[0] == '[') {
		end = strchr(*host + 1, ']');
		if (end) {
			*end = 0;
			end++;
			(*host)++;
		} else
			end = *host;
	} else
		end = *host;
	colon = strchr(end, ':');

	if (colon) {
		*colon = 0;
		*port = colon + 1;
	}
}

static void enable_keepalive(int sockfd)
{
	int ka = 1;

	if (setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, &ka, sizeof(ka)) < 0)
		fprintf(stderr, "unable to set SO_KEEPALIVE on socket: %s\n",
			strerror(errno));
}

#ifndef NO_IPV6

static const char *ai_name(const struct addrinfo *ai)
{
	static char addr[NI_MAXHOST];
	if (getnameinfo(ai->ai_addr, ai->ai_addrlen, addr, sizeof(addr), NULL, 0,
			NI_NUMERICHOST) != 0)
		strcpy(addr, "(unknown)");

	return addr;
}

void git_locate_host(const char *hostname, char **ip_address,
					char **canon_hostname)
{
	struct addrinfo hints;
	struct addrinfo *ai;
	int gai;
	static char addrbuf[HOST_NAME_MAX + 1];
	struct sockaddr_in *sin_addr;

	memset(&hints, 0, sizeof(hints));
	hints.ai_flags = AI_CANONNAME;

	gai = getaddrinfo(hostname, NULL, &hints, &ai);
	if (gai)
		return;

	sin_addr = (void *)ai->ai_addr;
	inet_ntop(AF_INET, &sin_addr->sin_addr, addrbuf, sizeof(addrbuf));
	free(*ip_address);
	*ip_address = xstrdup(addrbuf);

	free(*canon_hostname);
	*canon_hostname = xstrdup(ai->ai_canonname ?
				  ai->ai_canonname : *ip_address);

	freeaddrinfo(ai);
}

/*
 * Returns a connected socket() fd, or else die()s.
 */
static int git_tcp_connect_sock(char *host, int flags)
{
	struct strbuf error_message = STRBUF_INIT;
	int sockfd = -1;
	const char *port = STR(DEFAULT_GIT_PORT);
	struct addrinfo hints, *ai0, *ai;
	int gai;
	int cnt = 0;

	get_host_and_port(&host, &port);
	if (!*port)
		port = "<none>";

	memset(&hints, 0, sizeof(hints));
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	if (flags & CONNECT_VERBOSE)
		fprintf(stderr, "Looking up %s ... ", host);

	gai = getaddrinfo(host, port, &hints, &ai);
	if (gai)
		die("Unable to look up %s (port %s) (%s)", host, port, gai_strerror(gai));

	if (flags & CONNECT_VERBOSE)
		fprintf(stderr, "done.\nConnecting to %s (port %s) ... ", host, port);

	for (ai0 = ai; ai; ai = ai->ai_next, cnt++) {
		sockfd = socket(ai->ai_family,
				ai->ai_socktype, ai->ai_protocol);
		if ((sockfd < 0) ||
		    (connect(sockfd, ai->ai_addr, ai->ai_addrlen) < 0)) {
			strbuf_addf(&error_message, "%s[%d: %s]: errno=%s\n",
				    host, cnt, ai_name(ai), strerror(errno));
			if (0 <= sockfd)
				close(sockfd);
			sockfd = -1;
			continue;
		}
		if (flags & CONNECT_VERBOSE)
			fprintf(stderr, "%s ", ai_name(ai));
		break;
	}

	freeaddrinfo(ai0);

	if (sockfd < 0)
		die("unable to connect to %s:\n%s", host, error_message.buf);

	enable_keepalive(sockfd);

	if (flags & CONNECT_VERBOSE)
		fprintf(stderr, "done.\n");

	strbuf_release(&error_message);

	return sockfd;
}

#else /* NO_IPV6 */

void git_locate_host(const char *hostname, char **ip_address,
					char **canon_hostname)
{
	struct hostent *hent;
	struct sockaddr_in sa;
	char **ap;
	static char addrbuf[HOST_NAME_MAX + 1];

	hent = gethostbyname(hostname);

	ap = hent->h_addr_list;
	memset(&sa, 0, sizeof sa);
	sa.sin_family = hent->h_addrtype;
	sa.sin_port = htons(0);
	memcpy(&sa.sin_addr, *ap, hent->h_length);

	inet_ntop(hent->h_addrtype, &sa.sin_addr,
		  addrbuf, sizeof(addrbuf));

	free(*canon_hostname);
	*canon_hostname = xstrdup(hent->h_name);
	free(*ip_address);
	*ip_address = xstrdup(addrbuf);
}

/*
 * Returns a connected socket() fd, or else die()s.
 */
static int git_tcp_connect_sock(char *host, int flags)
{
	struct strbuf error_message = STRBUF_INIT;
	int sockfd = -1;
	const char *port = STR(DEFAULT_GIT_PORT);
	char *ep;
	struct hostent *he;
	struct sockaddr_in sa;
	char **ap;
	unsigned int nport;
	int cnt;

	get_host_and_port(&host, &port);

	if (flags & CONNECT_VERBOSE)
		fprintf(stderr, "Looking up %s ... ", host);

	he = gethostbyname(host);
	if (!he)
		die("Unable to look up %s (%s)", host, hstrerror(h_errno));
	nport = strtoul(port, &ep, 10);
	if ( ep == port || *ep ) {
		/* Not numeric */
		struct servent *se = getservbyname(port,"tcp");
		if ( !se )
			die("Unknown port %s", port);
		nport = se->s_port;
	}

	if (flags & CONNECT_VERBOSE)
		fprintf(stderr, "done.\nConnecting to %s (port %s) ... ", host, port);

	for (cnt = 0, ap = he->h_addr_list; *ap; ap++, cnt++) {
		memset(&sa, 0, sizeof sa);
		sa.sin_family = he->h_addrtype;
		sa.sin_port = htons(nport);
		memcpy(&sa.sin_addr, *ap, he->h_length);

		sockfd = socket(he->h_addrtype, SOCK_STREAM, 0);
		if ((sockfd < 0) ||
		    connect(sockfd, (struct sockaddr *)&sa, sizeof sa) < 0) {
			strbuf_addf(&error_message, "%s[%d: %s]: errno=%s\n",
				host,
				cnt,
				inet_ntoa(*(struct in_addr *)&sa.sin_addr),
				strerror(errno));
			if (0 <= sockfd)
				close(sockfd);
			sockfd = -1;
			continue;
		}
		if (flags & CONNECT_VERBOSE)
			fprintf(stderr, "%s ",
				inet_ntoa(*(struct in_addr *)&sa.sin_addr));
		break;
	}

	if (sockfd < 0)
		die("unable to connect to %s:\n%s", host, error_message.buf);

	enable_keepalive(sockfd);

	if (flags & CONNECT_VERBOSE)
		fprintf(stderr, "done.\n");

	return sockfd;
}

#endif /* NO_IPV6 */


void git_tcp_connect(int fd[2], char *host, int flags)
{
	int sockfd = git_tcp_connect_sock(host, flags);

	fd[0] = sockfd;
	fd[1] = dup(sockfd);
}


static char *git_proxy_command;

static int git_proxy_command_options(const char *var, const char *value,
		void *cb)
{
	if (!strcmp(var, "core.gitproxy")) {
		const char *for_pos;
		int matchlen = -1;
		int hostlen;
		const char *rhost_name = cb;
		int rhost_len = strlen(rhost_name);

		if (git_proxy_command)
			return 0;
		if (!value)
			return config_error_nonbool(var);
		/* [core]
		 * ;# matches www.kernel.org as well
		 * gitproxy = netcatter-1 for kernel.org
		 * gitproxy = netcatter-2 for sample.xz
		 * gitproxy = netcatter-default
		 */
		for_pos = strstr(value, " for ");
		if (!for_pos)
			/* matches everybody */
			matchlen = strlen(value);
		else {
			hostlen = strlen(for_pos + 5);
			if (rhost_len < hostlen)
				matchlen = -1;
			else if (!strncmp(for_pos + 5,
					  rhost_name + rhost_len - hostlen,
					  hostlen) &&
				 ((rhost_len == hostlen) ||
				  rhost_name[rhost_len - hostlen -1] == '.'))
				matchlen = for_pos - value;
			else
				matchlen = -1;
		}
		if (0 <= matchlen) {
			/* core.gitproxy = none for kernel.org */
			if (matchlen == 4 &&
			    !memcmp(value, "none", 4))
				matchlen = 0;
			git_proxy_command = xmemdupz(value, matchlen);
		}
		return 0;
	}

	return git_default_config(var, value, cb);
}

int git_use_proxy(const char *host)
{
	git_proxy_command = getenv("GIT_PROXY_COMMAND");
	git_config(git_proxy_command_options, (void*)host);
	return (git_proxy_command && *git_proxy_command);
}

struct child_process *git_proxy_connect(int fd[2], char *host)
{
	const char *port = STR(DEFAULT_GIT_PORT);
	const char **argv;
	struct child_process *proxy;

	get_host_and_port(&host, &port);

	argv = xmalloc(sizeof(*argv) * 4);
	argv[0] = git_proxy_command;
	argv[1] = host;
	argv[2] = port;
	argv[3] = NULL;
	proxy = xcalloc(1, sizeof(*proxy));
	proxy->argv = argv;
	proxy->in = -1;
	proxy->out = -1;
	if (start_command(proxy))
		die("cannot start proxy %s", argv[0]);
	fd[0] = proxy->out; /* read from proxy stdout */
	fd[1] = proxy->in;  /* write to proxy stdin */
	return proxy;
}
