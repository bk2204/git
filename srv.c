#include "git-compat-util.h"
#include "strbuf.h"
#include "srv.h"

#include <arpa/nameser.h>
#include <resolv.h>

struct parsed_srv_rr {
	int priority;
	int weight;
	int port;
	char *target;
};

static void srv_swap(struct parsed_srv_rr *p1, struct parsed_srv_rr *p2)
{
	char *a, *b;
	int size = sizeof(struct parsed_srv_rr);

	for (a = (char *) p1, b = (char *) p2; size; size--) {
		char t = *a;
		*a++ = *b;
		*b++ = t;
	}
}

static int priority_compare(const void *p1, const void *p2)
{
	const struct parsed_srv_rr *a = p1, *b = p2;

	/* can't overflow because priorities are 16 bits wide */
	return b->priority - a->priority;
}

static int get_qname_for_srv(struct strbuf *sb, const char *host)
{
	const char prefix[] = "_git._tcp.";
	size_t hostlen;

	hostlen = strlen(host);
	if (unsigned_add_overflows(strlen(prefix) + 1, hostlen))
		return error("absurdly long hostname");

	strbuf_reset(sb);
	strbuf_grow(sb, strlen(prefix) + hostlen);
	strbuf_add(sb, prefix, strlen(prefix));
	strbuf_add(sb, host, hostlen);
	return 0;
}

static int srv_parse_rr(const ns_msg *msg,
			const ns_rr *rr, struct parsed_srv_rr *res)
{
	const unsigned char *p;
	char buf[1024];

	if (ns_rr_rdlen(*rr) < 2+2+2 /* priority, weight, port */)
		return error("SRV RR is too short");

	p = ns_rr_rdata(*rr);
	res->priority = *p++ << CHAR_BIT;
	res->priority += *p++;

	res->weight = *p++ << CHAR_BIT;
	res->weight += *p++;

	res->port = *p++ << CHAR_BIT;
	res->port += *p++;

	/*
	 * RFC2782 doesn't allow compressed target domain names but we
	 * might as well accept them anyway.
	 */
	if (dn_expand(ns_msg_base(*msg), ns_msg_end(*msg), p,
			buf, sizeof(buf)) < 0)
		return error("cannot expand target domain name in SRV RR");

	res->target = xstrdup(buf);
	return 0;
}

static int srv_parse(ns_msg *msg, struct parsed_srv_rr **res)
{
	struct parsed_srv_rr *rrs = NULL;
	int nr_parsed = 0;
	int cnames = 0;
	int i, n;

	n = ns_msg_count(*msg, ns_s_an);

	/* skip CNAME records */
	for (i = 0; i < n; i++) {
		ns_rr rr;
		if (ns_parserr(msg, ns_s_an, i, &rr)) {
			error("cannot parse DNS RR: %s", strerror(errno));
			goto fail;
		}
		if (ns_rr_type(rr) != ns_t_cname)
			break;
	}
	cnames = i;
	n -= cnames;

	rrs = xmalloc(n * sizeof(*rrs));
	for (i = 0; i < n; i++) {
		ns_rr rr;

		if (ns_parserr(msg, ns_s_an, cnames + i, &rr)) {
			error("cannot parse DNS RR: %s", strerror(errno));
			goto fail;
		}
		if (ns_rr_type(rr) != ns_t_srv) {
			error("expected SRV RR, found RR type %d",
						(int) ns_rr_type(rr));
			goto fail;
		}
		if (srv_parse_rr(msg, &rr, rrs + i))
			/* srv_parse_rr writes a message */
			goto fail;
		nr_parsed++;
	}

	*res = rrs;
	return n;
fail:
	for (i = 0; i < nr_parsed; i++)
		free(rrs[i].target);
	free(rrs);
	return -1;
}

static int weighted_item(struct parsed_srv_rr *rrs, int n, unsigned int pos)
{
	int i;

	for (i = 0; i < n; i++) {
		unsigned int wt = rrs[i].weight;

		if (pos <= wt)
			break;
		pos -= wt;
	}
	return i;
}

static void shuffle_one(struct parsed_srv_rr *rrs, int n,
				int i, int *j, unsigned int *wt_remaining)
{
	unsigned int pos;
	int k;

	pos = (unsigned int) ((*wt_remaining + 1) * drand48());

	if (!pos) {
		*wt_remaining -= rrs[i].weight;
		return;
	}

	/* Which item will take the place of rrs[i]? */
	if (*j < i)
		*j = i;
	k = *j + weighted_item(rrs + *j, n - *j, pos);

	assert(k < n);
	*wt_remaining -= rrs[k].weight;

	if (k == i)
		return;

	srv_swap(rrs + i, rrs + k);

	/*
	 * If rrs[i] had weight zero, move it to stay in the clump
	 * of weight-zero records.  rrs[k] cannot have had weight
	 * zero because pos > 0.
	 */
	assert(*j <= k);
	if (i < *j) {
		srv_swap(rrs + k, rrs + *j);
		(*j)++;
	}
}

static void weighted_shuffle(struct parsed_srv_rr *rrs, int n)
{
	int i, j;
	unsigned int total = 0;
	static int seeded;

	/*
	 * Calculate total weight and move weight-zero
	 * records to the beginning of the array.
	 */
	assert(n < (1 << 16));
	for (i = j = 0; i < n; i++) {
		unsigned int wt = rrs[i].weight;
		assert(wt < (1 << 16));

		if (!wt) {
			srv_swap(rrs + i, rrs + j);
			j++;
		}

		/*
		 * In the worst case, n is 2^16 - 1 and
		 * each weight is 2^16 - 1, making the total
		 * a little less than 2^32.
		 */
		assert(!unsigned_add_overflows(total, wt + 1));
		total += wt;
	}
	/* Usual case: all weights are zero. */
	if (!total)
		return;

	if (!seeded) {
		seeded = 1;
		srand48(time(NULL));
	}

	for (i = 0; i < n; i++)
		/*
		 * Now the records starting at rrs[i] could be in any order,
		 * except those of weight 0 are at the start of the list
		 * (ending with rrs[j-1]).
		 *
		 * Pick an item from rrs[i]..rrs[n] at random, taking weights
		 * into account, and reorder to make it rrs[i], preserving
		 * that invariant.
		 */
		shuffle_one(rrs, n, i, &j, &total);
}

static void sort_rrs(struct parsed_srv_rr *rrs, int n)
{
	int i, j, prio;

	qsort(rrs, n, sizeof(*rrs), priority_compare);

	/*
	 * Within each priority level, order servers randomly,
	 * respecting weight.
	 */
	j = 0;
	prio = rrs[j].priority;
	for (i = 0; i < n; i++) {
		if (rrs[i].priority == prio)
			continue;

		weighted_shuffle(rrs + j, i - j);
		j = i;
		prio = rrs[j].priority;
	}
	weighted_shuffle(rrs + j, n - j);
}

/* Reference: RFC2782. */
int get_srv(const char *host, struct host **hosts)
{
	struct strbuf sb = STRBUF_INIT;
	unsigned char buf[1024];
	ns_msg msg;
	int len, n, i, ret;
	struct parsed_srv_rr *rrs = NULL;
	struct host *reply = NULL;

	/* if no SRV record is found, fall back to plain address lookup */
	ret = 0;

	/* _git._tcp.<host> */
	if (get_qname_for_srv(&sb, host))
		goto out;
	len = res_query(sb.buf, ns_c_in, ns_t_srv, buf, sizeof(buf));
	if (len < 0)
		goto out;

	/* If a SRV RR cannot be parsed, give up. */
	ret = -1;

	if (ns_initparse(buf, len, &msg)) {
		error("cannot initialize DNS parser: %s", strerror(errno));
		goto out;
	}
	n = srv_parse(&msg, &rrs);
	if (n < 0)
		/* srv_parse writes a message */
		goto out;
	if (!n) {
		ret = 0;
		goto out;
	}
	assert(n < (1 << 16));

	/* A single RR with target "." means "go away". */
	if (n == 1 &&
	    (!*rrs[0].target || !strcmp(rrs[0].target, ".")))
		goto out2;

	sort_rrs(rrs, n);

	/* Success! */
	ret = n;
	reply = xmalloc(n * sizeof(*reply));
	for (i = 0; i < n; i++) {
		char buf[32];
		snprintf(buf, sizeof(buf), "%d", rrs[i].port);

		reply[i].hostname = rrs[i].target;
		reply[i].port = xstrdup(buf);
	}
	*hosts = reply;
	goto out;

out2:
	for (i = 0; i < n; i++)
		free(rrs[i].target);
out:
	free(rrs);
	strbuf_release(&sb);
	return ret;
}
