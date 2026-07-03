#include "git-compat-util.h"
#include "gettext.h"
#include "error.h"
#include "hex.h"
#include "strbuf.h"

static bool format_error(struct strbuf *buf, struct git_err err)
{
	void *p = err.value.ptr;
	struct conversion_error *conv_err = NULL;

	if (err.code & GIT_ERR_BIT_IS_ERRNO) {
		strbuf_addstr(buf, strerror(err.code & ~GIT_ERR_BITMASK));
		return true;
	}
	switch (err.code & ~GIT_ERR_BITMASK) {
	case GIT_ERRC_INVALID_HASH_ALGORITHM:
		strbuf_addf(buf, _("invalid hash algorithm %d"), err.value.hash_algo);
		return true;
	case GIT_ERRC_INVALID_HEX_OBJECT_ID_WITH_INVALID_CHAR:
		strbuf_addf(buf, _("invalid hex object ID at character %" PRIuMAX), (uintmax_t)err.value.offset);
		return true;
	case GIT_ERRC_INVALID_TRUNCATED_HEX_OBJECT_ID:
		strbuf_addf(buf, _("truncated hex object ID"));
		return true;
	case GIT_ERRC_MISSING_OBJECT_IN_CONVERSION:
		conv_err = p;
		strbuf_addf(buf, _("unable to map %.*s %s in %s object"),
			   (int)conv_err->header_len, conv_err->header, oid_to_hex(&conv_err->missing_object.oid),
			   type_name(conv_err->type));
		return true;
	case GIT_ERRC_INVALID_TREE_ENTRY:
		conv_err = p;
		strbuf_addf(buf, _("failed to decode tree entry"));
		return true;
	case GIT_ERRC_INVALID_OBJECT:
		conv_err = p;
		strbuf_addf(buf, _("invalid object of type %s"), type_name(conv_err->type));
		return true;
	case GIT_ERRC_UNKNOWN_OBJECT_HEADER:
		conv_err = p;
		strbuf_addf(buf, _("unknown header in %s object"), type_name(conv_err->type));
		return true;
	case GIT_ERRC_INVALID_OBJECT_HEADER:
		conv_err = p;
		strbuf_addf(buf, _("invalid or corrupt header in %s object"), type_name(conv_err->type));
		return true;
	case GIT_ERRC_INVALID_OBJECT_ID_IN_HEADER:
		conv_err = p;
		strbuf_addf(buf, _("invalid object ID in %.*s header in %s object"),
			   (int)conv_err->header_len, conv_err->header, type_name(conv_err->type));
		return true;
	case GIT_ERRC_INVALID_OBJECT_TYPE:
		strbuf_addf(buf, _("invalid object type %" PRIu64), err.value.u64val);
		return true;
	default:
		return false;
	}
}

char *git_aerrf(struct git_err err)
{
	struct strbuf buf = STRBUF_INIT;

	if (format_error(&buf, err))
		return strbuf_detach(&buf, NULL);
	strbuf_release(&buf);
	return NULL;
}

struct git_err git_rerrf(struct git_err err)
{
	git_errf(err);
	return err;
}

void git_errf(struct git_err err)
{
	struct strbuf buf = STRBUF_INIT;

	format_error(&buf, err);
	error("%s", buf.buf);
	strbuf_release(&buf);
}
