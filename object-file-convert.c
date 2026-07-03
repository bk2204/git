#define DISABLE_SIGN_COMPARE_WARNINGS

#include "git-compat-util.h"
#include "gettext.h"
#include "error.h"
#include "strbuf.h"
#include "hex.h"
#include "repository.h"
#include "hash.h"
#include "object.h"
#include "object-file.h"
#include "loose.h"
#include "commit.h"
#include "gpg-interface.h"
#include "object-file-convert.h"
#include "odb.h"
#include "packfile.h"

static int repo_packed_object_map_oid(struct repository *repo,
				      const struct object_id *src,
				      const struct git_hash_algo *dest_algo,
				      struct object_id *dest)
{
	struct packed_git *p;
	repo_for_each_pack(repo, p) {
		uint32_t off;
		const struct git_hash_algo *src_algo = src->algo ?
						       &hash_algos[src->algo] :
						       repo->hash_algo;

		if (open_pack_index(p) || !p->num_objects)
			continue;
		if (!bsearch_pack(src, p, &off))
			continue;
		return nth_packed_object_id_algop(dest, p, off, src_algo, dest_algo);
	}
	return -1;
}


int repo_oid_to_algop(struct repository *repo, const struct object_id *srcoid,
		      const struct git_hash_algo *to, struct object_id *dest)
{
	/*
	 * If the source algorithm is not set, then we're using the
	 * default hash algorithm for that object.
	 */
	const struct git_hash_algo *from =
		srcoid->algo ? &hash_algos[srcoid->algo] : repo->hash_algo;
	struct object_id temp;
	const struct object_id *src = srcoid;

	if (!srcoid->algo) {
		oidcpy(&temp, srcoid);
		temp.algo = hash_algo_by_ptr(repo->hash_algo);
		src = &temp;
	}

	if (from == to || !to) {
		if (src != dest)
			oidcpy(dest, src);
		return 0;
	}
	if (repo_loose_object_map_oid(repo, src, to, dest, 0)) {
		/*
		 * It's not in the loose object map, so let's see if it's in a
		 * pack.
		 */
		if (!repo_packed_object_map_oid(repo, src, to, dest))
			return 0;

		/*
		 * We may have loaded the object map at repo initialization but
		 * another process (perhaps upstream of a pipe from us) may have
		 * written a new object into the map.  If the object is missing,
		 * let's reload the map to see if the object has appeared.
		 */
		repo_read_loose_object_map(repo);
		if (repo_loose_object_map_oid(repo, src, to, dest, 0))
			return -1;
	}
	return 0;
}

struct git_err convert_object_file_1(struct repository *repo,
				    struct strbuf *outbuf,
				    const struct git_hash_algo *from,
				    const struct git_hash_algo *to,
				    const void *buf, size_t len,
				    enum object_type type,
				    int flags);

int convert_object_file(struct repository *repo,
			struct strbuf *outbuf,
			const struct git_hash_algo *from,
			const struct git_hash_algo *to,
			const void *buf, size_t len,
			enum object_type type,
			struct missing_object *missing,
			int flags)
{
	int ret = -1;
	struct git_err err;

	/* Don't call this function when no conversion is necessary */
	if ((from == to) || (type == OBJ_BLOB))
		BUG("Refusing noop object file conversion");

#ifdef WITH_RUST
	err = convert_object_file_1(repo, outbuf, from, to, buf, len, type, flags);
	switch (git_err_code(err)) {
		case 0:
			ret = 0;
			break;
		case GIT_ERRC_MISSING_OBJECT_IN_CONVERSION:
			if (missing) {
				struct conversion_error *e = err.value.ptr;
				*missing = e->missing_object;
				if (!missing->type)
					missing->type = OBJ_ANY;
				ret = -2;
				break;
			}
			/* fallthrough */
		default:
			git_errf(err);
			ret = -1;
			break;
	}
	git_err_free(&err);
#endif
	if (!ret)
		return 0;
	if (flags & OBJ_CONVERT_GENTLE) {
		strbuf_release(outbuf);
		return ret;
	}
	die(_("Failed to convert object from %s to %s"),
		from->name, to->name);
}

int convert_and_hash_object_file(struct repository *repo,
				 const struct git_hash_algo *from,
				 const struct git_hash_algo *to,
				 const void *buf, size_t len,
				 enum object_type type,
				 struct object_id *oid,
				 int flags)
{
	struct strbuf strbuf = STRBUF_INIT;
	size_t hdrlen;
	unsigned char hdr[96];
	struct git_hash_ctx c;

	if (type != OBJ_BLOB) {
		int ret = 0;
		ret = convert_object_file(repo, &strbuf, from, to, buf, len,
					  type, NULL, flags);
		if (ret)
			return ret;
		buf = strbuf.buf;
		len = strbuf.len;
	}

	hdrlen = format_object_header((char *)hdr, sizeof(hdr), type,
				      len);

	to->init_fn(&c);
	git_hash_update(&c, hdr, hdrlen);
	git_hash_update(&c, buf, len);
	git_hash_final_oid(oid, &c);

	return 0;
}
