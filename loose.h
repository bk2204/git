#ifndef LOOSE_H
#define LOOSE_H

#include "khash.h"

struct repository;
struct odb_source;

struct loose_object_map {
	void *ptr;
};

struct loose_object_map_bin_entry {
	char *name;
	int fd;
	void *mem;
	size_t size;
	void *ptr;
	struct loose_object_map_bin_entry *next;
};

struct loose_object_map_bin {
	void *hashmap;
	struct loose_object_map_bin_entry *entries;
};

/* Should we write this object to disk? */
#define LOOSE_WRITE		(1 << 6)
/*
 * This is an internal, reserved value that does not appear on disk.
 * This value is only used for values like the null OID, the empty blob, and the
 * empty tree.
 */
#define LOOSE_TYPE_RESERVED	0
/* This is a loose object. */
#define LOOSE_TYPE_LOOSE	1
/* This is a shallow, its parent, or its tree. */
#define LOOSE_TYPE_SHALLOW	2
/* This is a submodule. */
#define LOOSE_TYPE_SUBMODULE	3
#define LOOSE_TYPE_MASK		0x3f

void loose_object_map_init(struct loose_object_map **map, uint32_t algo,
			   uint32_t compat_algo);
void loose_object_map_clear(struct loose_object_map **map);
int repo_loose_object_map_oid(struct repository *repo,
			      const struct object_id *src,
			      const struct git_hash_algo *dest_algo,
			      struct object_id *dest);
int repo_add_loose_object_map(struct odb_source *source,
			      const struct object_id *oid,
			      const struct object_id *compat_oid, int flags);
int repo_read_loose_object_map(struct repository *repo);
int repo_clear_loose_object_map(struct repository *repo);

bool repo_loose_object_map_has_batch(struct odb_source *source);
void repo_loose_object_map_start_batch(struct odb_source *source);
int repo_loose_object_map_finish_batch(struct odb_source *source, bool noop_ok, char **file);
int64_t repo_loose_object_map_batch_len(struct odb_source *source);

void loose_object_map_bin_init(struct loose_object_map_bin **map);
void loose_object_map_bin_clear(struct loose_object_map_bin **map);

typedef void each_file_in_loose_map_fn(const char *full_path, size_t full_path_len,
				       const char *file_name, void *data);

typedef int loose_object_map_bin_for_each_fn(const struct object_id *main, const struct object_id *compat, uint32_t, void *data);

#ifdef WITH_RUST
int loose_object_map_bin_for_each(struct loose_object_map_bin_entry *entry,
				  loose_object_map_bin_for_each_fn fn, void *data);
void loose_object_map_bin_entry_clear(struct loose_object_map_bin_entry **ent);
#endif
void loose_object_map_bin_entry_clear(struct loose_object_map_bin_entry **ent);

#endif
