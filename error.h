#ifndef ERROR_H
#define ERROR_H

#include "git-compat-util.h"
#include "object.h"
#include "object-file-convert.h"

#define GIT_ERR_BIT_IS_ERRNO 0x8000000000000000
#define GIT_ERR_BIT_IS_ALLOCATED 0x4000000000000000
#define GIT_ERR_BIT_IS_ALLOCATED_RUST 0x2000000000000000
#define GIT_ERR_BITMASK (GIT_ERR_BIT_IS_ERRNO|GIT_ERR_BIT_IS_ALLOCATED|GIT_ERR_BIT_IS_ALLOCATED_RUST)

#define GIT_ERR_OK() (git_err_new_ok())
#define GIT_ERR_FREE(err) (git_err_free(&err))

enum git_err_const {
	GIT_ERRC_ERRNO = 1,
	GIT_ERRC_INVALID_HASH_ALGORITHM = 2,
	GIT_ERRC_INVALID_HEX_OBJECT_ID_WITH_INVALID_CHAR = 3,
	GIT_ERRC_INVALID_TRUNCATED_HEX_OBJECT_ID = 4,
	GIT_ERRC_MISSING_OBJECT_IN_CONVERSION = 5,
	GIT_ERRC_INVALID_TREE_ENTRY = 6,
	GIT_ERRC_INVALID_OBJECT = 7,
	GIT_ERRC_UNKNOWN_OBJECT_HEADER = 8,
	GIT_ERRC_INVALID_OBJECT_HEADER = 9,
	GIT_ERRC_INVALID_OBJECT_ID_IN_HEADER = 10,
	GIT_ERRC_INVALID_OBJECT_TYPE = 11,
};

union git_err_value {
	uint32_t hash_algo;
	void *ptr;
	uint64_t u64val;
	size_t offset;
};

struct git_err {
	uint64_t code;
	union git_err_value value;
};

struct conversion_error {
	enum object_type type;
	const char *header;
	size_t header_len;
	struct missing_object missing_object;
};

/* Return an error value indicating success. */
static inline struct git_err git_err_new_ok(void)
{
	struct git_err err;
	memset(&err, 0, sizeof(err));
	return err;
}

/* Return an error value indicating an errno value. */
static inline struct git_err git_err_new_errno(int errnov)
{
	struct git_err err = {
		((uint32_t)errnov) | GIT_ERR_BIT_IS_ERRNO,
		{0}
	};
	return err;
}

void git_err_free_1(struct git_err *err);

/* Free and reset the error pointed to by err. */
static inline void git_err_free(struct git_err *err)
{
	if (err->code & GIT_ERR_BIT_IS_ALLOCATED) {
		if (err->code & GIT_ERR_BIT_IS_ALLOCATED_RUST) {
#ifdef WITH_RUST
			git_err_free_1(err);
#endif
		} else {
			free(err->value.ptr);
		}
	}
	memset(err, 0, sizeof(*err));
}

/* Return the errno value for this error.
 *
 * If it is a success, returns 0; otherwise, if it's an errno value, returns
 * that.  If it is neither, returns -1.
 */
static inline int git_err_errno(struct git_err err)
{
	if (err.code == 0)
		return 0;
	if (err.code & GIT_ERR_BIT_IS_ERRNO)
		return err.code & ~GIT_ERR_BITMASK;
	return -1;
}

/* Return the code for this error.
 *
 * If it is a success, returns 0; otherwise, if it's an errno value, returns
 * GIT_ERRC_ERRNO.  If it is neither, returns the code value for the error.
 */
static inline int git_err_code(struct git_err err)
{
	if (err.code == 0)
		return 0;
	if (err.code & GIT_ERR_BIT_IS_ERRNO)
		return GIT_ERRC_ERRNO;
	return err.code & ~GIT_ERR_BITMASK;
}

/* Format an error to a string.  The return value should be freed with free. */
char *git_aerrf(struct git_err err);

/*
 * Format an error to the standard error routine.  The return value is err.
 * This function exists to print an error to standard error and propagate it up
 * the call stack.
 */
struct git_err git_rerrf(struct git_err err);

/* Format an error to the standard error routine. */
void git_errf(struct git_err err);

#endif
