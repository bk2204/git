#ifndef TCP_H
#define TCP_H

extern void git_locate_host(const char *hostname,
			char **ip_address, char **canon_hostname);

extern int git_use_proxy(const char *host);
extern void git_tcp_connect(int fd[2], char *host, int flags);
extern struct child_process *git_proxy_connect(int fd[2], char *host);

#endif
