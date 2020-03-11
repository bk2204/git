#ifndef DATA_BUFFER_H
#define DATA_BUFFER_H

#include "git-compat-util.h"
#include "strbuf.h"
#include "tempfile.h"

enum data_buffer_mode {
	BUFFER_MODE_STRBUF = 0,
	BUFFER_MODE_TEMPFILE,
};

struct data_buffer {
	enum data_buffer_mode mode;
	size_t offset;
	struct tempfile *file;
	struct strbuf buf;
};

#define DATA_BUFFER_INIT { .mode = BUFFER_MODE_STRBUF, .offset = 0, .file = NULL, .buf = STRBUF_INIT }

/*
 * Initialize a data buffer for use.  Calling this function is not necessary if
 * the buffer has been initialized with DATA_BUFFER_INIT.
 */
void data_buffer_init(struct data_buffer *b);
/*
 * Initialize a data buffer for use based on an existing strbuf.  buf is reset
 * to an empty strbuf.
 */
void data_buffer_init_from_strbuf(struct data_buffer *b, struct strbuf *buf);
/*
 * Free resources and return the buffer to its initial state.  It may be reused
 * after this, but will not contain any file descriptors or heap-allocated
 * storage unless more data is added.
 */
void data_buffer_free(struct data_buffer *b);
/*
 * Move the contents of this buffer to a strbuf.  Note that in general, this is
 * a bad idea, since the contents of a data buffer may be large.  However, this
 * function is provided for compatibility with existing APIs until they can be
 * ported over.
 */
int data_buffer_to_strbuf(struct data_buffer *b, struct strbuf *buf);
off_t data_buffer_write_from_fd(struct data_buffer *b, int fd);

ssize_t data_buffer_read(struct data_buffer *b, void *buf, size_t len);
ssize_t data_buffer_write(struct data_buffer *b, const void *buf, size_t len);
off_t data_buffer_seek(struct data_buffer *b, off_t offset, int whence);
off_t data_buffer_length(struct data_buffer *b);

#endif
