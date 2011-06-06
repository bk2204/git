#include "cache.h"
#include "run-command.h"
#include "srv.h"

#ifndef NO_IPV6
#include "dns-ipv6.h"
#else
#include "dns-ipv4.h"
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

void git_locate_host(const char *hostname, char **ip_address,
					char **canon_hostname)
{
	resolver_result ai;
	resolved_address i;

	if (dns_resolve(hostname, NULL,
			RESOLVE_CANONNAME | RESOLVE_FAIL_QUIETLY, &ai))
		return;

	for_each_address(i, ai) {
		free(*ip_address);
		*ip_address = dns_ip_address(&i, &ai);

		free(*canon_hostname);
		*canon_hostname =
			dns_canonname(i, ai) ? xstrdup(dns_canonname(i, ai)) :
			*ip_address ? xstrdup(*ip_address) :
			NULL;
		break;
	}

	dns_free(ai);
}

/*
 * Returns a connected socket() fd, or else die()s.
 */
static int git_tcp_connect_sock(char *host, int flags)
{
	struct strbuf error_message = STRBUF_INIT;
	int sockfd = -1, gai = 0;
	const char *port = NULL;
	struct host *hosts = NULL;
	int j, n = 0;

	get_host_and_port(&host, &port);
	if (!port) {
		port = STR(DEFAULT_GIT_PORT);
		n = get_srv(host, &hosts);
	}
	if (n < 0)
		die("Unable to look up %s", host);
	if (!*port)
		port = "<none>";
	if (!n) {
		hosts = xmalloc(sizeof(*hosts));
		hosts[0].hostname = xstrdup(host);
		hosts[0].port = xstrdup(port);
		n = 1;
	}

	for (j = 0; j < n; j++) {
		resolver_result ai;
		resolved_address i;
		int cnt;

		if (flags & CONNECT_VERBOSE)
			fprintf(stderr, "Looking up %s ... ", hosts[j].hostname);

		gai = dns_resolve(hosts[j].hostname,
				hosts[j].port, RESOLVE_FAIL_QUIETLY, &ai);
		if (gai) {
			if (flags & CONNECT_VERBOSE)
				fprintf(stderr, "failed.\n");

			if (n == 1 && !strcmp(host, hosts[j].hostname))
				strbuf_addf(&error_message, "%s: %s\n",
					host, dns_strerror(gai));
			else
				strbuf_addf(&error_message,
					"%s[%d: %s:%s]: %s\n", host, j,
					hosts[j].hostname, hosts[j].port,
					dns_strerror(gai));
			continue;
		}

		if (flags & CONNECT_VERBOSE)
			fprintf(stderr, "done.\nConnecting to %s (port %s) ... ",
					hosts[j].hostname, hosts[j].port);

		cnt = -1;
		for_each_address(i, ai) {
			cnt++;
			sockfd = socket(dns_family(i, ai),
					dns_socktype(i, ai), dns_protocol(i, ai));
			if (sockfd < 0 ||
			    connect(sockfd, dns_addr(i, ai), dns_addrlen(i, ai)) < 0) {
				strbuf_addf(&error_message, "%s[%d: %s]: errno=%s\n",
						hosts[j].hostname,
						cnt,
						dns_name(&i),
						strerror(errno));
				if (0 <= sockfd)
					close(sockfd);
				sockfd = -1;
				continue;
			}
			if (flags & CONNECT_VERBOSE)
				fprintf(stderr, "%s ", dns_name(&i));
			break;
		}

		dns_free(ai);

		if (sockfd >= 0)
			break;
	}

	if (gai || sockfd < 0)
		die("unable to connect to %s:\n%s", host, error_message.buf);

	enable_keepalive(sockfd);

	if (flags & CONNECT_VERBOSE)
		fprintf(stderr, "done.\n");

	for (j = 0; j < n; j++) {
		free(hosts[j].hostname);
		free(hosts[j].port);
	}
	free(hosts);
	strbuf_release(&error_message);
	return sockfd;
}

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
