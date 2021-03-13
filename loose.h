#ifndef LOOSE_H
#define LOOSE_H

#include "khash.h"

struct repository;
struct odb_source;

struct loose_object_map {
	kh_oid_map_t *to_compat;
	kh_oid_map_t *to_storage;
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

void loose_object_map_init(struct loose_object_map **map);
void loose_object_map_clear(struct loose_object_map **map);
int repo_loose_object_map_oid(struct repository *repo,
			      const struct object_id *src,
			      const struct git_hash_algo *dest_algo,
			      struct object_id *dest);
int repo_add_loose_object_map(struct odb_source *source,
			      const struct object_id *oid,
			      const struct object_id *compat_oid, int flags);
int repo_read_loose_object_map(struct repository *repo);
int repo_write_loose_object_map(struct repository *repo, int flags);

#endif
