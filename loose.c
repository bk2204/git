#include "cache.h"
#include "hash.h"
#include "loose.h"
#include "lockfile.h"
#include "object-store.h"
#include "packfile.h"

static const char *loose_object_header = "# loose-object-idx\n";

static inline int should_use_loose_object_map(struct repository *repo)
{
	return repo->compat_hash_algo && repo->gitdir;
}

void loose_object_map_init(struct loose_object_map **map)
{
	struct loose_object_map *m;
	m = xmalloc(sizeof(**map));
	m->to_compat = kh_init_oid_map();
	m->to_storage = kh_init_oid_map();
	*map = m;
}

static int insert_oid_pair(kh_oid_map_t *map, const struct object_id *key, const struct object_id *value)
{
	khiter_t pos;
	int ret;
	struct object_id *stored;

	pos = kh_put_oid_map(map, *key, &ret);

	/* This item already exists in the map. */
	if (ret == 0)
		return 0;

	stored = xmalloc(sizeof(*stored));
	oidcpy(stored, value);
	kh_value(map, pos) = stored;
	return 1;
}

static void find_loose_object_map(struct repository *repo, struct object_directory *dir, struct strbuf *buf, int flags)
{
	switch (flags & LOOSE_INDEX_MASK) {
	case LOOSE_INDEX_LOOSE:
		if (dir)
			strbuf_addf(buf, "%s/loose-object-idx", dir->path);
		else
			strbuf_git_common_path(buf, repo, "objects/loose-object-idx");
		break;
	case LOOSE_INDEX_SUBMODULE:
		if (dir)
			strbuf_addf(buf, "%s/submodule-object-idx", dir->path);
		else
			strbuf_git_common_path(buf, repo, "objects/submodule-object-idx");
		break;
	}
}

static int load_one_loose_object_map(struct repository *repo, struct object_directory *dir, int flags)
{
	struct strbuf buf = STRBUF_INIT, path = STRBUF_INIT;
	FILE *fp;

	if (!dir->loose_map)
		loose_object_map_init(&dir->loose_map);

	insert_oid_pair(dir->loose_map->to_compat, repo->hash_algo->empty_tree, repo->compat_hash_algo->empty_tree);
	insert_oid_pair(dir->loose_map->to_storage, repo->compat_hash_algo->empty_tree, repo->hash_algo->empty_tree);

	insert_oid_pair(dir->loose_map->to_compat, repo->hash_algo->empty_blob, repo->compat_hash_algo->empty_blob);
	insert_oid_pair(dir->loose_map->to_storage, repo->compat_hash_algo->empty_blob, repo->hash_algo->empty_blob);

	insert_oid_pair(dir->loose_map->to_compat, repo->hash_algo->null_oid, repo->compat_hash_algo->null_oid);
	insert_oid_pair(dir->loose_map->to_storage, repo->compat_hash_algo->null_oid, repo->hash_algo->null_oid);

	find_loose_object_map(repo, dir, &path, flags);
	fp = fopen(path.buf, "rb");
	if (!fp)
		return 0;

	errno = 0;
	if (strbuf_getwholeline(&buf, fp, '\n') || strcmp(buf.buf, loose_object_header))
		goto err;
	while (!strbuf_getline_lf(&buf, fp)) {
		const char *p;
		struct object_id oid, compat_oid;
		if (parse_oid_hex_algop(buf.buf, &oid, &p, repo->hash_algo) ||
		    *p++ != ' ' ||
		    parse_oid_hex_algop(p, &compat_oid, &p, repo->compat_hash_algo) ||
		    p != buf.buf + buf.len)
			goto err;
		insert_oid_pair(dir->loose_map->to_compat, &oid, &compat_oid);
		insert_oid_pair(dir->loose_map->to_storage, &compat_oid, &oid);
	}

	strbuf_release(&buf);
	strbuf_release(&path);
	return errno ? -1 : 0;
err:
	strbuf_release(&buf);
	strbuf_release(&path);
	return -1;
}

int repo_read_loose_object_map(struct repository *repo)
{
	struct object_directory *dir;

	if (!should_use_loose_object_map(repo))
		return 0;

	prepare_alt_odb(repo);

	for (dir = repo->objects->odb; dir; dir = dir->next) {
		if (load_one_loose_object_map(repo, dir, LOOSE_INDEX_LOOSE) < 0) {
			return -1;
		}
		if (load_one_loose_object_map(repo, dir, LOOSE_INDEX_SUBMODULE) < 0) {
			return -1;
		}
	}
	return 0;
}

int repo_write_loose_object_map(struct repository *repo, int flags)
{
	kh_oid_map_t *map = repo->objects->odb->loose_map->to_compat;
	struct lock_file lock;
	int fd;
	khiter_t iter;
	struct strbuf buf = STRBUF_INIT, path = STRBUF_INIT;

	if (!should_use_loose_object_map(repo))
		return 0;

	find_loose_object_map(repo, NULL, &path, LOOSE_INDEX_LOOSE);
	fd = hold_lock_file_for_update_timeout(&lock, path.buf, LOCK_DIE_ON_ERROR, -1);
	iter = kh_begin(map);
	if (write_in_full(fd, loose_object_header, strlen(loose_object_header)) < 0)
		goto errout;

	for (; iter != kh_end(map); iter++) {
		if (kh_exist(map, iter)) {
			if (oideq(&kh_key(map, iter), the_hash_algo->empty_tree) ||
			    oideq(&kh_key(map, iter), the_hash_algo->empty_blob))
				continue;
			strbuf_addf(&buf, "%s %s\n", oid_to_hex(&kh_key(map, iter)), oid_to_hex(kh_value(map, iter)));
			if (write_in_full(fd, buf.buf, buf.len) < 0)
				goto errout;
			strbuf_reset(&buf);
		}
	}
	strbuf_release(&buf);
	if (commit_lock_file(&lock) < 0) {
		error_errno(_("could not write loose object index %s"), path.buf);
		strbuf_release(&path);
		return -1;
	}
	strbuf_release(&path);
	return 0;
errout:
	rollback_lock_file(&lock);
	strbuf_release(&buf);
	error_errno(_("failed to write loose object index %s\n"), path.buf);
	strbuf_release(&path);
	return -1;
}

static int write_one_object(struct repository *repo, const struct object_id *oid,
			    const struct object_id *compat_oid, int flags)
{
	struct lock_file lock;
	int fd;
	struct stat st;
	struct strbuf buf = STRBUF_INIT, path = STRBUF_INIT;

	find_loose_object_map(repo, NULL, &path, flags);
	hold_lock_file_for_update_timeout(&lock, path.buf, LOCK_DIE_ON_ERROR, -1);

	fd = open(path.buf, O_WRONLY | O_CREAT | O_APPEND, 0666);
	if (fd < 0)
		goto errout;
	if (fstat(fd, &st) < 0)
		goto errout;
	if (!st.st_size && write_in_full(fd, loose_object_header, strlen(loose_object_header)) < 0)
		goto errout;

	strbuf_addf(&buf, "%s %s\n", oid_to_hex(oid), oid_to_hex(compat_oid));
	if (write_in_full(fd, buf.buf, buf.len) < 0)
		goto errout;
	if (close(fd))
		goto errout;
	adjust_shared_perm(path.buf);
	rollback_lock_file(&lock);
	strbuf_release(&buf);
	strbuf_release(&path);
	return 0;
errout:
	error_errno(_("failed to write loose object index %s\n"), path.buf);
	close(fd);
	rollback_lock_file(&lock);
	strbuf_release(&buf);
	strbuf_release(&path);
	return -1;
}

int repo_add_loose_object_map(struct repository *repo, const struct object_id *oid,
			      const struct object_id *compat_oid, int flags)
{
	int inserted = 0;

	if (!should_use_loose_object_map(repo))
		return 0;

	inserted |= insert_oid_pair(repo->objects->odb->loose_map->to_compat, oid, compat_oid);
	inserted |= insert_oid_pair(repo->objects->odb->loose_map->to_storage, compat_oid, oid);
	if (inserted && (flags & LOOSE_WRITE))
		return write_one_object(repo, oid, compat_oid, flags);
	return 0;
}

static int repo_map_packed_object(struct repository *repo, struct object_id *dest,
		    const struct git_hash_algo *dest_algo, const struct object_id *src)
{
	struct packed_git *p;
	for (p = get_packed_git(repo); p; p = p->next) {
		uint32_t off;
		if (!bsearch_pack(src, p, &off))
			continue;
		return nth_packed_object_id_algop(dest, p, off, dest_algo);
	}
	return -1;
}

int repo_map_object(struct repository *repo, struct object_id *dest,
		    const struct git_hash_algo *dest_algo, const struct object_id *src)
{
	struct object_directory *dir;
	int retried = 0;
	kh_oid_map_t *map;
	khiter_t pos;

	/*
	 * If the destination algorithm is NULL, then we're probably trying to
	 * convert to a nonexistent compatibility object format.  If the source
	 * algorithm is not set, then we're using the default hash algorithm for
	 * that object.  If it looks like no conversion is needed, either
	 * because no compatibility object format is set or because the two
	 * algorithms are the same, then simply copy the object unless the
	 * source and destination are the same (because we use memcpy).
	 */
	if (!dest_algo || (!src->algo && dest_algo == repo->hash_algo) ||
	    &hash_algos[src->algo] == dest_algo) {
		if (src != dest)
			oidcpy(dest, src);
		return 0;
	}

retry:
	for (dir = repo->objects->odb; dir; dir = dir->next) {
		map = dest_algo == repo->compat_hash_algo ? dir->loose_map->to_compat : dir->loose_map->to_storage;
		pos = kh_get_oid_map(map, *src);
		if (pos < kh_end(map)) {
			oidcpy(dest, kh_value(map, pos));
			return 0;
		}
	}
	if (!retried) {
		/*
		 * It's not in the loose object map, so let's see if it's in a
		 * pack.
		 */
		if (!repo_map_packed_object(repo, dest, dest_algo, src))
			return 0;
		/*
		 * We may have loaded the object map at repo initialization but
		 * another process (perhaps upstream of a pipe from us) may have
		 * written a new object into the map.  If the object is missing,
		 * let's reload the map to see if the object has appeared.
		 */
		repo_read_loose_object_map(repo);
		retried = 1;
		goto retry;
	}
	return -1;
}

void loose_object_map_clear(struct loose_object_map **map)
{
	struct loose_object_map *m = *map;
	struct object_id *oid;

	if (!m)
		return;

	kh_foreach_value(m->to_compat, oid, free(oid));
	kh_foreach_value(m->to_storage, oid, free(oid));
	kh_destroy_oid_map(m->to_compat);
	kh_destroy_oid_map(m->to_storage);
	free(m);
	*map = NULL;
}
