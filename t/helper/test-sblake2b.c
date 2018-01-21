#include "cache.h"

int cmd__sblake2b(int ac, const char **av)
{
	git_hash_ctx ctx;
	unsigned char hash[GIT_MAX_RAWSZ];
	unsigned bufsz = 8192;
	int binary = 0;
	char *buffer;

	if (ac == 2) {
		if (!strcmp(av[1], "-b"))
			binary = 1;
		else
			bufsz = strtoul(av[1], NULL, 10) * 1024 * 1024;
	}

	if (!bufsz)
		bufsz = 8192;

	while ((buffer = malloc(bufsz)) == NULL) {
		fprintf(stderr, "bufsz %u is too big, halving...\n", bufsz);
		bufsz /= 2;
		if (bufsz < 1024)
			die("OOPS");
	}

	hash_algos[GIT_HASH_SBLAKE2].init_fn(&ctx);

	while (1) {
		ssize_t sz, this_sz;
		char *cp = buffer;
		unsigned room = bufsz;
		this_sz = 0;
		while (room) {
			sz = xread(0, cp, room);
			if (sz == 0)
				break;
			if (sz < 0)
				die_errno("test-sha1");
			this_sz += sz;
			cp += sz;
			room -= sz;
		}
		if (this_sz == 0)
			break;
		hash_algos[GIT_HASH_SBLAKE2].update_fn(&ctx, buffer, this_sz);
	}
	hash_algos[GIT_HASH_SBLAKE2].final_fn(hash, &ctx);

	if (binary)
		fwrite(hash, 1, hash_algos[GIT_HASH_SBLAKE2].rawsz, stdout);
	else
		puts(sha1_to_hex(hash));
	exit(0);
}
