#define USE_THE_REPOSITORY_VARIABLE
#define DISABLE_SIGN_COMPARE_WARNINGS

#include "builtin.h"
#include "gettext.h"
#include "hash.h"
#include "hex.h"
#include "pack.h"
#include "parse-options.h"

static const char *const show_index_usage[] = {
	"git show-index [--object-format=<hash-algorithm>] < <pack-idx-file>",
	NULL
};

struct hash_format {
	const struct git_hash_algo *algo;
	uint32_t abbrev_len;
	uint64_t offset;
};

struct entry {
	struct object_id oid;
	uint32_t crc;
	uint64_t off;
};

static void discard_bytes(uint64_t to_discard)
{
	char discard[256];
	while (to_discard) {
		size_t chunk = to_discard > sizeof(discard) ? sizeof(discard) : to_discard;
		if (fread(discard, chunk, 1, stdin) != 1)
			die(_("unable to discard bytes"));
		to_discard -= chunk;
	}
}

static void read_common_data(struct entry *entries, unsigned nr, unsigned *off64_nr)
{
	int i;
	for (i = 0; i < nr; i++)
		if (fread(&entries[i].crc, 4, 1, stdin) != 1)
			die(_("unable to read crc %u/%u"), i, nr);
	for (i = 0; i < nr; i++)
		if (fread(&entries[i].off, 4, 1, stdin) != 1)
			die(_("unable to read 32b offset %u/%u"), i, nr);
	for (i = 0; i < nr; i++) {
		uint32_t off = ntohl(entries[i].off);
		if (!(off & 0x80000000)) {
			entries[i].off = off;
		} else {
			uint32_t off64[2];
			if ((off & 0x7fffffff) != *off64_nr)
				die(_("inconsistent 64b offset index"));
			if (fread(off64, 8, 1, stdin) != 1)
				die(_("unable to read 64b offset %u"), *off64_nr);
			entries[i].off = (((uint64_t)ntohl(off64[0])) << 32) |
						     ntohl(off64[1]);
			(*off64_nr)++;
		}
	}
}

int cmd_show_index(int argc,
		   const char **argv,
		   const char *prefix,
		   struct repository *repo UNUSED)
{
	int i;
	unsigned nr;
	unsigned int version;
	static unsigned int top_index[256];
	unsigned hashsz;
	unsigned nformats;
	struct hash_format *format = NULL, *cur_format = NULL;
	const char *hash_name = NULL;
	int hash_algo;
	const struct option show_index_options[] = {
		OPT_STRING(0, "object-format", &hash_name, N_("hash-algorithm"),
			   N_("specify the hash algorithm to use")),
		OPT_END()
	};

	argc = parse_options(argc, argv, prefix, show_index_options, show_index_usage, 0);

	if (hash_name) {
		hash_algo = hash_algo_by_name(hash_name);
		if (hash_algo == GIT_HASH_UNKNOWN)
			die(_("Unknown hash algorithm"));
		repo_set_hash_algo(the_repository, hash_algo);
	}

	/*
	 * Fallback to SHA1 if we are running outside of a repository.
	 *
	 * TODO: If a future implementation of index file version encodes the hash
	 *       algorithm in its header, enable show-index to infer it from the
	 *       header rather than relying on repository context or a default fallback.
	 */
	if (!the_hash_algo) {
		warning(_("assuming SHA-1; use --object-format to override"));
		repo_set_hash_algo(the_repository, GIT_HASH_DEFAULT);
	}

	hashsz = the_hash_algo->rawsz;

	if (fread(top_index, 2 * 4, 1, stdin) != 1)
		die(_("unable to read header"));
	if (top_index[0] == htonl(PACK_IDX_SIGNATURE)) {
		version = ntohl(top_index[1]);
		if (version < 2 || version > 3)
			die(_("unknown index version"));
		if (version == 2) {
			if (fread(top_index, 256 * 4, 1, stdin) != 1)
				die(_("unable to read index"));
		} else if (version == 3) {
			if (fread(top_index, 3 * 4, 1, stdin) != 1)
				die(_("unable to read header"));
			nr = ntohl(top_index[1]);
			nformats = ntohl(top_index[2]);
			ALLOC_ARRAY(format, nformats);
			for (int i = 0; i < nformats; i++) {
				int algo;
				if (fread(top_index, 4 * 4, 1, stdin) != 1)
					die("unable to read header object format tables");
				algo = hash_algo_by_id(ntohl(top_index[0]));
				if (algo == GIT_HASH_UNKNOWN)
					die("unknown hash algorithm id %08x", ntohl(top_index[0]));
				format[i].algo = &hash_algos[algo];
				format[i].abbrev_len = ntohl(top_index[1]);
				format[i].offset = (((uint64_t)ntohl(top_index[2])) << 32) | ntohl(top_index[3]);
				if (the_hash_algo == format[i].algo)
					cur_format = &format[i];
			}
			if (!cur_format)
				die("specified hash algorithm not in this index");
			discard_bytes(st_add(format[0].offset,
					     st_mult(format[0].abbrev_len, nr)) -
				      ((4 * 5) + st_mult(nformats, 16)));
		}
	} else {
		version = 1;
		if (fread(&top_index[2], 254 * 4, 1, stdin) != 1)
			die(_("unable to read index"));
	}
	if (version <= 2) {
		nr = 0;
		for (i = 0; i < 256; i++) {
			unsigned n = ntohl(top_index[i]);
			if (n < nr)
				die(_("corrupt index file"));
			nr = n;
		}
	}
	if (version == 1) {
		for (i = 0; i < nr; i++) {
			unsigned int offset, entry[(GIT_MAX_RAWSZ + 4) / sizeof(unsigned int)];

			if (fread(entry, 4 + hashsz, 1, stdin) != 1)
				die(_("unable to read entry %u/%u"), i, nr);
			offset = ntohl(entry[0]);
			printf("%u %s\n", offset, hash_to_hex((void *)(entry+1)));
		}
	} else if (version == 2) {
		struct entry *entries;
		unsigned off64_nr = 0;
		ALLOC_ARRAY(entries, nr);
		for (i = 0; i < nr; i++) {
			if (fread(entries[i].oid.hash, hashsz, 1, stdin) != 1)
				die(_("unable to read sha1 %u/%u"), i, nr);
			entries[i].oid.algo = hash_algo_by_ptr(the_hash_algo);
		}
		read_common_data(entries, nr, &off64_nr);
		for (i = 0; i < nr; i++) {
			printf("%" PRIuMAX " %s (%08"PRIx32")\n",
			       (uintmax_t) entries[i].off,
			       oid_to_hex(&entries[i].oid),
			       ntohl(entries[i].crc));
		}
		free(entries);
	} else {
		struct entry *entries;
		unsigned off64_nr = 0;
		ALLOC_ARRAY(entries, nr);
		if (cur_format == &format[0]) {
			for (i = 0; i < nr; i++) {
				if (fread(entries[i].oid.hash, format[0].algo->rawsz, 1, stdin) != 1)
					die("unable to read %s %u/%u", format[0].algo->name, i, nr);
				entries[i].oid.algo = hash_algo_by_ptr(format[0].algo);
			}
		} else {
			discard_bytes((uint64_t)nr * format[0].algo->rawsz);
		}
		discard_bytes((uint64_t)nr * 4);
		read_common_data(entries, nr, &off64_nr);
		if (cur_format == &format[1]) {
			discard_bytes(format[1].offset + (format[1].abbrev_len * nr) -
				      ((4 + 4 + 4) * nr) - (8 * off64_nr) - format[0].offset);
			for (i = 0; i < nr; i++) {
				if (fread(entries[i].oid.hash, format[0].algo->rawsz, 1, stdin) != 1)
					die("unable to read %s %u/%u", format[1].algo->name, i, nr);
				entries[i].oid.algo = hash_algo_by_ptr(format[1].algo);
			}
		}
		for (i = 0; i < nr; i++) {
			printf("%" PRIuMAX " %s (%08"PRIx32")\n",
			       (uintmax_t) entries[i].off,
			       oid_to_hex(&entries[i].oid),
			       ntohl(entries[i].crc));
		}
		free(entries);
	}
	free(format);
	return 0;
}
