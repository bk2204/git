#include "cache.h"
#include "data-buffer.h"

void data_buffer_init(struct data_buffer *b)
{
	memset(b, 0, sizeof(*b));
	strbuf_init(&b->buf, 0);
}

void data_buffer_init_from_strbuf(struct data_buffer *b, struct strbuf *buf)
{
	data_buffer_init(b);
	strbuf_swap(&b->buf, buf);
}

void data_buffer_free(struct data_buffer *b)
{
	if (b->file)
		delete_tempfile(&b->file);
	strbuf_release(&b->buf);
	data_buffer_init(b);
}

int data_buffer_to_strbuf(struct data_buffer *b, struct strbuf *buf)
{
	if (b->mode == BUFFER_MODE_STRBUF) {
		strbuf_swap(&b->buf, buf);
		strbuf_release(&b->buf);
		data_buffer_free(b);
		return 0;
	}
	data_buffer_seek(b, 0, SEEK_SET);
	strbuf_reset(buf);
	if (strbuf_read(buf, b->file->fd, data_buffer_length(b)) < 0)
		return -1;
	data_buffer_free(b);
	return 0;
}

ssize_t data_buffer_read(struct data_buffer *b, void *buf, size_t len)
{
	size_t chunk;

	if (!len)
		return 0;

	if (b->mode == BUFFER_MODE_TEMPFILE)
		return xread(b->file->fd, buf, len);

	chunk = b->buf.len - b->offset;
	if (len < chunk)
		chunk = len;
	memcpy(buf, b->buf.buf + b->offset, chunk);
	b->offset += chunk;
	return chunk;
}

off_t data_buffer_write_from_fd(struct data_buffer *b, int fd)
{
	off_t total = 0;
	char buf[8192];
	ssize_t nread;

	do {
		nread = xread(fd, buf, sizeof(buf));
		if (nread < 0)
			return -1;
		total += nread;

		if (data_buffer_write(b, buf, nread) < 0)
			return -1;
	} while (nread);
	return total;
}


ssize_t data_buffer_write(struct data_buffer *b, const void *buf, size_t len)
{
	ssize_t ret;

	if (!len)
		return 0;

	if (b->mode == BUFFER_MODE_TEMPFILE)
		return write_in_full(b->file->fd, buf, len);
	if (b->offset + len <= big_file_threshold) {
		if (b->offset == b->buf.len)
			strbuf_add(&b->buf, buf, len);
		else {
			strbuf_remove(&b->buf, b->offset, len);
			strbuf_insert(&b->buf, b->offset, buf, len);
		}
		b->offset += len;
		return len;
	}

	/*
	 * We're larger than the threshold here, so let's dump to a file and
	 * switch modes.
	 */
	b->file = mks_tempfile_t("data-buffer.XXXXXX");
	if (!b->file)
		return -1;
	if (write_in_full(b->file->fd, b->buf.buf, b->buf.len) < 0)
		return -1;
	strbuf_release(&b->buf);
	b->mode = BUFFER_MODE_TEMPFILE;
	if (lseek(b->file->fd, b->offset, SEEK_SET) < 0)
		return -1;
	ret = write_in_full(b->file->fd, buf, len);
	return ret;
}

off_t data_buffer_length(struct data_buffer *b)
{
	struct stat st;
	switch (b->mode) {
	case BUFFER_MODE_STRBUF:
		return b->buf.len;
	case BUFFER_MODE_TEMPFILE:
		if (fstat(b->file->fd, &st) < 0)
			return -1;
		return st.st_size;
	default:
		BUG("data_buffer_length: unexpected mode type");
	}
}

off_t data_buffer_seek(struct data_buffer *b, off_t offset, int whence)
{
	off_t computed;
	if (b->mode == BUFFER_MODE_TEMPFILE)
		return lseek(b->file->fd, offset, whence);

	switch (whence) {
	case SEEK_SET:
		if (offset > b->buf.len || offset < 0)
			return -1;
		b->offset = offset;
		break;
	case SEEK_CUR:
		computed = b->offset + offset;
		if (computed > b->buf.len || computed < 0)
			return -1;
		b->offset = computed;
		break;
	case SEEK_END:
		computed = b->buf.len + offset;
		if (computed > b->buf.len || computed < 0)
			return -1;
		b->offset = computed;
		break;
	default:
		BUG("data_buffer_seek: unexpected whence value");
	}
	return b->offset;
}

