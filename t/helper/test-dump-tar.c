#include "test-tool.h"
#include "cache.h"
#include "tar.h"

#define TAR_BLOCK_SIZE 512

enum archive_type {
	TYPE_DEFAULT,
	TYPE_CTAR_V1,
	TYPE_CTAR_V1_T,
};

static int canonical_version(enum archive_type atype)
{
	switch (atype) {
	case TYPE_CTAR_V1:
	case TYPE_CTAR_V1_T:
		return 1;
	default:
		return 0;
	}
}

const static uint8_t all_zeros[TAR_BLOCK_SIZE] = { 0 };

static unsigned long long verify_numeric(const char *p, size_t sz, const char *name)
{
	size_t i;
	char *end;
	unsigned long long val;

	if (p[sz - 1] != '\0')
		die("expected %s to be NUL-terminated", name);
	for (i = 0; i < sz - 1; i++)
		if (p[i] < '0' || p[i] > '7')
			die("found non-octal value in %s", name);
	val = strtoull(p, &end, 8);
	if (end != p + sz - 1)
		die("failed to parse octal value in %s", name);
	return val;
}

static void verify_string(const char *s, size_t sz, const char *name)
{
	const char *s_end = memchr(s, '\0', sz);
	const char *p;
	if (!s_end)
		s_end = s + sz;
	for (p = s_end; p < s + sz; p++)
		if (*p)
			die("found non-NUL value in %s", name);
}

static const char *dump_ustar_string(const char *s, size_t sz) {
	static char buf[TAR_BLOCK_SIZE];
	if (!s[sz-1])
		return s;
	memcpy(buf, s, sz);
	buf[sz] = '\0';
	return buf;
}

static int dump_ustar_header(FILE *fp, uint64_t *sz, enum archive_type atype)
{
	struct ustar_header hdr;
	unsigned long long mode, uid, gid, size, mtime, devmajor, devminor;
	if (fread(&hdr, sizeof(hdr), 1, fp) != sizeof(hdr)) {
		if (feof(fp))
			die("unexpected EOF");
		else
			die("short read");
	}
	if (!memcmp(&hdr, all_zeros, sizeof(hdr)))
		return 0;

	verify_string(hdr.name, sizeof(hdr.name), "name");
	mode = verify_numeric(hdr.mode, sizeof(hdr.mode), "mode");
	uid = verify_numeric(hdr.uid, sizeof(hdr.uid), "uid");
	gid = verify_numeric(hdr.gid, sizeof(hdr.gid), "gid");
	size = verify_numeric(hdr.size, sizeof(hdr.size), "size");
	mtime = verify_numeric(hdr.mtime, sizeof(hdr.mtime), "mtime");
	verify_numeric(hdr.chksum, sizeof(hdr.chksum), "chksum");
	verify_string(hdr.linkname, sizeof(hdr.linkname), "linkname");
	verify_string(hdr.magic, sizeof(hdr.magic), "magic");
	verify_string(hdr.version, sizeof(hdr.version), "version");
	verify_string(hdr.uname, sizeof(hdr.uname), "uname");
	verify_string(hdr.gname, sizeof(hdr.gname), "gname");
	devmajor = verify_numeric(hdr.devmajor, sizeof(hdr.devmajor), "devmajor");
	devminor = verify_numeric(hdr.devminor, sizeof(hdr.devminor), "devminor");
	verify_string(hdr.prefix, sizeof(hdr.prefix), "prefix");

	if (hdr.typeflag[0] == 0)
		die("invalid NUL typeflag");
	if (uid != 0 || gid != 0)
		die("unexpected ID");
	if (devmajor != 0 || devminor != 0)
		die("unexpected devmajor or devminor");
	if (strcmp(hdr.uname, "root") || strcmp(hdr.gname, "root"))
		die("unexpected user or group name");
	if (strcmp(hdr.magic, "ustar"))
		die("invalid magic");
	if (memcmp(hdr.version, "00", 2))
		die("invalid version");
	if (canonical_version(atype) == 1 && mtime != 1)
		die("invalid mtime for ctar-v1*");

	*sz = size;

	printf("ustar:\n\tname=%s\n", dump_ustar_string(hdr.name, sizeof(hdr.name)));
	printf("\tmode=%04llo\tsize=%llu\n\tmtime=%llu\n\tlinkname=%s\n", mode,
	       size, mtime, dump_ustar_string(hdr.linkname, sizeof(hdr.linkname)));
	printf("\tprefix=%s\n", dump_ustar_string(hdr.prefix, sizeof(hdr.prefix)));

	return hdr.typeflag[0];
}

struct pax_header_entry {
	size_t len;
	const char *key;
	const char *value;
	struct pax_header_entry *next;
};

static void free_pax_header_entry(struct pax_header_entry *p)
{
	for (; p;) {
		struct pax_header_entry *next = p->next;
		free(p);
		p = next;
	}
}

/*
 * Parse the pax headers from buf and sz into a linked list.
 *
 * This overwrites the buffer and the linked list refers to its memory.
 */
static struct pax_header_entry *parse_pax_headers(char *buf, uint64_t sz)
{
	unsigned long len;
	char *p, *endptr;
	struct pax_header_entry *phe = NULL, *phe_end = NULL;
	if (sz > INT_MAX)
		die("too much data in pax header");
	while (sz > 0) {
		/*
		 * Each header looks like so:
		 *
		 * "%d %s=%s\n", len, key, value
		 *
		 * Note that the value may contain embedded newlines or any
		 * non-NUL byte.
		 */
		struct pax_header_entry *cur = calloc(1, sizeof(struct pax_header_entry));
		if (!phe_end)
			phe = cur;
		else
			phe_end->next = cur;
		phe_end = cur;
		p = memchr(buf, ' ', sz);
		if (!p)
			die("missing space in pax header");
		*p = '\0';
		len = strtoul(buf, &endptr, 10);
		if (!*buf || *endptr)
			die("invalid length");
		cur->len = len;
		if (len > sz || buf[len - 1] != '\n')
			die("invalid header line");
		buf[len - 1] = '\0';
		p = strchr(endptr + 1, '=');
		if (!p)
			die("missing equals sign");
		*p = '\0';
		cur->key = endptr + 1;
		cur->value = p + 1;
		sz -= len;
		buf += len;
	}
	return phe;
}

/*
 * Is this a power of 10 greater than or equal to 10?
 */
static int is_nonzero_power_of_10(size_t n)
{
	if (n <= 1)
		return 0;
	while (n >= 10) {
		if (n % 10)
			return 0;
		n /= 10;
	}
	return n == 1;
}

static int dump_pax_header(FILE *fp, uint64_t sz, uint64_t *outsz, enum archive_type atype)
{
	/* Round up to the nearest full block. */
	uint64_t chunksz = ((sz + TAR_BLOCK_SIZE - 1) / TAR_BLOCK_SIZE), i;
	struct pax_header_entry *phe, *phe_s, *prev = NULL;
	char *buf = malloc(chunksz);
	int seen_sz = 0;

	/*
	 * We only need sz bytes to parse, but we also need to read any trailing
	 * data from the file descriptor and we need to verify the padding, if
	 * any.
	 */
	if (fread(buf, 1, chunksz, fp) != chunksz)
		die("short read");
	for (i = sz; i < chunksz; i++)
		if (!buf[i])
			die("non-NUL padding in pax header");
	phe_s = phe = parse_pax_headers(buf, sz);
	printf("pax:\n");
	for (; phe; prev = phe, phe = phe->next) {
		printf("(%lu) %s=%s\n", (unsigned long)phe->len, phe->key, phe->value);
		if (canonical_version(atype)) {
			/*
			 * The length in the pax header encodes its own length
			 * as well. Thus, if the key and value lengths together
			 * equal (10^k)-3-k for integral k, the value can be
			 * encoded as all nines (e.g., 99) or as the next
			 * integer (e.g., 100), which is a power of 10.  This is
			 * a wart in the pax format specification.
			 *
			 * We require in canonical form that the encoding always
			 * be the shortest possible value, so the all-nines
			 * value must be used here; if we get the power of 10,
			 * we should fail.
			 */
			if (is_nonzero_power_of_10(phe->len))
				die("pax header length is not shortest form");
			if (prev && strcmp(prev->key, phe->key) >= 0)
				die("pax headers are not in sorted order");
		}
		if (outsz && !strcmp(prev->key, "size")) {
			char *p;
			*outsz = (uint64_t)strtoull(prev->value, &p, 10);
			if (*p)
				die("invalid size pax header");
			seen_sz = 1;
		}
	}
	free_pax_header_entry(phe_s);
	return seen_sz;
}

static void dump_ustar_body(FILE *fp, uint64_t sz)
{
	char buf[TAR_BLOCK_SIZE];
	unsigned char hash[GIT_MAX_RAWSZ];
	uint64_t chunksz = ((sz + TAR_BLOCK_SIZE - 1) / TAR_BLOCK_SIZE), i, j;
	const struct git_hash_algo *algo = &hash_algos[GIT_HASH_SHA256];
	git_hash_ctx ctx;

	algo->init_fn(&ctx);

	for (i = 0; i < chunksz; i += TAR_BLOCK_SIZE) {
		uint64_t data_left = ((sz - i) > TAR_BLOCK_SIZE) ? TAR_BLOCK_SIZE : (sz - i);
		if (fread(buf, 1, sizeof(buf), fp) != sizeof(buf))
			die("short read");
		for (j = data_left; j < TAR_BLOCK_SIZE; j++)
			if (buf[j])
				die("non-NUL padding in ustar body");
		algo->update_fn(&ctx, buf, data_left);
	}
	algo->final_fn(hash, &ctx);
	printf("ustar body:\n%s hash: %s\n", algo->name, hash_to_hex_algop(hash, algo));
}

enum state {
	START,
	SEEN_GLOBAL_HEADER,
	SEEN_FILE_PAX_HEADER,
	SEEN_FILE_USTAR_HEADER,
	SEEN_FIRST_ZERO,
};

static void dump_pax(FILE *fp, enum archive_type atype)
{
	enum state st = START;
	uint64_t datasz;

	while (1) {
		uint64_t sz;
		int type = dump_ustar_header(fp, &sz, atype);
		switch (st) {
		case START:
			if (type == 'g') {
				dump_pax_header(fp, sz, NULL, atype);
				st = SEEN_GLOBAL_HEADER;
			}
			else
				die("missing global header");
			break;
		case SEEN_GLOBAL_HEADER:
		case SEEN_FILE_USTAR_HEADER:
			if (type == 'x') {
				datasz = sz;
				if (!dump_pax_header(fp, sz, &datasz, atype) && canonical_version(atype))
					die("no size header in pax data");
				st = SEEN_FILE_PAX_HEADER;
			} else if (!canonical_version(atype) && (type == '0' || type == '2' || type == '5')) {
				dump_ustar_body(fp, sz);
				st = SEEN_FILE_USTAR_HEADER;
			} else if (type == 0) {
				st = SEEN_FIRST_ZERO;
			} else {
				die("unexpected type %d looking for pax file header", type);
			}
		case SEEN_FILE_PAX_HEADER:
			if ((type == '0' || type == '2' || type == '5')) {
				dump_ustar_body(fp, datasz);
				st = SEEN_FILE_USTAR_HEADER;
			} else {
				die("unexpected type %d looking for ustar file header", type);
			}
		case SEEN_FIRST_ZERO:
			if (type == 0)
				break;
			else
				die("unexpected type %d looking for last zero block", type);
		}
	}
}

static void test_is_nonzero_power_of_10(void)
{
	struct {
		unsigned long n;
		int result;
	} data[] = {
		{0, 0},
		{1, 0},
		{9, 0},
		{10, 1},
		{13, 0},
		{20, 0},
		{22, 0},
		{99, 0},
		{100, 1},
		{101, 0},
		{104, 0},
		{110, 0},
		{500, 0},
		{504, 0},
		{1000, 1},
	};
	size_t i;
	for (i = 0; i < ARRAY_SIZE(data); i++)
		if (data[i].result != is_nonzero_power_of_10((size_t)data[i].n))
			die("is_nonzero_power_of_10(%lu) != %d", data[i].n, data[i].result);
}

int cmd__dump_tar(int ac, const char **av)
{
	const char *format;
	enum archive_type atype = TYPE_DEFAULT;
	FILE *fp;

	/* Let's unit-test some of our important code here. */
	test_is_nonzero_power_of_10();

	if (ac < 3)
		die("usage: dump-tar --format=FORMAT FILE");

	if (!skip_prefix(av[1], "--format=", &format))
		die("usage: dump-tar --format=FORMAT FILE");

	if (!strcmp(format, "ctar-v1"))
		atype = TYPE_CTAR_V1;
	else if (!strcmp(format, "ctar-v1-t"))
		atype = TYPE_CTAR_V1_T;
	else if (strcmp(format, "default"))
		die("unknown format");

	if (!strcmp(av[2], "-"))
		fp = stdin;
	else {
		fp = fopen(av[2], "rb");
		if (!fp)
			die("can't open file");
	}

	dump_pax(fp, atype);
	return 0;
}
