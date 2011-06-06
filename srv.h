#ifndef SRV_H
#define SRV_H

struct host {
	char *hostname;
	char *port;
};

#ifndef USE_SRV_RR
#define get_srv(host, hosts) 0
#else
extern int get_srv(const char *host, struct host **hosts);
#endif

#endif
