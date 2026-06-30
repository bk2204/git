#include "git-compat-util.h"
#include "gettext.h"
#include "error.h"
#include "strbuf.h"

static bool format_error(struct strbuf *buf, struct git_err err)
{
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
