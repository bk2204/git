#ifndef LOOSE_H
#define LOOSE_H

#include "cache.h"
#include "khash.h"

struct loose_object_map {
	kh_oid_map_t *to_compat;
	kh_oid_map_t *to_storage;
};

/* Should we write this object to disk? */
#define LOOSE_WRITE		(1 << 0)
/* Write this object to the loose object index. */
#define LOOSE_INDEX_LOOSE	(0 << 1)
/* Write this object to the submodule object index. */
#define LOOSE_INDEX_SUBMODULE	(1 << 1)
#define LOOSE_INDEX_MASK	(1 << 1)

void loose_object_map_init(struct loose_object_map **map);
void loose_object_map_clear(struct loose_object_map **map);
int repo_map_object(struct repository *repo, struct object_id *dest,
		    const struct git_hash_algo *dest_algo, const struct object_id *src);
int repo_add_loose_object_map(struct repository *repo, const struct object_id *oid,
			      const struct object_id *compat_oid, int flags);
int repo_read_loose_object_map(struct repository *repo);
int repo_write_loose_object_map(struct repository *repo, int flags);

#endif
