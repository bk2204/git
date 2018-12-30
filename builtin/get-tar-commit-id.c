/*
 * Copyright (c) 2005, 2006 Rene Scharfe
 */
#include "cache.h"
#include "commit.h"
#include "tar.h"
#include "builtin.h"
#include "strbuf.h"
#include "quote.h"

static const char builtin_get_tar_commit_id_usage[] =
"git get-tar-commit-id";

/* ustar header + extended global header content */
#define RECORDSIZE	(512)
#define HEADERSIZE (2 * RECORDSIZE)

int cmd_get_tar_commit_id(int argc, const char **argv, const char *prefix)
{
	char buffer[HEADERSIZE];
	struct ustar_header *header = (struct ustar_header *)buffer;
	char *content = buffer + RECORDSIZE;
	const char *comment;
	ssize_t n;
	char *hdrprefix;
	int ret;
	int i;

	if (argc != 1)
		usage(builtin_get_tar_commit_id_usage);

	n = read_in_full(0, buffer, HEADERSIZE);
	if (n < 0)
		die_errno("git get-tar-commit-id: read error");
	if (n != HEADERSIZE)
		die_errno("git get-tar-commit-id: EOF before reading tar header");
	if (header->typeflag[0] != 'g')
		return 1;

	for (i = 1; i < GIT_HASH_NALGOS; i++) {
		const struct git_hash_algo *algo = &hash_algos[i];
		hdrprefix = xstrfmt("%zu comment=", algo->hexsz + strlen(" comment=") + 2 + 1);
		ret = skip_prefix(content, hdrprefix, &comment);
		free(hdrprefix);
		if (!ret)
			continue;

		if (write_in_full(1, comment, algo->hexsz + 1) < 0)
			die_errno("git get-tar-commit-id: write error");
		return 0;
	}

	return 1;
}
