/*
 * Recursive Merge algorithm stolen from git-merge-recursive.py by
 * Fredrik Kuivinen.
 * The thieves were Alex Riesen and Johannes Schindelin, in June/July 2006
 */
#include "cache.h"
#include "advice.h"
#include "lockfile.h"
#include "cache-tree.h"
#include "commit.h"
#include "blob.h"
#include "builtin.h"
#include "tree-walk.h"
#include "diff.h"
#include "diffcore.h"
#include "tag.h"
#include "unpack-trees.h"
#include "string-list.h"
#include "xdiff-interface.h"
#include "ll-merge.h"
#include "attr.h"
#include "merge-recursive.h"
#include "dir.h"
#include "submodule.h"

static void flush_output(struct merge_options *o)
{
	if (o->buffer_output < 2 && o->obuf.len) {
		fputs(o->obuf.buf, stdout);
		strbuf_reset(&o->obuf);
	}
}

static int err(struct merge_options *o, const char *err, ...)
{
	va_list params;

	if (o->buffer_output < 2)
		flush_output(o);
	else {
		strbuf_complete(&o->obuf, '\n');
		strbuf_addstr(&o->obuf, "error: ");
	}
	va_start(params, err);
	strbuf_vaddf(&o->obuf, err, params);
	va_end(params);
	if (o->buffer_output > 1)
		strbuf_addch(&o->obuf, '\n');
	else {
		error("%s", o->obuf.buf);
		strbuf_reset(&o->obuf);
	}

	return -1;
}

static struct tree *shift_tree_object(struct tree *one, struct tree *two,
				      const char *subtree_shift)
{
	struct object_id shifted;

	if (!*subtree_shift) {
		shift_tree(&one->object.oid, &two->object.oid, &shifted, 0);
	} else {
		shift_tree_by(&one->object.oid, &two->object.oid, &shifted,
			      subtree_shift);
	}
	if (!oidcmp(&two->object.oid, &shifted))
		return two;
	return lookup_tree(shifted.hash);
}

static struct commit *make_virtual_commit(struct tree *tree, const char *comment)
{
	struct commit *commit = alloc_commit_node();

	set_merge_remote_desc(commit, comment, (struct object *)commit);
	commit->tree = tree;
	commit->object.parsed = 1;
	return commit;
}

/*
 * Since we use get_tree_entry(), which does not put the read object into
 * the object pool, we cannot rely on a == b.
 */
static int oid_eq(const struct object_id *a, const struct object_id *b)
{
	if (!a && !b)
		return 2;
	return a && b && oidcmp(a, b) == 0;
}

enum rename_type {
	RENAME_NORMAL = 0,
	RENAME_DELETE,
	RENAME_ONE_FILE_TO_ONE,
	RENAME_ONE_FILE_TO_TWO,
	RENAME_TWO_FILES_TO_ONE
};

struct rename_conflict_info {
	enum rename_type rename_type;
	struct diff_filepair *pair1;
	struct diff_filepair *pair2;
	const char *branch1;
	const char *branch2;
	struct stage_data *dst_entry1;
	struct stage_data *dst_entry2;
	struct diff_filespec ren1_other;
	struct diff_filespec ren2_other;
};

/*
 * Since we want to write the index eventually, we cannot reuse the index
 * for these (temporary) data.
 */
struct stage_data {
	struct {
		unsigned mode;
		struct object_id oid;
	} stages[4];
	struct rename_conflict_info *rename_conflict_info;
	unsigned processed:1;
};

static inline void setup_rename_conflict_info(enum rename_type rename_type,
					      struct diff_filepair *pair1,
					      struct diff_filepair *pair2,
					      const char *branch1,
					      const char *branch2,
					      struct stage_data *dst_entry1,
					      struct stage_data *dst_entry2,
					      struct merge_options *o,
					      struct stage_data *src_entry1,
					      struct stage_data *src_entry2)
{
	struct rename_conflict_info *ci = xcalloc(1, sizeof(struct rename_conflict_info));
	ci->rename_type = rename_type;
	ci->pair1 = pair1;
	ci->branch1 = branch1;
	ci->branch2 = branch2;

	ci->dst_entry1 = dst_entry1;
	dst_entry1->rename_conflict_info = ci;
	dst_entry1->processed = 0;

	assert(!pair2 == !dst_entry2);
	if (dst_entry2) {
		ci->dst_entry2 = dst_entry2;
		ci->pair2 = pair2;
		dst_entry2->rename_conflict_info = ci;
	}

	if (rename_type == RENAME_TWO_FILES_TO_ONE) {
		/*
		 * For each rename, there could have been
		 * modifications on the side of history where that
		 * file was not renamed.
		 */
		int ostage1 = o->branch1 == branch1 ? 3 : 2;
		int ostage2 = ostage1 ^ 1;

		ci->ren1_other.path = pair1->one->path;
		oidcpy(&ci->ren1_other.oid, &src_entry1->stages[ostage1].oid);
		ci->ren1_other.mode = src_entry1->stages[ostage1].mode;

		ci->ren2_other.path = pair2->one->path;
		oidcpy(&ci->ren2_other.oid, &src_entry2->stages[ostage2].oid);
		ci->ren2_other.mode = src_entry2->stages[ostage2].mode;
	}
}

static int show(struct merge_options *o, int v)
{
	return (!o->call_depth && o->verbosity >= v) || o->verbosity >= 5;
}

__attribute__((format (printf, 3, 4)))
static void output(struct merge_options *o, int v, const char *fmt, ...)
{
	va_list ap;

	if (!show(o, v))
		return;

	strbuf_addchars(&o->obuf, ' ', o->call_depth * 2);

	va_start(ap, fmt);
	strbuf_vaddf(&o->obuf, fmt, ap);
	va_end(ap);

	strbuf_addch(&o->obuf, '\n');
	if (!o->buffer_output)
		flush_output(o);
}

static void output_commit_title(struct merge_options *o, struct commit *commit)
{
	strbuf_addchars(&o->obuf, ' ', o->call_depth * 2);
	if (commit->util)
		strbuf_addf(&o->obuf, "virtual %s\n",
			merge_remote_util(commit)->name);
	else {
		strbuf_addf(&o->obuf, "%s ",
			find_unique_abbrev(commit->object.oid.hash,
				DEFAULT_ABBREV));
		if (parse_commit(commit) != 0)
			strbuf_addf(&o->obuf, _("(bad commit)\n"));
		else {
			const char *title;
			const char *msg = get_commit_buffer(commit, NULL);
			int len = find_commit_subject(msg, &title);
			if (len)
				strbuf_addf(&o->obuf, "%.*s\n", len, title);
			unuse_commit_buffer(commit, msg);
		}
	}
	flush_output(o);
}

static int add_cacheinfo(struct merge_options *o,
		unsigned int mode, const struct object_id *oid,
		const char *path, int stage, int refresh, int options)
{
	struct cache_entry *ce;
	int ret;

	ce = make_cache_entry(mode, oid ? oid->hash : null_sha1, path, stage, 0);
	if (!ce)
		return err(o, _("addinfo_cache failed for path '%s'"), path);

	ret = add_cache_entry(ce, options);
	if (refresh) {
		struct cache_entry *nce;

		nce = refresh_cache_entry(ce, CE_MATCH_REFRESH | CE_MATCH_IGNORE_MISSING);
		if (nce != ce)
			ret = add_cache_entry(nce, options);
	}
	return ret;
}

static void init_tree_desc_from_tree(struct tree_desc *desc, struct tree *tree)
{
	parse_tree(tree);
	init_tree_desc(desc, tree->buffer, tree->size);
}

static int git_merge_trees(int index_only,
			   struct tree *common,
			   struct tree *head,
			   struct tree *merge)
{
	int rc;
	struct tree_desc t[3];
	struct unpack_trees_options opts;

	memset(&opts, 0, sizeof(opts));
	if (index_only)
		opts.index_only = 1;
	else
		opts.update = 1;
	opts.merge = 1;
	opts.head_idx = 2;
	opts.fn = threeway_merge;
	opts.src_index = &the_index;
	opts.dst_index = &the_index;
	setup_unpack_trees_porcelain(&opts, "merge");

	init_tree_desc_from_tree(t+0, common);
	init_tree_desc_from_tree(t+1, head);
	init_tree_desc_from_tree(t+2, merge);

	rc = unpack_trees(3, t, &opts);
	cache_tree_free(&active_cache_tree);
	return rc;
}

struct tree *write_tree_from_memory(struct merge_options *o)
{
	struct tree *result = NULL;

	if (unmerged_cache()) {
		int i;
		fprintf(stderr, "BUG: There are unmerged index entries:\n");
		for (i = 0; i < active_nr; i++) {
			const struct cache_entry *ce = active_cache[i];
			if (ce_stage(ce))
				fprintf(stderr, "BUG: %d %.*s\n", ce_stage(ce),
					(int)ce_namelen(ce), ce->name);
		}
		die("BUG: unmerged index entries in merge-recursive.c");
	}

	if (!active_cache_tree)
		active_cache_tree = cache_tree();

	if (!cache_tree_fully_valid(active_cache_tree) &&
	    cache_tree_update(&the_index, 0) < 0) {
		err(o, _("error building trees"));
		return NULL;
	}

	result = lookup_tree(active_cache_tree->sha1);

	return result;
}

static int save_files_dirs(const unsigned char *sha1,
		struct strbuf *base, const char *path,
		unsigned int mode, int stage, void *context)
{
	int baselen = base->len;
	struct merge_options *o = context;

	strbuf_addstr(base, path);

	if (S_ISDIR(mode))
		string_list_insert(&o->current_directory_set, base->buf);
	else
		string_list_insert(&o->current_file_set, base->buf);

	strbuf_setlen(base, baselen);
	return (S_ISDIR(mode) ? READ_TREE_RECURSIVE : 0);
}

static int get_files_dirs(struct merge_options *o, struct tree *tree)
{
	int n;
	struct pathspec match_all;
	memset(&match_all, 0, sizeof(match_all));
	if (read_tree_recursive(tree, "", 0, 0, &match_all, save_files_dirs, o))
		return 0;
	n = o->current_file_set.nr + o->current_directory_set.nr;
	return n;
}

/*
 * Returns an index_entry instance which doesn't have to correspond to
 * a real cache entry in Git's index.
 */
static struct stage_data *insert_stage_data(const char *path,
		struct tree *o, struct tree *a, struct tree *b,
		struct string_list *entries)
{
	struct string_list_item *item;
	struct stage_data *e = xcalloc(1, sizeof(struct stage_data));
	get_tree_entry(o->object.oid.hash, path,
			e->stages[1].oid.hash, &e->stages[1].mode);
	get_tree_entry(a->object.oid.hash, path,
			e->stages[2].oid.hash, &e->stages[2].mode);
	get_tree_entry(b->object.oid.hash, path,
			e->stages[3].oid.hash, &e->stages[3].mode);
	item = string_list_insert(entries, path);
	item->util = e;
	return e;
}

/*
 * Create a dictionary mapping file names to stage_data objects. The
 * dictionary contains one entry for every path with a non-zero stage entry.
 */
static struct string_list *get_unmerged(void)
{
	struct string_list *unmerged = xcalloc(1, sizeof(struct string_list));
	int i;

	unmerged->strdup_strings = 1;

	for (i = 0; i < active_nr; i++) {
		struct string_list_item *item;
		struct stage_data *e;
		const struct cache_entry *ce = active_cache[i];
		if (!ce_stage(ce))
			continue;

		item = string_list_lookup(unmerged, ce->name);
		if (!item) {
			item = string_list_insert(unmerged, ce->name);
			item->util = xcalloc(1, sizeof(struct stage_data));
		}
		e = item->util;
		e->stages[ce_stage(ce)].mode = ce->ce_mode;
		oidcpy(&e->stages[ce_stage(ce)].oid, &ce->oid);
	}

	return unmerged;
}

static int string_list_df_name_compare(const void *a, const void *b)
{
	const struct string_list_item *one = a;
	const struct string_list_item *two = b;
	int onelen = strlen(one->string);
	int twolen = strlen(two->string);
	/*
	 * Here we only care that entries for D/F conflicts are
	 * adjacent, in particular with the file of the D/F conflict
	 * appearing before files below the corresponding directory.
	 * The order of the rest of the list is irrelevant for us.
	 *
	 * To achieve this, we sort with df_name_compare and provide
	 * the mode S_IFDIR so that D/F conflicts will sort correctly.
	 * We use the mode S_IFDIR for everything else for simplicity,
	 * since in other cases any changes in their order due to
	 * sorting cause no problems for us.
	 */
	int cmp = df_name_compare(one->string, onelen, S_IFDIR,
				  two->string, twolen, S_IFDIR);
	/*
	 * Now that 'foo' and 'foo/bar' compare equal, we have to make sure
	 * that 'foo' comes before 'foo/bar'.
	 */
	if (cmp)
		return cmp;
	return onelen - twolen;
}

static void record_df_conflict_files(struct merge_options *o,
				     struct string_list *entries)
{
	/* If there is a D/F conflict and the file for such a conflict
	 * currently exist in the working tree, we want to allow it to be
	 * removed to make room for the corresponding directory if needed.
	 * The files underneath the directories of such D/F conflicts will
	 * be processed before the corresponding file involved in the D/F
	 * conflict.  If the D/F directory ends up being removed by the
	 * merge, then we won't have to touch the D/F file.  If the D/F
	 * directory needs to be written to the working copy, then the D/F
	 * file will simply be removed (in make_room_for_path()) to make
	 * room for the necessary paths.  Note that if both the directory
	 * and the file need to be present, then the D/F file will be
	 * reinstated with a new unique name at the time it is processed.
	 */
	struct string_list df_sorted_entries = STRING_LIST_INIT_NODUP;
	const char *last_file = NULL;
	int last_len = 0;
	int i;

	/*
	 * If we're merging merge-bases, we don't want to bother with
	 * any working directory changes.
	 */
	if (o->call_depth)
		return;

	/* Ensure D/F conflicts are adjacent in the entries list. */
	for (i = 0; i < entries->nr; i++) {
		struct string_list_item *next = &entries->items[i];
		string_list_append(&df_sorted_entries, next->string)->util =
				   next->util;
	}
	qsort(df_sorted_entries.items, entries->nr, sizeof(*entries->items),
	      string_list_df_name_compare);

	string_list_clear(&o->df_conflict_file_set, 1);
	for (i = 0; i < df_sorted_entries.nr; i++) {
		const char *path = df_sorted_entries.items[i].string;
		int len = strlen(path);
		struct stage_data *e = df_sorted_entries.items[i].util;

		/*
		 * Check if last_file & path correspond to a D/F conflict;
		 * i.e. whether path is last_file+'/'+<something>.
		 * If so, record that it's okay to remove last_file to make
		 * room for path and friends if needed.
		 */
		if (last_file &&
		    len > last_len &&
		    memcmp(path, last_file, last_len) == 0 &&
		    path[last_len] == '/') {
			string_list_insert(&o->df_conflict_file_set, last_file);
		}

		/*
		 * Determine whether path could exist as a file in the
		 * working directory as a possible D/F conflict.  This
		 * will only occur when it exists in stage 2 as a
		 * file.
		 */
		if (S_ISREG(e->stages[2].mode) || S_ISLNK(e->stages[2].mode)) {
			last_file = path;
			last_len = len;
		} else {
			last_file = NULL;
		}
	}
	string_list_clear(&df_sorted_entries, 0);
}

struct rename {
	struct diff_filepair *pair;
	struct stage_data *src_entry;
	struct stage_data *dst_entry;
	unsigned processed:1;
};

/*
 * Get information of all renames which occurred between 'o_tree' and
 * 'tree'. We need the three trees in the merge ('o_tree', 'a_tree' and
 * 'b_tree') to be able to associate the correct cache entries with
 * the rename information. 'tree' is always equal to either a_tree or b_tree.
 */
static struct string_list *get_renames(struct merge_options *o,
				       struct tree *tree,
				       struct tree *o_tree,
				       struct tree *a_tree,
				       struct tree *b_tree,
				       struct string_list *entries)
{
	int i;
	struct string_list *renames;
	struct diff_options opts;

	renames = xcalloc(1, sizeof(struct string_list));
	if (!o->detect_rename)
		return renames;

	diff_setup(&opts);
	DIFF_OPT_SET(&opts, RECURSIVE);
	DIFF_OPT_CLR(&opts, RENAME_EMPTY);
	opts.detect_rename = DIFF_DETECT_RENAME;
	opts.rename_limit = o->merge_rename_limit >= 0 ? o->merge_rename_limit :
			    o->diff_rename_limit >= 0 ? o->diff_rename_limit :
			    1000;
	opts.rename_score = o->rename_score;
	opts.show_rename_progress = o->show_rename_progress;
	opts.output_format = DIFF_FORMAT_NO_OUTPUT;
	diff_setup_done(&opts);
	diff_tree_sha1(o_tree->object.oid.hash, tree->object.oid.hash, "", &opts);
	diffcore_std(&opts);
	if (opts.needed_rename_limit > o->needed_rename_limit)
		o->needed_rename_limit = opts.needed_rename_limit;
	for (i = 0; i < diff_queued_diff.nr; ++i) {
		struct string_list_item *item;
		struct rename *re;
		struct diff_filepair *pair = diff_queued_diff.queue[i];
		if (pair->status != 'R') {
			diff_free_filepair(pair);
			continue;
		}
		re = xmalloc(sizeof(*re));
		re->processed = 0;
		re->pair = pair;
		item = string_list_lookup(entries, re->pair->one->path);
		if (!item)
			re->src_entry = insert_stage_data(re->pair->one->path,
					o_tree, a_tree, b_tree, entries);
		else
			re->src_entry = item->util;

		item = string_list_lookup(entries, re->pair->two->path);
		if (!item)
			re->dst_entry = insert_stage_data(re->pair->two->path,
					o_tree, a_tree, b_tree, entries);
		else
			re->dst_entry = item->util;
		item = string_list_insert(renames, pair->one->path);
		item->util = re;
	}
	opts.output_format = DIFF_FORMAT_NO_OUTPUT;
	diff_queued_diff.nr = 0;
	diff_flush(&opts);
	return renames;
}

static int update_stages(struct merge_options *opt, const char *path,
			 const struct diff_filespec *o,
			 const struct diff_filespec *a,
			 const struct diff_filespec *b)
{

	/*
	 * NOTE: It is usually a bad idea to call update_stages on a path
	 * before calling update_file on that same path, since it can
	 * sometimes lead to spurious "refusing to lose untracked file..."
	 * messages from update_file (via make_room_for path via
	 * would_lose_untracked).  Instead, reverse the order of the calls
	 * (executing update_file first and then update_stages).
	 */
	int clear = 1;
	int options = ADD_CACHE_OK_TO_ADD | ADD_CACHE_SKIP_DFCHECK;
	if (clear)
		if (remove_file_from_cache(path))
			return -1;
	if (o)
		if (add_cacheinfo(opt, o->mode, &o->oid, path, 1, 0, options))
			return -1;
	if (a)
		if (add_cacheinfo(opt, a->mode, &a->oid, path, 2, 0, options))
			return -1;
	if (b)
		if (add_cacheinfo(opt, b->mode, &b->oid, path, 3, 0, options))
			return -1;
	return 0;
}

static void update_entry(struct stage_data *entry,
			 struct diff_filespec *o,
			 struct diff_filespec *a,
			 struct diff_filespec *b)
{
	entry->processed = 0;
	entry->stages[1].mode = o->mode;
	entry->stages[2].mode = a->mode;
	entry->stages[3].mode = b->mode;
	oidcpy(&entry->stages[1].oid, &o->oid);
	oidcpy(&entry->stages[2].oid, &a->oid);
	oidcpy(&entry->stages[3].oid, &b->oid);
}

static int remove_file(struct merge_options *o, int clean,
		       const char *path, int no_wd)
{
	int update_cache = o->call_depth || clean;
	int update_working_directory = !o->call_depth && !no_wd;

	if (update_cache) {
		if (remove_file_from_cache(path))
			return -1;
	}
	if (update_working_directory) {
		if (ignore_case) {
			struct cache_entry *ce;
			ce = cache_file_exists(path, strlen(path), ignore_case);
			if (ce && ce_stage(ce) == 0)
				return 0;
		}
		if (remove_path(path))
			return -1;
	}
	return 0;
}

/* add a string to a strbuf, but converting "/" to "_" */
static void add_flattened_path(struct strbuf *out, const char *s)
{
	size_t i = out->len;
	strbuf_addstr(out, s);
	for (; i < out->len; i++)
		if (out->buf[i] == '/')
			out->buf[i] = '_';
}

static char *unique_path(struct merge_options *o, const char *path, const char *branch)
{
	struct strbuf newpath = STRBUF_INIT;
	int suffix = 0;
	size_t base_len;

	strbuf_addf(&newpath, "%s~", path);
	add_flattened_path(&newpath, branch);

	base_len = newpath.len;
	while (string_list_has_string(&o->current_file_set, newpath.buf) ||
	       string_list_has_string(&o->current_directory_set, newpath.buf) ||
	       (!o->call_depth && file_exists(newpath.buf))) {
		strbuf_setlen(&newpath, base_len);
		strbuf_addf(&newpath, "_%d", suffix++);
	}

	string_list_insert(&o->current_file_set, newpath.buf);
	return strbuf_detach(&newpath, NULL);
}

static int dir_in_way(const char *path, int check_working_copy)
{
	int pos;
	struct strbuf dirpath = STRBUF_INIT;
	struct stat st;

	strbuf_addstr(&dirpath, path);
	strbuf_addch(&dirpath, '/');

	pos = cache_name_pos(dirpath.buf, dirpath.len);

	if (pos < 0)
		pos = -1 - pos;
	if (pos < active_nr &&
	    !strncmp(dirpath.buf, active_cache[pos]->name, dirpath.len)) {
		strbuf_release(&dirpath);
		return 1;
	}

	strbuf_release(&dirpath);
	return check_working_copy && !lstat(path, &st) && S_ISDIR(st.st_mode);
}

static int was_tracked(const char *path)
{
	int pos = cache_name_pos(path, strlen(path));

	if (0 <= pos)
		/* we have been tracking this path */
		return 1;

	/*
	 * Look for an unmerged entry for the path,
	 * specifically stage #2, which would indicate
	 * that "our" side before the merge started
	 * had the path tracked (and resulted in a conflict).
	 */
	for (pos = -1 - pos;
	     pos < active_nr && !strcmp(path, active_cache[pos]->name);
	     pos++)
		if (ce_stage(active_cache[pos]) == 2)
			return 1;
	return 0;
}

static int would_lose_untracked(const char *path)
{
	return !was_tracked(path) && file_exists(path);
}

static int make_room_for_path(struct merge_options *o, const char *path)
{
	int status, i;
	const char *msg = _("failed to create path '%s'%s");

	/* Unlink any D/F conflict files that are in the way */
	for (i = 0; i < o->df_conflict_file_set.nr; i++) {
		const char *df_path = o->df_conflict_file_set.items[i].string;
		size_t pathlen = strlen(path);
		size_t df_pathlen = strlen(df_path);
		if (df_pathlen < pathlen &&
		    path[df_pathlen] == '/' &&
		    strncmp(path, df_path, df_pathlen) == 0) {
			output(o, 3,
			       _("Removing %s to make room for subdirectory\n"),
			       df_path);
			unlink(df_path);
			unsorted_string_list_delete_item(&o->df_conflict_file_set,
							 i, 0);
			break;
		}
	}

	/* Make sure leading directories are created */
	status = safe_create_leading_directories_const(path);
	if (status) {
		if (status == SCLD_EXISTS)
			/* something else exists */
			return err(o, msg, path, _(": perhaps a D/F conflict?"));
		return err(o, msg, path, "");
	}

	/*
	 * Do not unlink a file in the work tree if we are not
	 * tracking it.
	 */
	if (would_lose_untracked(path))
		return err(o, _("refusing to lose untracked file at '%s'"),
			     path);

	/* Successful unlink is good.. */
	if (!unlink(path))
		return 0;
	/* .. and so is no existing file */
	if (errno == ENOENT)
		return 0;
	/* .. but not some other error (who really cares what?) */
	return err(o, msg, path, _(": perhaps a D/F conflict?"));
}

static int update_file_flags(struct merge_options *o,
			     const struct object_id *oid,
			     unsigned mode,
			     const char *path,
			     int update_cache,
			     int update_wd)
{
	int ret = 0;

	if (o->call_depth)
		update_wd = 0;

	if (update_wd) {
		enum object_type type;
		void *buf;
		unsigned long size;

		if (S_ISGITLINK(mode)) {
			/*
			 * We may later decide to recursively descend into
			 * the submodule directory and update its index
			 * and/or work tree, but we do not do that now.
			 */
			update_wd = 0;
			goto update_index;
		}

		buf = read_sha1_file(oid->hash, &type, &size);
		if (!buf)
			return err(o, _("cannot read object %s '%s'"), oid_to_hex(oid), path);
		if (type != OBJ_BLOB) {
			ret = err(o, _("blob expected for %s '%s'"), oid_to_hex(oid), path);
			goto free_buf;
		}
		if (S_ISREG(mode)) {
			struct strbuf strbuf = STRBUF_INIT;
			if (convert_to_working_tree(path, buf, size, &strbuf)) {
				free(buf);
				size = strbuf.len;
				buf = strbuf_detach(&strbuf, NULL);
			}
		}

		if (make_room_for_path(o, path) < 0) {
			update_wd = 0;
			goto free_buf;
		}
		if (S_ISREG(mode) || (!has_symlinks && S_ISLNK(mode))) {
			int fd;
			if (mode & 0100)
				mode = 0777;
			else
				mode = 0666;
			fd = open(path, O_WRONLY | O_TRUNC | O_CREAT, mode);
			if (fd < 0) {
				ret = err(o, _("failed to open '%s': %s"),
					  path, strerror(errno));
				goto free_buf;
			}
			write_in_full(fd, buf, size);
			close(fd);
		} else if (S_ISLNK(mode)) {
			char *lnk = xmemdupz(buf, size);
			safe_create_leading_directories_const(path);
			unlink(path);
			if (symlink(lnk, path))
				ret = err(o, _("failed to symlink '%s': %s"),
					path, strerror(errno));
			free(lnk);
		} else
			ret = err(o,
				  _("do not know what to do with %06o %s '%s'"),
				  mode, oid_to_hex(oid), path);
 free_buf:
		free(buf);
	}
 update_index:
	if (!ret && update_cache)
		add_cacheinfo(o, mode, oid, path, 0, update_wd, ADD_CACHE_OK_TO_ADD);
	return ret;
}

static int update_file(struct merge_options *o,
		       int clean,
		       const struct object_id *oid,
		       unsigned mode,
		       const char *path)
{
	return update_file_flags(o, oid, mode, path, o->call_depth || clean, !o->call_depth);
}

/* Low level file merging, update and removal */

struct merge_file_info {
	struct object_id oid;
	unsigned mode;
	unsigned clean:1,
		 merge:1;
};

static int merge_3way(struct merge_options *o,
		      mmbuffer_t *result_buf,
		      const struct diff_filespec *one,
		      const struct diff_filespec *a,
		      const struct diff_filespec *b,
		      const char *branch1,
		      const char *branch2)
{
	mmfile_t orig, src1, src2;
	struct ll_merge_options ll_opts = {0};
	char *base_name, *name1, *name2;
	int merge_status;

	ll_opts.renormalize = o->renormalize;
	ll_opts.xdl_opts = o->xdl_opts;

	if (o->call_depth) {
		ll_opts.virtual_ancestor = 1;
		ll_opts.variant = 0;
	} else {
		switch (o->recursive_variant) {
		case MERGE_RECURSIVE_OURS:
			ll_opts.variant = XDL_MERGE_FAVOR_OURS;
			break;
		case MERGE_RECURSIVE_THEIRS:
			ll_opts.variant = XDL_MERGE_FAVOR_THEIRS;
			break;
		default:
			ll_opts.variant = 0;
			break;
		}
	}

	if (strcmp(a->path, b->path) ||
	    (o->ancestor != NULL && strcmp(a->path, one->path) != 0)) {
		base_name = o->ancestor == NULL ? NULL :
			mkpathdup("%s:%s", o->ancestor, one->path);
		name1 = mkpathdup("%s:%s", branch1, a->path);
		name2 = mkpathdup("%s:%s", branch2, b->path);
	} else {
		base_name = o->ancestor == NULL ? NULL :
			mkpathdup("%s", o->ancestor);
		name1 = mkpathdup("%s", branch1);
		name2 = mkpathdup("%s", branch2);
	}

	read_mmblob(&orig, one->oid.hash);
	read_mmblob(&src1, a->oid.hash);
	read_mmblob(&src2, b->oid.hash);

	merge_status = ll_merge(result_buf, a->path, &orig, base_name,
				&src1, name1, &src2, name2, &ll_opts);

	free(base_name);
	free(name1);
	free(name2);
	free(orig.ptr);
	free(src1.ptr);
	free(src2.ptr);
	return merge_status;
}

static int merge_file_1(struct merge_options *o,
					   const struct diff_filespec *one,
					   const struct diff_filespec *a,
					   const struct diff_filespec *b,
					   const char *branch1,
					   const char *branch2,
					   struct merge_file_info *result)
{
	result->merge = 0;
	result->clean = 1;

	if ((S_IFMT & a->mode) != (S_IFMT & b->mode)) {
		result->clean = 0;
		if (S_ISREG(a->mode)) {
			result->mode = a->mode;
			oidcpy(&result->oid, &a->oid);
		} else {
			result->mode = b->mode;
			oidcpy(&result->oid, &b->oid);
		}
	} else {
		if (!oid_eq(&a->oid, &one->oid) && !oid_eq(&b->oid, &one->oid))
			result->merge = 1;

		/*
		 * Merge modes
		 */
		if (a->mode == b->mode || a->mode == one->mode)
			result->mode = b->mode;
		else {
			result->mode = a->mode;
			if (b->mode != one->mode) {
				result->clean = 0;
				result->merge = 1;
			}
		}

		if (oid_eq(&a->oid, &b->oid) || oid_eq(&a->oid, &one->oid))
			oidcpy(&result->oid, &b->oid);
		else if (oid_eq(&b->oid, &one->oid))
			oidcpy(&result->oid, &a->oid);
		else if (S_ISREG(a->mode)) {
			mmbuffer_t result_buf;
			int ret = 0, merge_status;

			merge_status = merge_3way(o, &result_buf, one, a, b,
						  branch1, branch2);

			if ((merge_status < 0) || !result_buf.ptr)
				ret = err(o, _("Failed to execute internal merge"));

			if (!ret && write_sha1_file(result_buf.ptr, result_buf.size,
						    blob_type, result->oid.hash))
				ret = err(o, _("Unable to add %s to database"),
					  a->path);

			free(result_buf.ptr);
			if (ret)
				return ret;
			result->clean = (merge_status == 0);
		} else if (S_ISGITLINK(a->mode)) {
			result->clean = merge_submodule(result->oid.hash,
						       one->path,
						       one->oid.hash,
						       a->oid.hash,
						       b->oid.hash,
						       !o->call_depth);
		} else if (S_ISLNK(a->mode)) {
			oidcpy(&result->oid, &a->oid);

			if (!oid_eq(&a->oid, &b->oid))
				result->clean = 0;
		} else
			die("BUG: unsupported object type in the tree");
	}

	return 0;
}

static int merge_file_special_markers(struct merge_options *o,
			   const struct diff_filespec *one,
			   const struct diff_filespec *a,
			   const struct diff_filespec *b,
			   const char *branch1,
			   const char *filename1,
			   const char *branch2,
			   const char *filename2,
			   struct merge_file_info *mfi)
{
	char *side1 = NULL;
	char *side2 = NULL;
	int ret;

	if (filename1)
		side1 = xstrfmt("%s:%s", branch1, filename1);
	if (filename2)
		side2 = xstrfmt("%s:%s", branch2, filename2);

	ret = merge_file_1(o, one, a, b,
			   side1 ? side1 : branch1,
			   side2 ? side2 : branch2, mfi);
	free(side1);
	free(side2);
	return ret;
}

static int merge_file_one(struct merge_options *o,
					 const char *path,
					 const struct object_id *o_oid, int o_mode,
					 const struct object_id *a_oid, int a_mode,
					 const struct object_id *b_oid, int b_mode,
					 const char *branch1,
					 const char *branch2,
					 struct merge_file_info *mfi)
{
	struct diff_filespec one, a, b;

	one.path = a.path = b.path = (char *)path;
	oidcpy(&one.oid, o_oid);
	one.mode = o_mode;
	oidcpy(&a.oid, a_oid);
	a.mode = a_mode;
	oidcpy(&b.oid, b_oid);
	b.mode = b_mode;
	return merge_file_1(o, &one, &a, &b, branch1, branch2, mfi);
}

static int handle_change_delete(struct merge_options *o,
				 const char *path,
				 const struct object_id *o_oid, int o_mode,
				 const struct object_id *a_oid, int a_mode,
				 const struct object_id *b_oid, int b_mode,
				 const char *change, const char *change_past)
{
	char *renamed = NULL;
	int ret = 0;
	if (dir_in_way(path, !o->call_depth)) {
		renamed = unique_path(o, path, a_oid ? o->branch1 : o->branch2);
	}

	if (o->call_depth) {
		/*
		 * We cannot arbitrarily accept either a_sha or b_sha as
		 * correct; since there is no true "middle point" between
		 * them, simply reuse the base version for virtual merge base.
		 */
		ret = remove_file_from_cache(path);
		if (!ret)
			ret = update_file(o, 0, o_oid, o_mode,
					  renamed ? renamed : path);
	} else if (!a_oid) {
		if (!renamed) {
			output(o, 1, _("CONFLICT (%s/delete): %s deleted in %s "
			       "and %s in %s. Version %s of %s left in tree."),
			       change, path, o->branch1, change_past,
			       o->branch2, o->branch2, path);
			ret = update_file(o, 0, b_oid, b_mode, path);
		} else {
			output(o, 1, _("CONFLICT (%s/delete): %s deleted in %s "
			       "and %s in %s. Version %s of %s left in tree at %s."),
			       change, path, o->branch1, change_past,
			       o->branch2, o->branch2, path, renamed);
			ret = update_file(o, 0, b_oid, b_mode, renamed);
		}
	} else {
		if (!renamed) {
			output(o, 1, _("CONFLICT (%s/delete): %s deleted in %s "
			       "and %s in %s. Version %s of %s left in tree."),
			       change, path, o->branch2, change_past,
			       o->branch1, o->branch1, path);
		} else {
			output(o, 1, _("CONFLICT (%s/delete): %s deleted in %s "
			       "and %s in %s. Version %s of %s left in tree at %s."),
			       change, path, o->branch2, change_past,
			       o->branch1, o->branch1, path, renamed);
			ret = update_file(o, 0, a_oid, a_mode, renamed);
		}
		/*
		 * No need to call update_file() on path when !renamed, since
		 * that would needlessly touch path.  We could call
		 * update_file_flags() with update_cache=0 and update_wd=0,
		 * but that's a no-op.
		 */
	}
	free(renamed);

	return ret;
}

static int conflict_rename_delete(struct merge_options *o,
				   struct diff_filepair *pair,
				   const char *rename_branch,
				   const char *other_branch)
{
	const struct diff_filespec *orig = pair->one;
	const struct diff_filespec *dest = pair->two;
	const struct object_id *a_oid = NULL;
	const struct object_id *b_oid = NULL;
	int a_mode = 0;
	int b_mode = 0;

	if (rename_branch == o->branch1) {
		a_oid = &dest->oid;
		a_mode = dest->mode;
	} else {
		b_oid = &dest->oid;
		b_mode = dest->mode;
	}

	if (handle_change_delete(o,
				 o->call_depth ? orig->path : dest->path,
				 &orig->oid, orig->mode,
				 a_oid, a_mode,
				 b_oid, b_mode,
				 _("rename"), _("renamed")))
		return -1;

	if (o->call_depth)
		return remove_file_from_cache(dest->path);
	else
		return update_stages(o, dest->path, NULL,
				     rename_branch == o->branch1 ? dest : NULL,
				     rename_branch == o->branch1 ? NULL : dest);
}

static struct diff_filespec *filespec_from_entry(struct diff_filespec *target,
						 struct stage_data *entry,
						 int stage)
{
	struct object_id *oid = &entry->stages[stage].oid;
	unsigned mode = entry->stages[stage].mode;
	if (mode == 0 || is_null_oid(oid))
		return NULL;
	oidcpy(&target->oid, oid);
	target->mode = mode;
	return target;
}

static int handle_file(struct merge_options *o,
			struct diff_filespec *rename,
			int stage,
			struct rename_conflict_info *ci)
{
	char *dst_name = rename->path;
	struct stage_data *dst_entry;
	const char *cur_branch, *other_branch;
	struct diff_filespec other;
	struct diff_filespec *add;
	int ret;

	if (stage == 2) {
		dst_entry = ci->dst_entry1;
		cur_branch = ci->branch1;
		other_branch = ci->branch2;
	} else {
		dst_entry = ci->dst_entry2;
		cur_branch = ci->branch2;
		other_branch = ci->branch1;
	}

	add = filespec_from_entry(&other, dst_entry, stage ^ 1);
	if (add) {
		char *add_name = unique_path(o, rename->path, other_branch);
		if (update_file(o, 0, &add->oid, add->mode, add_name))
			return -1;

		remove_file(o, 0, rename->path, 0);
		dst_name = unique_path(o, rename->path, cur_branch);
	} else {
		if (dir_in_way(rename->path, !o->call_depth)) {
			dst_name = unique_path(o, rename->path, cur_branch);
			output(o, 1, _("%s is a directory in %s adding as %s instead"),
			       rename->path, other_branch, dst_name);
		}
	}
	if ((ret = update_file(o, 0, &rename->oid, rename->mode, dst_name)))
		; /* fall through, do allow dst_name to be released */
	else if (stage == 2)
		ret = update_stages(o, rename->path, NULL, rename, add);
	else
		ret = update_stages(o, rename->path, NULL, add, rename);

	if (dst_name != rename->path)
		free(dst_name);

	return ret;
}

static int conflict_rename_rename_1to2(struct merge_options *o,
					struct rename_conflict_info *ci)
{
	/* One file was renamed in both branches, but to different names. */
	struct diff_filespec *one = ci->pair1->one;
	struct diff_filespec *a = ci->pair1->two;
	struct diff_filespec *b = ci->pair2->two;

	output(o, 1, _("CONFLICT (rename/rename): "
	       "Rename \"%s\"->\"%s\" in branch \"%s\" "
	       "rename \"%s\"->\"%s\" in \"%s\"%s"),
	       one->path, a->path, ci->branch1,
	       one->path, b->path, ci->branch2,
	       o->call_depth ? _(" (left unresolved)") : "");
	if (o->call_depth) {
		struct merge_file_info mfi;
		struct diff_filespec other;
		struct diff_filespec *add;
		if (merge_file_one(o, one->path,
				 &one->oid, one->mode,
				 &a->oid, a->mode,
				 &b->oid, b->mode,
				 ci->branch1, ci->branch2, &mfi))
			return -1;

		/*
		 * FIXME: For rename/add-source conflicts (if we could detect
		 * such), this is wrong.  We should instead find a unique
		 * pathname and then either rename the add-source file to that
		 * unique path, or use that unique path instead of src here.
		 */
		if (update_file(o, 0, &mfi.oid, mfi.mode, one->path))
			return -1;

		/*
		 * Above, we put the merged content at the merge-base's
		 * path.  Now we usually need to delete both a->path and
		 * b->path.  However, the rename on each side of the merge
		 * could also be involved in a rename/add conflict.  In
		 * such cases, we should keep the added file around,
		 * resolving the conflict at that path in its favor.
		 */
		add = filespec_from_entry(&other, ci->dst_entry1, 2 ^ 1);
		if (add) {
			if (update_file(o, 0, &add->oid, add->mode, a->path))
				return -1;
		}
		else
			remove_file_from_cache(a->path);
		add = filespec_from_entry(&other, ci->dst_entry2, 3 ^ 1);
		if (add) {
			if (update_file(o, 0, &add->oid, add->mode, b->path))
				return -1;
		}
		else
			remove_file_from_cache(b->path);
	} else if (handle_file(o, a, 2, ci) || handle_file(o, b, 3, ci))
		return -1;

	return 0;
}

static int conflict_rename_rename_2to1(struct merge_options *o,
					struct rename_conflict_info *ci)
{
	/* Two files, a & b, were renamed to the same thing, c. */
	struct diff_filespec *a = ci->pair1->one;
	struct diff_filespec *b = ci->pair2->one;
	struct diff_filespec *c1 = ci->pair1->two;
	struct diff_filespec *c2 = ci->pair2->two;
	char *path = c1->path; /* == c2->path */
	struct merge_file_info mfi_c1;
	struct merge_file_info mfi_c2;
	int ret;

	output(o, 1, _("CONFLICT (rename/rename): "
	       "Rename %s->%s in %s. "
	       "Rename %s->%s in %s"),
	       a->path, c1->path, ci->branch1,
	       b->path, c2->path, ci->branch2);

	remove_file(o, 1, a->path, o->call_depth || would_lose_untracked(a->path));
	remove_file(o, 1, b->path, o->call_depth || would_lose_untracked(b->path));

	if (merge_file_special_markers(o, a, c1, &ci->ren1_other,
				       o->branch1, c1->path,
				       o->branch2, ci->ren1_other.path, &mfi_c1) ||
	    merge_file_special_markers(o, b, &ci->ren2_other, c2,
				       o->branch1, ci->ren2_other.path,
				       o->branch2, c2->path, &mfi_c2))
		return -1;

	if (o->call_depth) {
		/*
		 * If mfi_c1.clean && mfi_c2.clean, then it might make
		 * sense to do a two-way merge of those results.  But, I
		 * think in all cases, it makes sense to have the virtual
		 * merge base just undo the renames; they can be detected
		 * again later for the non-recursive merge.
		 */
		remove_file(o, 0, path, 0);
		ret = update_file(o, 0, &mfi_c1.oid, mfi_c1.mode, a->path);
		if (!ret)
			ret = update_file(o, 0, &mfi_c2.oid, mfi_c2.mode,
					  b->path);
	} else {
		char *new_path1 = unique_path(o, path, ci->branch1);
		char *new_path2 = unique_path(o, path, ci->branch2);
		output(o, 1, _("Renaming %s to %s and %s to %s instead"),
		       a->path, new_path1, b->path, new_path2);
		remove_file(o, 0, path, 0);
		ret = update_file(o, 0, &mfi_c1.oid, mfi_c1.mode, new_path1);
		if (!ret)
			ret = update_file(o, 0, &mfi_c2.oid, mfi_c2.mode,
					  new_path2);
		free(new_path2);
		free(new_path1);
	}

	return ret;
}

static int process_renames(struct merge_options *o,
			   struct string_list *a_renames,
			   struct string_list *b_renames)
{
	int clean_merge = 1, i, j;
	struct string_list a_by_dst = STRING_LIST_INIT_NODUP;
	struct string_list b_by_dst = STRING_LIST_INIT_NODUP;
	const struct rename *sre;

	for (i = 0; i < a_renames->nr; i++) {
		sre = a_renames->items[i].util;
		string_list_insert(&a_by_dst, sre->pair->two->path)->util
			= (void *)sre;
	}
	for (i = 0; i < b_renames->nr; i++) {
		sre = b_renames->items[i].util;
		string_list_insert(&b_by_dst, sre->pair->two->path)->util
			= (void *)sre;
	}

	for (i = 0, j = 0; i < a_renames->nr || j < b_renames->nr;) {
		struct string_list *renames1, *renames2Dst;
		struct rename *ren1 = NULL, *ren2 = NULL;
		const char *branch1, *branch2;
		const char *ren1_src, *ren1_dst;
		struct string_list_item *lookup;

		if (i >= a_renames->nr) {
			ren2 = b_renames->items[j++].util;
		} else if (j >= b_renames->nr) {
			ren1 = a_renames->items[i++].util;
		} else {
			int compare = strcmp(a_renames->items[i].string,
					     b_renames->items[j].string);
			if (compare <= 0)
				ren1 = a_renames->items[i++].util;
			if (compare >= 0)
				ren2 = b_renames->items[j++].util;
		}

		/* TODO: refactor, so that 1/2 are not needed */
		if (ren1) {
			renames1 = a_renames;
			renames2Dst = &b_by_dst;
			branch1 = o->branch1;
			branch2 = o->branch2;
		} else {
			struct rename *tmp;
			renames1 = b_renames;
			renames2Dst = &a_by_dst;
			branch1 = o->branch2;
			branch2 = o->branch1;
			tmp = ren2;
			ren2 = ren1;
			ren1 = tmp;
		}

		if (ren1->processed)
			continue;
		ren1->processed = 1;
		ren1->dst_entry->processed = 1;
		/* BUG: We should only mark src_entry as processed if we
		 * are not dealing with a rename + add-source case.
		 */
		ren1->src_entry->processed = 1;

		ren1_src = ren1->pair->one->path;
		ren1_dst = ren1->pair->two->path;

		if (ren2) {
			/* One file renamed on both sides */
			const char *ren2_src = ren2->pair->one->path;
			const char *ren2_dst = ren2->pair->two->path;
			enum rename_type rename_type;
			if (strcmp(ren1_src, ren2_src) != 0)
				die("BUG: ren1_src != ren2_src");
			ren2->dst_entry->processed = 1;
			ren2->processed = 1;
			if (strcmp(ren1_dst, ren2_dst) != 0) {
				rename_type = RENAME_ONE_FILE_TO_TWO;
				clean_merge = 0;
			} else {
				rename_type = RENAME_ONE_FILE_TO_ONE;
				/* BUG: We should only remove ren1_src in
				 * the base stage (think of rename +
				 * add-source cases).
				 */
				remove_file(o, 1, ren1_src, 1);
				update_entry(ren1->dst_entry,
					     ren1->pair->one,
					     ren1->pair->two,
					     ren2->pair->two);
			}
			setup_rename_conflict_info(rename_type,
						   ren1->pair,
						   ren2->pair,
						   branch1,
						   branch2,
						   ren1->dst_entry,
						   ren2->dst_entry,
						   o,
						   NULL,
						   NULL);
		} else if ((lookup = string_list_lookup(renames2Dst, ren1_dst))) {
			/* Two different files renamed to the same thing */
			char *ren2_dst;
			ren2 = lookup->util;
			ren2_dst = ren2->pair->two->path;
			if (strcmp(ren1_dst, ren2_dst) != 0)
				die("BUG: ren1_dst != ren2_dst");

			clean_merge = 0;
			ren2->processed = 1;
			/*
			 * BUG: We should only mark src_entry as processed
			 * if we are not dealing with a rename + add-source
			 * case.
			 */
			ren2->src_entry->processed = 1;

			setup_rename_conflict_info(RENAME_TWO_FILES_TO_ONE,
						   ren1->pair,
						   ren2->pair,
						   branch1,
						   branch2,
						   ren1->dst_entry,
						   ren2->dst_entry,
						   o,
						   ren1->src_entry,
						   ren2->src_entry);

		} else {
			/* Renamed in 1, maybe changed in 2 */
			/* we only use sha1 and mode of these */
			struct diff_filespec src_other, dst_other;
			int try_merge;

			/*
			 * unpack_trees loads entries from common-commit
			 * into stage 1, from head-commit into stage 2, and
			 * from merge-commit into stage 3.  We keep track
			 * of which side corresponds to the rename.
			 */
			int renamed_stage = a_renames == renames1 ? 2 : 3;
			int other_stage =   a_renames == renames1 ? 3 : 2;

			/* BUG: We should only remove ren1_src in the base
			 * stage and in other_stage (think of rename +
			 * add-source case).
			 */
			remove_file(o, 1, ren1_src,
				    renamed_stage == 2 || !was_tracked(ren1_src));

			oidcpy(&src_other.oid,
			       &ren1->src_entry->stages[other_stage].oid);
			src_other.mode = ren1->src_entry->stages[other_stage].mode;
			oidcpy(&dst_other.oid,
			       &ren1->dst_entry->stages[other_stage].oid);
			dst_other.mode = ren1->dst_entry->stages[other_stage].mode;
			try_merge = 0;

			if (oid_eq(&src_other.oid, &null_oid)) {
				setup_rename_conflict_info(RENAME_DELETE,
							   ren1->pair,
							   NULL,
							   branch1,
							   branch2,
							   ren1->dst_entry,
							   NULL,
							   o,
							   NULL,
							   NULL);
			} else if ((dst_other.mode == ren1->pair->two->mode) &&
				   oid_eq(&dst_other.oid, &ren1->pair->two->oid)) {
				/*
				 * Added file on the other side identical to
				 * the file being renamed: clean merge.
				 * Also, there is no need to overwrite the
				 * file already in the working copy, so call
				 * update_file_flags() instead of
				 * update_file().
				 */
				if (update_file_flags(o,
						      &ren1->pair->two->oid,
						      ren1->pair->two->mode,
						      ren1_dst,
						      1, /* update_cache */
						      0  /* update_wd    */))
					clean_merge = -1;
			} else if (!oid_eq(&dst_other.oid, &null_oid)) {
				clean_merge = 0;
				try_merge = 1;
				output(o, 1, _("CONFLICT (rename/add): Rename %s->%s in %s. "
				       "%s added in %s"),
				       ren1_src, ren1_dst, branch1,
				       ren1_dst, branch2);
				if (o->call_depth) {
					struct merge_file_info mfi;
					if (merge_file_one(o, ren1_dst, &null_oid, 0,
							   &ren1->pair->two->oid,
							   ren1->pair->two->mode,
							   &dst_other.oid,
							   dst_other.mode,
							   branch1, branch2, &mfi)) {
						clean_merge = -1;
						goto cleanup_and_return;
					}
					output(o, 1, _("Adding merged %s"), ren1_dst);
					if (update_file(o, 0, &mfi.oid,
							mfi.mode, ren1_dst))
						clean_merge = -1;
					try_merge = 0;
				} else {
					char *new_path = unique_path(o, ren1_dst, branch2);
					output(o, 1, _("Adding as %s instead"), new_path);
					if (update_file(o, 0, &dst_other.oid,
							dst_other.mode, new_path))
						clean_merge = -1;
					free(new_path);
				}
			} else
				try_merge = 1;

			if (clean_merge < 0)
				goto cleanup_and_return;
			if (try_merge) {
				struct diff_filespec *one, *a, *b;
				src_other.path = (char *)ren1_src;

				one = ren1->pair->one;
				if (a_renames == renames1) {
					a = ren1->pair->two;
					b = &src_other;
				} else {
					b = ren1->pair->two;
					a = &src_other;
				}
				update_entry(ren1->dst_entry, one, a, b);
				setup_rename_conflict_info(RENAME_NORMAL,
							   ren1->pair,
							   NULL,
							   branch1,
							   NULL,
							   ren1->dst_entry,
							   NULL,
							   o,
							   NULL,
							   NULL);
			}
		}
	}
cleanup_and_return:
	string_list_clear(&a_by_dst, 0);
	string_list_clear(&b_by_dst, 0);

	return clean_merge;
}

static struct object_id *stage_oid(const struct object_id *oid, unsigned mode)
{
	return (is_null_oid(oid) || mode == 0) ? NULL: (struct object_id *)oid;
}

static int read_oid_strbuf(struct merge_options *o,
	const struct object_id *oid, struct strbuf *dst)
{
	void *buf;
	enum object_type type;
	unsigned long size;
	buf = read_sha1_file(oid->hash, &type, &size);
	if (!buf)
		return err(o, _("cannot read object %s"), oid_to_hex(oid));
	if (type != OBJ_BLOB) {
		free(buf);
		return err(o, _("object %s is not a blob"), oid_to_hex(oid));
	}
	strbuf_attach(dst, buf, size, size + 1);
	return 0;
}

static int blob_unchanged(struct merge_options *opt,
			  const struct object_id *o_oid,
			  unsigned o_mode,
			  const struct object_id *a_oid,
			  unsigned a_mode,
			  int renormalize, const char *path)
{
	struct strbuf o = STRBUF_INIT;
	struct strbuf a = STRBUF_INIT;
	int ret = 0; /* assume changed for safety */

	if (a_mode != o_mode)
		return 0;
	if (oid_eq(o_oid, a_oid))
		return 1;
	if (!renormalize)
		return 0;

	assert(o_oid && a_oid);
	if (read_oid_strbuf(opt, o_oid, &o) || read_oid_strbuf(opt, a_oid, &a))
		goto error_return;
	/*
	 * Note: binary | is used so that both renormalizations are
	 * performed.  Comparison can be skipped if both files are
	 * unchanged since their sha1s have already been compared.
	 */
	if (renormalize_buffer(path, o.buf, o.len, &o) |
	    renormalize_buffer(path, a.buf, a.len, &a))
		ret = (o.len == a.len && !memcmp(o.buf, a.buf, o.len));

error_return:
	strbuf_release(&o);
	strbuf_release(&a);
	return ret;
}

static int handle_modify_delete(struct merge_options *o,
				 const char *path,
				 struct object_id *o_oid, int o_mode,
				 struct object_id *a_oid, int a_mode,
				 struct object_id *b_oid, int b_mode)
{
	return handle_change_delete(o,
				    path,
				    o_oid, o_mode,
				    a_oid, a_mode,
				    b_oid, b_mode,
				    _("modify"), _("modified"));
}

static int merge_content(struct merge_options *o,
			 const char *path,
			 struct object_id *o_oid, int o_mode,
			 struct object_id *a_oid, int a_mode,
			 struct object_id *b_oid, int b_mode,
			 struct rename_conflict_info *rename_conflict_info)
{
	const char *reason = _("content");
	const char *path1 = NULL, *path2 = NULL;
	struct merge_file_info mfi;
	struct diff_filespec one, a, b;
	unsigned df_conflict_remains = 0;

	if (!o_oid) {
		reason = _("add/add");
		o_oid = (struct object_id *)&null_oid;
	}
	one.path = a.path = b.path = (char *)path;
	oidcpy(&one.oid, o_oid);
	one.mode = o_mode;
	oidcpy(&a.oid, a_oid);
	a.mode = a_mode;
	oidcpy(&b.oid, b_oid);
	b.mode = b_mode;

	if (rename_conflict_info) {
		struct diff_filepair *pair1 = rename_conflict_info->pair1;

		path1 = (o->branch1 == rename_conflict_info->branch1) ?
			pair1->two->path : pair1->one->path;
		/* If rename_conflict_info->pair2 != NULL, we are in
		 * RENAME_ONE_FILE_TO_ONE case.  Otherwise, we have a
		 * normal rename.
		 */
		path2 = (rename_conflict_info->pair2 ||
			 o->branch2 == rename_conflict_info->branch1) ?
			pair1->two->path : pair1->one->path;

		if (dir_in_way(path, !o->call_depth))
			df_conflict_remains = 1;
	}
	if (merge_file_special_markers(o, &one, &a, &b,
				       o->branch1, path1,
				       o->branch2, path2, &mfi))
		return -1;

	if (mfi.clean && !df_conflict_remains &&
	    oid_eq(&mfi.oid, a_oid) && mfi.mode == a_mode) {
		int path_renamed_outside_HEAD;
		output(o, 3, _("Skipped %s (merged same as existing)"), path);
		/*
		 * The content merge resulted in the same file contents we
		 * already had.  We can return early if those file contents
		 * are recorded at the correct path (which may not be true
		 * if the merge involves a rename).
		 */
		path_renamed_outside_HEAD = !path2 || !strcmp(path, path2);
		if (!path_renamed_outside_HEAD) {
			add_cacheinfo(o, mfi.mode, &mfi.oid, path,
				      0, (!o->call_depth), 0);
			return mfi.clean;
		}
	} else
		output(o, 2, _("Auto-merging %s"), path);

	if (!mfi.clean) {
		if (S_ISGITLINK(mfi.mode))
			reason = _("submodule");
		output(o, 1, _("CONFLICT (%s): Merge conflict in %s"),
				reason, path);
		if (rename_conflict_info && !df_conflict_remains)
			if (update_stages(o, path, &one, &a, &b))
				return -1;
	}

	if (df_conflict_remains) {
		char *new_path;
		if (o->call_depth) {
			remove_file_from_cache(path);
		} else {
			if (!mfi.clean) {
				if (update_stages(o, path, &one, &a, &b))
					return -1;
			} else {
				int file_from_stage2 = was_tracked(path);
				struct diff_filespec merged;
				oidcpy(&merged.oid, &mfi.oid);
				merged.mode = mfi.mode;

				if (update_stages(o, path, NULL,
						  file_from_stage2 ? &merged : NULL,
						  file_from_stage2 ? NULL : &merged))
					return -1;
			}

		}
		new_path = unique_path(o, path, rename_conflict_info->branch1);
		output(o, 1, _("Adding as %s instead"), new_path);
		if (update_file(o, 0, &mfi.oid, mfi.mode, new_path)) {
			free(new_path);
			return -1;
		}
		free(new_path);
		mfi.clean = 0;
	} else if (update_file(o, mfi.clean, &mfi.oid, mfi.mode, path))
		return -1;
	return mfi.clean;
}

/* Per entry merge function */
static int process_entry(struct merge_options *o,
			 const char *path, struct stage_data *entry)
{
	int clean_merge = 1;
	int normalize = o->renormalize;
	unsigned o_mode = entry->stages[1].mode;
	unsigned a_mode = entry->stages[2].mode;
	unsigned b_mode = entry->stages[3].mode;
	struct object_id *o_oid = stage_oid(&entry->stages[1].oid, o_mode);
	struct object_id *a_oid = stage_oid(&entry->stages[2].oid, a_mode);
	struct object_id *b_oid = stage_oid(&entry->stages[3].oid, b_mode);

	entry->processed = 1;
	if (entry->rename_conflict_info) {
		struct rename_conflict_info *conflict_info = entry->rename_conflict_info;
		switch (conflict_info->rename_type) {
		case RENAME_NORMAL:
		case RENAME_ONE_FILE_TO_ONE:
			clean_merge = merge_content(o, path,
						    o_oid, o_mode, a_oid, a_mode, b_oid, b_mode,
						    conflict_info);
			break;
		case RENAME_DELETE:
			clean_merge = 0;
			if (conflict_rename_delete(o,
						   conflict_info->pair1,
						   conflict_info->branch1,
						   conflict_info->branch2))
				clean_merge = -1;
			break;
		case RENAME_ONE_FILE_TO_TWO:
			clean_merge = 0;
			if (conflict_rename_rename_1to2(o, conflict_info))
				clean_merge = -1;
			break;
		case RENAME_TWO_FILES_TO_ONE:
			clean_merge = 0;
			if (conflict_rename_rename_2to1(o, conflict_info))
				clean_merge = -1;
			break;
		default:
			entry->processed = 0;
			break;
		}
	} else if (o_oid && (!a_oid || !b_oid)) {
		/* Case A: Deleted in one */
		if ((!a_oid && !b_oid) ||
		    (!b_oid && blob_unchanged(o, o_oid, o_mode, a_oid, a_mode, normalize, path)) ||
		    (!a_oid && blob_unchanged(o, o_oid, o_mode, b_oid, b_mode, normalize, path))) {
			/* Deleted in both or deleted in one and
			 * unchanged in the other */
			if (a_oid)
				output(o, 2, _("Removing %s"), path);
			/* do not touch working file if it did not exist */
			remove_file(o, 1, path, !a_oid);
		} else {
			/* Modify/delete; deleted side may have put a directory in the way */
			clean_merge = 0;
			if (handle_modify_delete(o, path, o_oid, o_mode,
						 a_oid, a_mode, b_oid, b_mode))
				clean_merge = -1;
		}
	} else if ((!o_oid && a_oid && !b_oid) ||
		   (!o_oid && !a_oid && b_oid)) {
		/* Case B: Added in one. */
		/* [nothing|directory] -> ([nothing|directory], file) */

		const char *add_branch;
		const char *other_branch;
		unsigned mode;
		const struct object_id *oid;
		const char *conf;

		if (a_oid) {
			add_branch = o->branch1;
			other_branch = o->branch2;
			mode = a_mode;
			oid = a_oid;
			conf = _("file/directory");
		} else {
			add_branch = o->branch2;
			other_branch = o->branch1;
			mode = b_mode;
			oid = b_oid;
			conf = _("directory/file");
		}
		if (dir_in_way(path, !o->call_depth)) {
			char *new_path = unique_path(o, path, add_branch);
			clean_merge = 0;
			output(o, 1, _("CONFLICT (%s): There is a directory with name %s in %s. "
			       "Adding %s as %s"),
			       conf, path, other_branch, path, new_path);
			if (update_file(o, 0, oid, mode, new_path))
				clean_merge = -1;
			else if (o->call_depth)
				remove_file_from_cache(path);
			free(new_path);
		} else {
			output(o, 2, _("Adding %s"), path);
			/* do not overwrite file if already present */
			if (update_file_flags(o, oid, mode, path, 1, !a_oid))
				clean_merge = -1;
		}
	} else if (a_oid && b_oid) {
		/* Case C: Added in both (check for same permissions) and */
		/* case D: Modified in both, but differently. */
		clean_merge = merge_content(o, path,
					    o_oid, o_mode, a_oid, a_mode, b_oid, b_mode,
					    NULL);
	} else if (!o_oid && !a_oid && !b_oid) {
		/*
		 * this entry was deleted altogether. a_mode == 0 means
		 * we had that path and want to actively remove it.
		 */
		remove_file(o, 1, path, !a_mode);
	} else
		die("BUG: fatal merge failure, shouldn't happen.");

	return clean_merge;
}

int merge_trees(struct merge_options *o,
		struct tree *head,
		struct tree *merge,
		struct tree *common,
		struct tree **result)
{
	int code, clean;

	if (o->subtree_shift) {
		merge = shift_tree_object(head, merge, o->subtree_shift);
		common = shift_tree_object(head, common, o->subtree_shift);
	}

	if (oid_eq(&common->object.oid, &merge->object.oid)) {
		output(o, 0, _("Already up-to-date!"));
		*result = head;
		return 1;
	}

	code = git_merge_trees(o->call_depth, common, head, merge);

	if (code != 0) {
		if (show(o, 4) || o->call_depth)
			err(o, _("merging of trees %s and %s failed"),
			    oid_to_hex(&head->object.oid),
			    oid_to_hex(&merge->object.oid));
		return -1;
	}

	if (unmerged_cache()) {
		struct string_list *entries, *re_head, *re_merge;
		int i;
		string_list_clear(&o->current_file_set, 1);
		string_list_clear(&o->current_directory_set, 1);
		get_files_dirs(o, head);
		get_files_dirs(o, merge);

		entries = get_unmerged();
		record_df_conflict_files(o, entries);
		re_head  = get_renames(o, head, common, head, merge, entries);
		re_merge = get_renames(o, merge, common, head, merge, entries);
		clean = process_renames(o, re_head, re_merge);
		if (clean < 0)
			return clean;
		for (i = entries->nr-1; 0 <= i; i--) {
			const char *path = entries->items[i].string;
			struct stage_data *e = entries->items[i].util;
			if (!e->processed) {
				int ret = process_entry(o, path, e);
				if (!ret)
					clean = 0;
				else if (ret < 0)
					return ret;
			}
		}
		for (i = 0; i < entries->nr; i++) {
			struct stage_data *e = entries->items[i].util;
			if (!e->processed)
				die("BUG: unprocessed path??? %s",
				    entries->items[i].string);
		}

		string_list_clear(re_merge, 0);
		string_list_clear(re_head, 0);
		string_list_clear(entries, 1);

		free(re_merge);
		free(re_head);
		free(entries);
	}
	else
		clean = 1;

	if (o->call_depth && !(*result = write_tree_from_memory(o)))
		return -1;

	return clean;
}

static struct commit_list *reverse_commit_list(struct commit_list *list)
{
	struct commit_list *next = NULL, *current, *backup;
	for (current = list; current; current = backup) {
		backup = current->next;
		current->next = next;
		next = current;
	}
	return next;
}

/*
 * Merge the commits h1 and h2, return the resulting virtual
 * commit object and a flag indicating the cleanness of the merge.
 */
int merge_recursive(struct merge_options *o,
		    struct commit *h1,
		    struct commit *h2,
		    struct commit_list *ca,
		    struct commit **result)
{
	struct commit_list *iter;
	struct commit *merged_common_ancestors;
	struct tree *mrtree = mrtree;
	int clean;

	if (show(o, 4)) {
		output(o, 4, _("Merging:"));
		output_commit_title(o, h1);
		output_commit_title(o, h2);
	}

	if (!ca) {
		ca = get_merge_bases(h1, h2);
		ca = reverse_commit_list(ca);
	}

	if (show(o, 5)) {
		unsigned cnt = commit_list_count(ca);

		output(o, 5, Q_("found %u common ancestor:",
				"found %u common ancestors:", cnt), cnt);
		for (iter = ca; iter; iter = iter->next)
			output_commit_title(o, iter->item);
	}

	merged_common_ancestors = pop_commit(&ca);
	if (merged_common_ancestors == NULL) {
		/* if there is no common ancestor, use an empty tree */
		struct tree *tree;

		tree = lookup_tree(EMPTY_TREE_SHA1_BIN);
		merged_common_ancestors = make_virtual_commit(tree, "ancestor");
	}

	for (iter = ca; iter; iter = iter->next) {
		const char *saved_b1, *saved_b2;
		o->call_depth++;
		/*
		 * When the merge fails, the result contains files
		 * with conflict markers. The cleanness flag is
		 * ignored (unless indicating an error), it was never
		 * actually used, as result of merge_trees has always
		 * overwritten it: the committed "conflicts" were
		 * already resolved.
		 */
		discard_cache();
		saved_b1 = o->branch1;
		saved_b2 = o->branch2;
		o->branch1 = "Temporary merge branch 1";
		o->branch2 = "Temporary merge branch 2";
		if (merge_recursive(o, merged_common_ancestors, iter->item,
				    NULL, &merged_common_ancestors) < 0)
			return -1;
		o->branch1 = saved_b1;
		o->branch2 = saved_b2;
		o->call_depth--;

		if (!merged_common_ancestors)
			return err(o, _("merge returned no commit"));
	}

	discard_cache();
	if (!o->call_depth)
		read_cache();

	o->ancestor = "merged common ancestors";
	clean = merge_trees(o, h1->tree, h2->tree, merged_common_ancestors->tree,
			    &mrtree);
	if (clean < 0) {
		flush_output(o);
		return clean;
	}

	if (o->call_depth) {
		*result = make_virtual_commit(mrtree, "merged tree");
		commit_list_insert(h1, &(*result)->parents);
		commit_list_insert(h2, &(*result)->parents->next);
	}
	flush_output(o);
	if (!o->call_depth && o->buffer_output < 2)
		strbuf_release(&o->obuf);
	if (show(o, 2))
		diff_warn_rename_limit("merge.renamelimit",
				       o->needed_rename_limit, 0);
	return clean;
}

static struct commit *get_ref(const struct object_id *oid, const char *name)
{
	struct object *object;

	object = deref_tag(parse_object(oid->hash), name, strlen(name));
	if (!object)
		return NULL;
	if (object->type == OBJ_TREE)
		return make_virtual_commit((struct tree*)object, name);
	if (object->type != OBJ_COMMIT)
		return NULL;
	if (parse_commit((struct commit *)object))
		return NULL;
	return (struct commit *)object;
}

int merge_recursive_generic(struct merge_options *o,
			    const struct object_id *head,
			    const struct object_id *merge,
			    int num_base_list,
			    const struct object_id **base_list,
			    struct commit **result)
{
	int clean;
	struct lock_file *lock = xcalloc(1, sizeof(struct lock_file));
	struct commit *head_commit = get_ref(head, o->branch1);
	struct commit *next_commit = get_ref(merge, o->branch2);
	struct commit_list *ca = NULL;

	if (base_list) {
		int i;
		for (i = 0; i < num_base_list; ++i) {
			struct commit *base;
			if (!(base = get_ref(base_list[i], oid_to_hex(base_list[i]))))
				return err(o, _("Could not parse object '%s'"),
					oid_to_hex(base_list[i]));
			commit_list_insert(base, &ca);
		}
	}

	hold_locked_index(lock, 1);
	clean = merge_recursive(o, head_commit, next_commit, ca,
			result);
	if (clean < 0)
		return clean;

	if (active_cache_changed &&
	    write_locked_index(&the_index, lock, COMMIT_LOCK))
		return err(o, _("Unable to write index."));

	return clean ? 0 : 1;
}

static void merge_recursive_config(struct merge_options *o)
{
	git_config_get_int("merge.verbosity", &o->verbosity);
	git_config_get_int("diff.renamelimit", &o->diff_rename_limit);
	git_config_get_int("merge.renamelimit", &o->merge_rename_limit);
	git_config(git_xmerge_config, NULL);
}

void init_merge_options(struct merge_options *o)
{
	memset(o, 0, sizeof(struct merge_options));
	o->verbosity = 2;
	o->buffer_output = 1;
	o->diff_rename_limit = -1;
	o->merge_rename_limit = -1;
	o->renormalize = 0;
	o->detect_rename = 1;
	merge_recursive_config(o);
	if (getenv("GIT_MERGE_VERBOSITY"))
		o->verbosity =
			strtol(getenv("GIT_MERGE_VERBOSITY"), NULL, 10);
	if (o->verbosity >= 5)
		o->buffer_output = 0;
	strbuf_init(&o->obuf, 0);
	string_list_init(&o->current_file_set, 1);
	string_list_init(&o->current_directory_set, 1);
	string_list_init(&o->df_conflict_file_set, 1);
}

int parse_merge_opt(struct merge_options *o, const char *s)
{
	const char *arg;

	if (!s || !*s)
		return -1;
	if (!strcmp(s, "ours"))
		o->recursive_variant = MERGE_RECURSIVE_OURS;
	else if (!strcmp(s, "theirs"))
		o->recursive_variant = MERGE_RECURSIVE_THEIRS;
	else if (!strcmp(s, "subtree"))
		o->subtree_shift = "";
	else if (skip_prefix(s, "subtree=", &arg))
		o->subtree_shift = arg;
	else if (!strcmp(s, "patience"))
		o->xdl_opts = DIFF_WITH_ALG(o, PATIENCE_DIFF);
	else if (!strcmp(s, "histogram"))
		o->xdl_opts = DIFF_WITH_ALG(o, HISTOGRAM_DIFF);
	else if (skip_prefix(s, "diff-algorithm=", &arg)) {
		long value = parse_algorithm_value(arg);
		if (value < 0)
			return -1;
		/* clear out previous settings */
		DIFF_XDL_CLR(o, NEED_MINIMAL);
		o->xdl_opts &= ~XDF_DIFF_ALGORITHM_MASK;
		o->xdl_opts |= value;
	}
	else if (!strcmp(s, "ignore-space-change"))
		o->xdl_opts |= XDF_IGNORE_WHITESPACE_CHANGE;
	else if (!strcmp(s, "ignore-all-space"))
		o->xdl_opts |= XDF_IGNORE_WHITESPACE;
	else if (!strcmp(s, "ignore-space-at-eol"))
		o->xdl_opts |= XDF_IGNORE_WHITESPACE_AT_EOL;
	else if (!strcmp(s, "renormalize"))
		o->renormalize = 1;
	else if (!strcmp(s, "no-renormalize"))
		o->renormalize = 0;
	else if (!strcmp(s, "no-renames"))
		o->detect_rename = 0;
	else if (!strcmp(s, "find-renames")) {
		o->detect_rename = 1;
		o->rename_score = 0;
	}
	else if (skip_prefix(s, "find-renames=", &arg) ||
		 skip_prefix(s, "rename-threshold=", &arg)) {
		if ((o->rename_score = parse_rename_score(&arg)) == -1 || *arg != 0)
			return -1;
		o->detect_rename = 1;
	}
	else
		return -1;
	return 0;
}
