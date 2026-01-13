#include "git-compat-util.h"
#include "hash.h"
#include "dir.h"
#include "path.h"
#include "object-file.h"
#include "odb.h"
#include "odb/source-files.h"
#include "hex.h"
#include "repository.h"
#include "wrapper.h"
#include "gettext.h"
#include "loose.h"
#include "lockfile.h"
#include "oidtree.h"
#include "packfile.h"
#include "write-or-die.h"
#include "csum-file.h"
#include "object-file.h"

#ifdef WITH_RUST
int repo_loose_object_map_oid_1(const void *map, const void *src,
				uint32_t to, const void *dest);
int repo_add_loose_object_map_1(void *map, const struct object_id *oid1,
				const struct object_id *oid2,
				uint32_t kind, bool write);
int repo_loose_object_map_write_batch(void *map, int fd,
				      const char *name, uint8_t *csum,
				      uint32_t component,
				      uint32_t flags);
bool repo_loose_object_map_has_batch_1(const void *map);
void repo_loose_object_map_start_batch_1(void *map);
void repo_loose_object_map_abort_batch_1(void *map);
int64_t repo_loose_object_map_batch_len_1(const void *map);

int loose_object_map_bin_init_1(void **map, const uint8_t *buf,
				size_t len, uint32_t storage);
int loose_object_map_oid_bin_1(const void *map, const void *src,
			       uint32_t to, const void *dest);
void loose_object_map_bin_clear_1(void **map);
int loose_object_map_bin_for_each_1(void *map, loose_object_map_bin_for_each_fn fn, void *data);

void loose_object_map_bin_hashmap_init(void **hashmap);
void loose_object_map_bin_hashmap_insert(void *hashmap, const char *s);
bool loose_object_map_bin_hashmap_contains(void *hashmap, const char *s);
void loose_object_map_bin_hashmap_clear(void **hashmap);

static void for_each_file_in_loose_map_dir(const char *objdir,
					   each_file_in_pack_dir_fn fn,
					   void *data)
{
	struct strbuf path = STRBUF_INIT;
	size_t dirnamelen;
	DIR *dir;
	struct dirent *de;

	strbuf_addstr(&path, objdir);
	strbuf_addstr(&path, "/object-map");
	dir = opendir(path.buf);
	if (!dir) {
		if (errno != ENOENT)
			error_errno("unable to open object pack directory: %s",
				    path.buf);
		strbuf_release(&path);
		return;
	}
	strbuf_addch(&path, '/');
	dirnamelen = path.len;
	while ((de = readdir_skip_dot_and_dotdot(dir)) != NULL) {
		strbuf_setlen(&path, dirnamelen);
		strbuf_addstr(&path, de->d_name);

		fn(path.buf, path.len, de->d_name, data);
	}

	closedir(dir);
	strbuf_release(&path);
}

int64_t repo_loose_object_map_batch_len(struct odb_source *source)
{
	struct odb_source_files *files = odb_source_files_downcast(source);

	return repo_loose_object_map_batch_len_1(files->loose->map);
}
#endif

void loose_object_map_bin_init(struct loose_object_map_bin **map)
{
	struct loose_object_map_bin *m;

	m = xcalloc(1, sizeof(**map));
#ifdef WITH_RUST
	loose_object_map_bin_hashmap_init(&m->hashmap);
#endif

	*map = m;
}

void loose_object_map_bin_entry_clear(struct loose_object_map_bin_entry **ent)
{
	struct loose_object_map_bin_entry *entry;

	if (!ent || !*ent)
		return;

	entry = *ent;
	/*
	 * The Rust code holds onto the memory, so de-allocate the Rust
	 * objects before unmapping the memory.
	 */
#ifdef WITH_RUST
	loose_object_map_bin_clear_1(&entry->ptr);
#endif
	munmap(entry->mem, entry->size);
	free(entry->name);
	close(entry->fd);
	free(entry);

	*ent = NULL;
}

void loose_object_map_bin_clear(struct loose_object_map_bin **map)
{
	struct loose_object_map_bin *m = *map;

	if (!m)
		return;

#ifdef WITH_RUST
	loose_object_map_bin_hashmap_clear(&m->hashmap);
	for (struct loose_object_map_bin_entry *cur, *entry = m->entries; entry;) {
		cur = entry;
		entry = entry->next;
		loose_object_map_bin_entry_clear(&cur);
	}
#endif

	free(m);
	*map = NULL;
}

static inline int should_use_loose_object_map(struct repository *repo)
{
	return repo->compat_hash_algo && repo->gitdir;
}

#ifdef WITH_RUST
struct loose_object_map_data {
	struct repository *repo;
	struct odb_source *source;
};

static int insert_cached_objects(const struct object_id *main UNUSED,
				 const struct object_id *compat,
				 uint32_t kind UNUSED,
				 void *data)
{
	if (data)
		oidtree_insert(data, compat, NULL);
	return 0;
}

int loose_object_map_bin_for_each(struct loose_object_map_bin_entry *entry,
				  loose_object_map_bin_for_each_fn fn, void *data)
{
	return loose_object_map_bin_for_each_1(entry->ptr, fn, data);
}

static void prepare_loose_object_map_bin(const char *full_name,
					 size_t full_name_len,
					 const char *file_name UNUSED, void *data)
{
	struct loose_object_map_data *d = data;
	struct odb_source_files *files = odb_source_files_downcast(d->source);
	struct loose_object_map_bin *bin = files->loose->map_bin;
	struct loose_object_map_bin_entry *new = NULL, *entry = bin->entries;
	char *name = xmemdupz(full_name, full_name_len);
	struct stat st;

	if (loose_object_map_bin_hashmap_contains(bin->hashmap, name)) {
		free(name);
		return;
	}

	new = xcalloc(1, sizeof(*new));
	new->name = name;
	new->fd = git_open(name);
	if (new->fd < 0) {
		error(_("cannot open %s as loose object map"), full_name);
		goto out;
	}
	if (fstat(new->fd, &st)) {
		error(_("cannot fstat %s as loose object map"), full_name);
		goto out;
	}
	if ((uintmax_t)st.st_size >= (uintmax_t)SIZE_MAX) {
		error(_("loose object map %s is too large"), full_name);
		goto out;
	}
	new->size = st.st_size;
	new->mem = xmmap(NULL, new->size, PROT_READ, MAP_PRIVATE, new->fd, 0);
	if (loose_object_map_bin_init_1(&new->ptr, new->mem, new->size,
					hash_algo_by_ptr(d->repo->hash_algo))) {
		error(_("loose object map %s could not be loaded"), full_name);
		goto out;
	}

	loose_object_map_bin_for_each(new, insert_cached_objects, files->loose->cache);

	new->next = entry;
	bin->entries = new;
	loose_object_map_bin_hashmap_insert(bin->hashmap, name);
	return;
out:
	close(new->fd);
	free(new);
	free(name);
}

static void read_loose_object_map(struct odb_source *source)
{
	struct loose_object_map_data data = {
		.repo = source->odb->repo,
		.source = source,
	};
	for_each_file_in_loose_map_dir(source->path,
				       prepare_loose_object_map_bin,
				       &data);
}

static int load_one_loose_object_map(struct odb_source *source)
{
	struct odb_source_files *files = odb_source_files_downcast(source);
	struct repository *repo = source->odb->repo;

	if (!files->loose->map)
		loose_object_map_init(&files->loose->map,
				      hash_algo_by_ptr(repo->hash_algo),
				      hash_algo_by_ptr(repo->compat_hash_algo));
	if (!files->loose->map_bin)
		loose_object_map_bin_init(&files->loose->map_bin);
	if (!files->loose->cache) {
		ALLOC_ARRAY(files->loose->cache, 1);
		oidtree_init(files->loose->cache);
	}
	read_loose_object_map(source);
	return 0;
}

static void clear_one_loose_object_map(struct odb_source *source)
{
	struct odb_source_files *files = odb_source_files_downcast(source);
	if (!files->loose->map)
		loose_object_map_clear(&files->loose->map);
	if (!files->loose->map_bin)
		loose_object_map_bin_clear(&files->loose->map_bin);
	if (!files->loose->cache) {
		oidtree_clear(files->loose->cache);
		free(files->loose->cache);
	}
}
#endif

int repo_read_loose_object_map(struct repository *repo)
{
	if (!should_use_loose_object_map(repo))
		return 0;

	odb_prepare_alternates(repo->objects);

#ifdef WITH_RUST
	for (struct odb_source *source = repo->objects->sources; source; source = source->next)
		if (load_one_loose_object_map(source) < 0)
			return -1;
#endif
	return 0;
}

int repo_clear_loose_object_map(struct repository *repo)
{
	/* The ODB is not yet initialized; silently succeed. */
	if (!repo->objects)
		return 0;
#ifdef WITH_RUST
	for (struct odb_source *source = repo->objects->sources; source; source = source->next)
		clear_one_loose_object_map(source);
#endif
	return 0;
}

#ifdef WITH_RUST
static int repo_write_loose_object_map(struct repository *repo, void *map, char **file)
{
	int fd;
	struct strbuf path = STRBUF_INIT, final = STRBUF_INIT;
	uint8_t trailing_hash[GIT_MAX_RAWSZ];
	int ret = -1;

	repo_git_path_replace(repo, &path, "objects/object-map");
	if (safe_create_dir_in_gitdir(repo, path.buf) && errno != EEXIST) {
		error(_("could not create object-map directory"));
		goto out;
	}
	strbuf_reset(&path);
	fd = odb_mkstemp(repo->objects, &path, "object-map/tmp_map_XXXXXX");
	if (repo_loose_object_map_write_batch(map, fd, path.buf, trailing_hash,
					      FSYNC_COMPONENT_OBJECT_MAP,
					      CSUM_CLOSE | CSUM_FSYNC | CSUM_HASH_IN_STREAM)) {
		unlink(path.buf);
		goto out;
	}
	repo_git_path_replace(repo, &final, "objects/object-map/map-%s.map",
		    hash_to_hex_algop(trailing_hash, repo->hash_algo));

	if (finalize_object_file(repo, path.buf, final.buf))
		goto out;
	ret = 0;
out:
	strbuf_release(&path);
	if (file)
		*file = strbuf_detach(&final, NULL);
	else
		strbuf_release(&final);
	return ret;
}
#endif

int repo_add_loose_object_map(struct odb_source *source MAYBE_UNUSED,
			      const struct object_id *oid MAYBE_UNUSED,
			      const struct object_id *compat_oid MAYBE_UNUSED,
			      int flags MAYBE_UNUSED)
{
#ifdef WITH_RUST
	bool started_batch = false;
	struct repository *repo = source->odb->repo;
	struct odb_source_files *files = odb_source_files_downcast(source);

	if (!should_use_loose_object_map(repo))
		return 0;

	if (!files->loose->map)
		loose_object_map_init(&files->loose->map,
				      hash_algo_by_ptr(repo->hash_algo),
				      hash_algo_by_ptr(repo->compat_hash_algo));

	/*
	 * This case should not happen because it means either hash_algo or
	 * compat_hash_algo is invalid.
	 */
	if (!files->loose->map) {
		return error(_("loose object map could not be initialized"));
	}

	if ((flags & LOOSE_WRITE) && !repo_loose_object_map_has_batch(source)) {
		started_batch = true;
		repo_loose_object_map_start_batch(source);
	}

	if (repo_add_loose_object_map_1(files->loose->map, oid, compat_oid,
					flags & LOOSE_TYPE_MASK, flags & LOOSE_WRITE))
		return error(_("failed to insert object in loose object map"));

	if (started_batch && repo_loose_object_map_finish_batch(source, false, NULL)) {
		return error(_("failed to write loose object map"));
	}

	if (files->loose->cache && (flags & LOOSE_WRITE))
		oidtree_insert(files->loose->cache, compat_oid, NULL);
	return 0;
#else
	return -1;
#endif
}

bool repo_loose_object_map_has_batch(struct odb_source *source MAYBE_UNUSED)
{
#ifdef WITH_RUST
	struct odb_source_files *files = odb_source_files_downcast(source);

	return repo_loose_object_map_has_batch_1(files->loose->map);
#else
	return false;
#endif
}

void repo_loose_object_map_start_batch(struct odb_source *source MAYBE_UNUSED)
{
#ifdef WITH_RUST
	struct odb_source_files *files = odb_source_files_downcast(source);

	repo_loose_object_map_start_batch_1(files->loose->map);
#endif
}

int repo_loose_object_map_finish_batch(struct odb_source *source MAYBE_UNUSED,
				       bool noop_ok MAYBE_UNUSED,
				       char **file MAYBE_UNUSED)
{
#ifdef WITH_RUST
	struct odb_source_files *files = odb_source_files_downcast(source);

	if (repo_loose_object_map_batch_len(source) <= 0) {
		repo_loose_object_map_abort_batch_1(files->loose->map);
		return noop_ok ? 0 : -1;
	}

	return repo_write_loose_object_map(source->odb->repo,
					   files->loose->map,
					   file);
#else
	return 0;
#endif
}

int repo_loose_object_map_oid(struct repository *repo MAYBE_UNUSED,
			      const struct object_id *src MAYBE_UNUSED,
			      const struct git_hash_algo *to MAYBE_UNUSED,
			      struct object_id *dest MAYBE_UNUSED,
			      int flags MAYBE_UNUSED)
{
#ifdef WITH_RUST
	struct odb_source *source;
	uint32_t algo = hash_algo_by_ptr(to);
	struct object_id cur;
	bool seen = false;

	for (source = repo->objects->sources; source; source = source->next) {
		struct odb_source_files *files = odb_source_files_downcast(source);
		struct loose_object_map *loose_map = files->loose->map;
		struct loose_object_map_bin *bin = files->loose->map_bin;
		struct loose_object_map_bin_entry *entry;

		if (loose_map &&
		    !repo_loose_object_map_oid_1(loose_map, src, algo, dest)) {
			if (flags & LOOSE_MAP_VERIFY) {
				if (!seen) {
					oidcpy(&cur, dest);
					seen = true;
				} else if (!oideq(&cur, dest)) {
					return -2;
				}
			} else {
				return 0;
			}
		}
		if (!bin)
			continue;
		for (entry = bin->entries; entry; entry = entry->next)
			if (!loose_object_map_oid_bin_1(entry->ptr, src,
							algo, dest)) {
				if (flags & LOOSE_MAP_VERIFY) {
					if (!seen) {
						oidcpy(&cur, dest);
						seen = true;
					} else if (!oideq(&cur, dest)) {
						return -2;
					}
				} else {
					return 0;
				}
			}
	}
	if ((flags & LOOSE_MAP_VERIFY) && seen)
		return 0;
#endif
	return -1;
}

#ifndef WITH_RUST
void loose_object_map_init(struct loose_object_map **map, uint32_t algo UNUSED,
			   uint32_t compat_algo UNUSED)
{
	*map = NULL;
}

void loose_object_map_clear(struct loose_object_map **map)
{
	*map = NULL;
}
#endif
