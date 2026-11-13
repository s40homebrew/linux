/* SPDX-License-Identifier: MIT */
/* Session-oriented client for the RM-443 privileged experiment interface. */
#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../overlay/include/uapi/linux/rm443_probe.h"

#define NAME_BYTES 32
#define MAX_WORDS 40
#define MAX_LINE 4096
struct named_buffer {
	char name[NAME_BYTES];
	struct rm443_probe_buffer buffer;
	struct rm443_probe_object object;
};
struct session {
	int fd;
	struct rm443_probe_info info;
	FILE *input;
	int binary_stdout;
	struct named_buffer buffers[RM443_PROBE_MAX_BUFFERS];
};

static FILE *status_output;
static FILE *status_stream(void) { return status_output ? status_output : stdout; }

static int fail(const char *format, ...)
{
	va_list ap;
	fputs("rmprobe: ", stderr);
	va_start(ap, format);
	vfprintf(stderr, format, ap);
	va_end(ap);
	fputc('\n', stderr);
	return -1;
}

static int request(struct session *s, unsigned long operation, void *arg)
{
	/* Never retry CALL: an interrupted/failed result copy may follow execution. */
	if (ioctl(s->fd, operation, arg) < 0)
		return fail("ioctl: %s", strerror(errno));
	return 0;
}

static int number(const char *text, uint32_t *value)
{
	char *end;
	unsigned long long parsed;
	if (!text[0] || text[0] == '-' || text[0] == '+' || isspace((unsigned char)text[0]))
		return -1;
	errno = 0;
	parsed = strtoull(text, &end, text[0] == '0' && (text[1] == 'x' || text[1] == 'X') ? 16 : 10);
	if (errno || *end || parsed > UINT32_MAX)
		return -1;
	*value = parsed;
	return 0;
}

static struct named_buffer *named(struct session *s, const char *name)
{
	unsigned int i;
	for (i = 0; i < RM443_PROBE_MAX_BUFFERS; i++)
		if (s->buffers[i].name[0] && !strcmp(s->buffers[i].name, name))
			return &s->buffers[i];
	return NULL;
}

static int address_mode(struct session *s, const char *text, uint32_t *value, int allow_virtual)
{
	char name[NAME_BYTES];
	const char *plus;
	struct named_buffer *b;
	uint32_t offset = 0;
	size_t length;
	if (text[0] != '@' && text[0] != '%')
		return number(text, value);
	if (text[0] == '%' && !allow_virtual) return -1;
	plus = strchr(text + 1, '+');
	length = plus ? (size_t)(plus - text - 1) : strlen(text + 1);
	if (!length || length >= sizeof(name) || (plus && number(plus + 1, &offset)))
		return -1;
	memcpy(name, text + 1, length);
	name[length] = 0;
	b = named(s, name);
	if (!b || offset >= b->buffer.bytes) return -1;
	uint32_t base;
	if (text[0] == '%') {
		if (!b->object.bridge_va) return -1;
		base = b->object.bridge_va;
	} else {
		if (b->object.kind != RM443_PROBE_OBJECT_CONTIG) return -1;
		base = b->buffer.physical;
	}
	if (offset > UINT32_MAX - base) return -1;
	*value = base + offset;
	return 0;
}

static int address(struct session *s, const char *text, uint32_t *value)
{
	return address_mode(s, text, value, 1);
}

static int name_valid(const char *name)
{
	size_t i, n = strlen(name);
	if (!n || n >= NAME_BYTES || (!isalpha((unsigned char)name[0]) && name[0] != '_'))
		return 0;
	for (i = 1; i < n; i++)
		if (!isalnum((unsigned char)name[i]) && name[i] != '_')
			return 0;
	return 1;
}

/* No shell expansion or subprocesses. Quotes and backslash escape filenames. */
static int split(char *line, char **words)
{
	char *read = line, *write = line;
	int count = 0;
	while (*read) {
		int quote = 0;
		while (isspace((unsigned char)*read)) read++;
		if (!*read || *read == '#') break;
		if (count == MAX_WORDS) return fail("too many words");
		words[count++] = write;
		while (*read) {
			char c = *read++;
			if (!quote && isspace((unsigned char)c)) break;
			if (c == '\\' && quote != '\'') {
				if (!*read) return fail("trailing backslash");
				*write++ = *read++;
			} else if (c == '\'' || c == '"') {
				if (!quote) quote = c;
				else if (quote == c) quote = 0;
				else *write++ = c;
			} else *write++ = c;
		}
		if (quote) return fail("unclosed quote");
		*write++ = 0;
	}
	return count;
}

static void help(void)
{
	fputs("rmprobe [--binary] [--fifo PATH | -f SCRIPT | -c COMMAND | COMMAND ARGS...]\n"
	     "With no arguments, read a persistent session from stdin.\n"
	     "  info | buffers | record | help | quit\n"
	     "  alloc NAME BYTES\n"
	     "  paged NAME BYTES | flash NAME NOR_PHYSICAL BYTES\n"
	     "  map NAME | unmap NAME | pages NAME\n"
	     "  budget [BYTES [HEADROOM_BYTES]]\n"
	     "  free NAME\n"
	     "  upload NAME FILE [OFFSET]\n"
	     "  download NAME NEWFILE [OFFSET [BYTES]]\n"
	     "  import NAME PATH OFFSET BYTES | export NAME PATH OFFSET BYTES\n"
	     "  puthex NAME OFFSET HEX | gethex NAME OFFSET BYTES\n"
	     "  copy NAME OFFSET NOR_PHYSICAL BYTES\n"
	     "  call descriptor=W0,W1,W2,W3 [entry=ADDR] [r1=VALUE ... r12=VALUE] [stack=V0,V1,...]\n"
	     "  call descriptor=file:FILE [entry=ADDR] [r1=VALUE ...] [stack=V0,V1,...]\n"
	     "  exec ENTRY [r0=VALUE ... r12=VALUE] [stack=V0,V1,...] (execva for numeric bridge VA)\n"
	     "  read physical|bridge ADDRESS WIDTH BYTES [NEWFILE]\n"
	     "Numbers: decimal or 0xHEX. @NAME+OFFSET is contiguous physical; %NAME+OFFSET is bridge VA.\n"
	     "Paged/flash objects have no contiguous physical address. ROM calls reject % references.\n"
	     "import/export accept FIFOs. '-' import consumes exactly BYTES from command input.\n"
	     "'-' export requires --binary, which routes all text to stderr.\n"
	     "ROM entry defaults to 0xffff0809; r0 is the descriptor address.\n"
	     "Entry bit 0 selects Thumb; ARM entries must be word-aligned.\n"
	     "Close/quit frees session buffers. No command is executed automatically.\n", status_stream());
}

static void print_buffer(const struct named_buffer *b)
{
	const struct rm443_probe_object *o = &b->object;
	const char *kind = o->kind == RM443_PROBE_OBJECT_CONTIG ? "contiguous" :
		o->kind == RM443_PROBE_OBJECT_PAGED ? "paged" : "flash-view";
	fprintf(status_stream(), "%s id=%u kind=%s bytes=%u resident=%u pages=%u ",
	       b->name, b->buffer.id, kind, b->buffer.bytes, o->resident_bytes, o->page_count);
	if (o->kind == RM443_PROBE_OBJECT_CONTIG)
		fprintf(status_stream(), "physical=0x%08x ", b->buffer.physical);
	else fputs("physical=none ", status_stream());
	if (o->bridge_va) fprintf(status_stream(), "bridge_va=0x%08x ", o->bridge_va);
	else fputs("bridge_va=none ", status_stream());
	if (o->kind == RM443_PROBE_OBJECT_FLASH)
		fprintf(status_stream(), "flash_backing=0x%08x ", o->backing_physical);
	fputc('\n', status_stream());
}

static int refresh_object(struct session *s, struct named_buffer *b);

static int alloc_buffer(struct session *s, int argc, char **argv)
{
	struct rm443_probe_buffer b = {0};
	unsigned int i;
	if (argc != 3 || !name_valid(argv[1]) || number(argv[2], &b.bytes) || !b.bytes || b.bytes > s->info.max_buffer)
		return fail("alloc NAME BYTES: invalid name or size");
	if (named(s, argv[1])) return fail("buffer name already exists");
	for (i = 0; i < RM443_PROBE_MAX_BUFFERS && s->buffers[i].name[0]; i++);
	if (i == RM443_PROBE_MAX_BUFFERS) return fail("buffer table full");
	if (request(s, RM443_PROBE_ALLOC, &b)) return -1;
	s->buffers[i].buffer = b;
	s->buffers[i].object = (struct rm443_probe_object){ .id = b.id, .bytes = b.bytes,
		.kind = RM443_PROBE_OBJECT_CONTIG, .physical = b.physical,
		.resident_bytes = (b.bytes + 4095) & ~4095U, .page_count = (b.bytes + 4095) / 4096 };
	strcpy(s->buffers[i].name, argv[1]);
	if (refresh_object(s, &s->buffers[i])) return -1;
	print_buffer(&s->buffers[i]);
	return 0;
}

static int transfer_file(struct session *s, int argc, char **argv, int upload)
{
	struct named_buffer *b;
	struct stat st;
	unsigned char *data;
	uint32_t offset = 0, bytes, done = 0;
	FILE *file;
	int fd, result = -1;
	if (argc < 3 || argc > (upload ? 4 : 5) || !(b = named(s, argv[1])) ||
	    (argc >= 4 && number(argv[3], &offset)) || offset >= b->buffer.bytes)
		return fail("%s NAME FILE [OFFSET%s]", argv[0], upload ? "" : " [BYTES]");
	bytes = b->buffer.bytes - offset;
	if (!upload && argc == 5 && (number(argv[4], &bytes) || bytes > b->buffer.bytes - offset))
		return fail("download range exceeds buffer");
	fd = open(argv[2], upload ? O_RDONLY | O_CLOEXEC : O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (fd < 0) return fail("%s: %s", argv[2], strerror(errno));
	if (upload && (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size > bytes)) {
		close(fd);
		return fail("upload requires a regular file fitting the buffer range");
	}
	if (upload) bytes = st.st_size;
	file = fdopen(fd, upload ? "rb" : "wb");
	if (!file) { close(fd); return fail("fdopen: %s", strerror(errno)); }
	data = malloc(RM443_PROBE_MAX_TRANSFER);
	if (!data) { fclose(file); return fail("out of memory"); }
	while (done < bytes) {
		struct rm443_probe_transfer x = { .id = b->buffer.id, .offset = offset + done,
			.bytes = bytes - done, .data = (uintptr_t)data };
		if (x.bytes > RM443_PROBE_MAX_TRANSFER) x.bytes = RM443_PROBE_MAX_TRANSFER;
		if (upload && fread(data, 1, x.bytes, file) != x.bytes) { fail("short upload read"); goto out; }
		if (request(s, upload ? RM443_PROBE_UPLOAD : RM443_PROBE_DOWNLOAD, &x)) goto out;
		if (!upload && fwrite(data, 1, x.bytes, file) != x.bytes) { fail("download write failed"); goto out; }
		done += x.bytes;
	}
	result = 0;
out:
	free(data);
	if (fclose(file)) result = fail("file close: %s", strerror(errno));
	fprintf(status_stream(), "%s %" PRIu32 "/%" PRIu32 " bytes\n", upload ? "uploaded" : "downloaded", done, bytes);
	return result;
}

static int refresh_object(struct session *s, struct named_buffer *b)
{
	struct rm443_probe_object object = { .id = b->buffer.id };
	if (request(s, RM443_PROBE_OBJECT_INFO, &object)) return -1;
	b->object = object;
	return 0;
}

static int create_object(struct session *s, int argc, char **argv, int flash)
{
	struct rm443_probe_object object = { .kind = flash ? RM443_PROBE_OBJECT_FLASH : RM443_PROBE_OBJECT_PAGED };
	unsigned int i;
	if (argc != (flash ? 4 : 3) || !name_valid(argv[1]) ||
	    number(argv[flash ? 3 : 2], &object.bytes) || !object.bytes ||
	    object.bytes > RM443_PROBE_MAX_OBJECT ||
	    (flash && (number(argv[2], &object.backing_physical) ||
	               (object.backing_physical & 4095U) ||
	               (uint64_t)object.backing_physical + object.bytes > 0x04000000U)))
		return fail("%s NAME %sBYTES: invalid name, range or page alignment", argv[0], flash ? "PHYSICAL " : "");
	if (named(s, argv[1])) return fail("buffer name already exists");
	for (i = 0; i < RM443_PROBE_MAX_BUFFERS && s->buffers[i].name[0]; i++);
	if (i == RM443_PROBE_MAX_BUFFERS) return fail("buffer table full");
	if (request(s, RM443_PROBE_OBJECT_CREATE, &object)) return -1;
	s->buffers[i].object = object;
	s->buffers[i].buffer = (struct rm443_probe_buffer){ .id = object.id, .bytes = object.bytes,
		.physical = object.physical };
	strcpy(s->buffers[i].name, argv[1]);
	print_buffer(&s->buffers[i]);
	return 0;
}

static int map_object(struct session *s, int argc, char **argv, int map)
{
	struct named_buffer *b;
	struct rm443_probe_object object = {0};
	if (argc != 2 || !(b = named(s, argv[1]))) return fail("%s requires an existing buffer name", argv[0]);
	object.id = b->buffer.id;
	if (request(s, map ? RM443_PROBE_MAP : RM443_PROBE_UNMAP, &object)) return -1;
	b->object = object;
	print_buffer(b);
	return 0;
}

static int page_list(struct session *s, int argc, char **argv)
{
	struct named_buffer *b;
	struct rm443_probe_page pages[256];
	uint32_t start = 0;
	if (argc != 2 || !(b = named(s, argv[1]))) return fail("pages requires an existing buffer name");
	if (refresh_object(s, b)) return -1;
	while (start < b->object.page_count) {
		struct rm443_probe_pages list = { .id = b->buffer.id, .start = start,
			.count = b->object.page_count - start, .data = (uintptr_t)pages };
		if (list.count > 256) list.count = 256;
		uint32_t capacity = list.count;
		if (request(s, RM443_PROBE_PAGES, &list)) return -1;
		if (!list.count || list.count > capacity) return fail("invalid page-list count");
		for (uint32_t i = 0; i < list.count; i++)
			fprintf(status_stream(), "%s page=%u offset=0x%08x physical=0x%08x backing=%s\n",
				b->name, start + i, pages[i].offset, pages[i].physical,
				pages[i].kind == RM443_PROBE_PAGE_RAM ? "ram" : "flash");
		start += list.count;
	}
	return 0;
}

static int budget(struct session *s, int argc, char **argv)
{
	struct rm443_probe_budget b = {0};
	if (argc != 1 && argc != 2 && argc != 3) return fail("budget [BYTES [HEADROOM_BYTES]]");
	if (argc > 1) {
		b.flags = RM443_PROBE_BUDGET_SET;
		b.headroom_bytes = RM443_PROBE_MIN_HEADROOM;
		if (number(argv[1], &b.limit_bytes) || !b.limit_bytes ||
		    (argc == 3 && number(argv[2], &b.headroom_bytes)) || b.headroom_bytes < RM443_PROBE_MIN_HEADROOM)
			return fail("invalid budget or insufficient headroom");
	}
	if (request(s, RM443_PROBE_BUDGET, &b)) return -1;
	fprintf(status_stream(), "budget=%u used=%u headroom=%u available=%u max_object=%u max_contiguous=%u\n",
		b.limit_bytes, b.used_bytes, b.headroom_bytes, b.available_bytes,
		b.max_object_bytes, b.max_contiguous_bytes);
	return 0;
}

static int hex_nibble(unsigned char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static int put_hex(struct session *s, int argc, char **argv)
{
	struct named_buffer *b;
	struct rm443_probe_transfer x = {0};
	unsigned char data[MAX_LINE / 2];
	size_t chars;
	if (argc != 4 || !(b = named(s, argv[1])) || number(argv[2], &x.offset))
		return fail("puthex NAME OFFSET HEX");
	chars = strlen(argv[3]);
	if (!chars || (chars & 1) || chars / 2 > sizeof(data) ||
	    x.offset > b->buffer.bytes || chars / 2 > b->buffer.bytes - x.offset)
		return fail("invalid hex length or buffer range");
	for (size_t i = 0; i < chars; i += 2) {
		int high = hex_nibble(argv[3][i]), low = hex_nibble(argv[3][i + 1]);
		if (high < 0 || low < 0) return fail("invalid hex digit");
		data[i / 2] = high * 16 + low;
	}
	x.id = b->buffer.id; x.bytes = chars / 2; x.data = (uintptr_t)data;
	if (request(s, RM443_PROBE_UPLOAD, &x)) return -1;
	fprintf(status_stream(), "uploaded %s offset=%u bytes=%u\n", b->name, x.offset, x.bytes);
	return 0;
}

static int exact_file(FILE *file, unsigned char *data, size_t bytes, int writing)
{
	size_t done = 0;
	while (done < bytes) {
		size_t n = writing ? fwrite(data + done, 1, bytes - done, file) :
			fread(data + done, 1, bytes - done, file);
		done += n;
		if (done == bytes) break;
		if (ferror(file)) {
			if (errno == EINTR) { clearerr(file); continue; }
			return fail("stream %s: %s", writing ? "write" : "read", strerror(errno));
		}
		if (!n || feof(file)) return fail("short stream %s", writing ? "write" : "read");
	}
	return 0;
}

/* Exact-length streams can be pipes; no seek, stat-sized image or second
 * image-sized allocation is needed. '-' input consumes this session's input
 * after the command newline, so command/data framing is unambiguous. */
static int stream_object(struct session *s, int argc, char **argv, int upload)
{
	struct named_buffer *b;
	struct stat st;
	uint32_t offset, bytes, done = 0;
	unsigned char *data;
	FILE *file;
	int fd, close_file = 1, result = -1;
	if (argc != 5 || !(b = named(s, argv[1])) || number(argv[3], &offset) ||
	    number(argv[4], &bytes) || offset > b->buffer.bytes || bytes > b->buffer.bytes - offset)
		return fail("%s NAME PATH OFFSET BYTES", argv[0]);
	if (!strcmp(argv[2], "-")) {
		if (upload) {
			file = s->input ? s->input : stdin;
			if (isatty(fileno(file))) return fail("binary import requires a non-terminal input stream; use puthex on serial");
		} else {
			if (!s->binary_stdout) return fail("stdout export requires --binary so text goes to stderr");
			file = stdout;
		}
		close_file = 0;
	} else {
		if (upload) fd = open(argv[2], O_RDONLY | O_CLOEXEC);
		else {
			/* Existing FIFO streams are allowed; existing regular files are
			 * never truncated. O_NOFOLLOW and fstat cover a replaced path. */
			fd = open(argv[2], O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
			if (fd < 0 && errno == EEXIST) {
				if (lstat(argv[2], &st) || !S_ISFIFO(st.st_mode)) return fail("export target exists and is not a FIFO");
				fd = open(argv[2], O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
				if (fd >= 0 && (fstat(fd, &st) || !S_ISFIFO(st.st_mode))) { close(fd); return fail("export target changed"); }
			}
		}
		if (fd < 0) return fail("%s: %s", argv[2], strerror(errno));
		file = fdopen(fd, upload ? "rb" : "wb");
		if (!file) { close(fd); return fail("fdopen: %s", strerror(errno)); }
	}
	data = malloc(RM443_PROBE_MAX_TRANSFER);
	if (!data) { if (close_file) fclose(file); return fail("out of memory"); }
	while (done < bytes) {
		struct rm443_probe_transfer x = { .id = b->buffer.id, .offset = offset + done,
			.bytes = bytes - done, .data = (uintptr_t)data };
		if (x.bytes > RM443_PROBE_MAX_TRANSFER) x.bytes = RM443_PROBE_MAX_TRANSFER;
		if (upload && exact_file(file, data, x.bytes, 0)) goto out;
		if (request(s, upload ? RM443_PROBE_UPLOAD : RM443_PROBE_DOWNLOAD, &x)) goto out;
		if (!upload && exact_file(file, data, x.bytes, 1)) goto out;
		done += x.bytes;
	}
	result = 0;
out:
	free(data);
	if (!upload && fflush(file)) result = fail("stream flush: %s", strerror(errno));
	if (close_file && fclose(file)) result = fail("stream close: %s", strerror(errno));
	fprintf(status_stream(), "%s %u/%u bytes at %u\n", upload ? "imported" : "exported", done, bytes, offset);
	return result;
}

static int copy_physical(struct session *s, int argc, char **argv)
{
	struct named_buffer *b;
	uint32_t offset, physical, bytes, done = 0;
	if (argc != 5 || !(b = named(s, argv[1])) || number(argv[2], &offset) ||
	    number(argv[3], &physical) || number(argv[4], &bytes) ||
	    offset > b->buffer.bytes || bytes > b->buffer.bytes - offset ||
	    (uint64_t)physical + bytes > 0x04000000U)
		return fail("copy NAME OFFSET NOR_PHYSICAL BYTES: invalid range");
	while (done < bytes) {
		struct rm443_probe_copy c = { .id = b->buffer.id, .offset = offset + done,
			.physical = physical + done, .bytes = bytes - done };
		if (c.bytes > RM443_PROBE_MAX_TRANSFER) c.bytes = RM443_PROBE_MAX_TRANSFER;
		if (request(s, RM443_PROBE_COPY_PHYS, &c)) return -1;
		if (c.done > c.bytes) return fail("driver returned invalid copy length");
		done += c.done;
		if (c.status || c.done != c.bytes) return fail("copy stopped after %u/%u bytes: status=%d", done, bytes, c.status);
	}
	fprintf(status_stream(), "copied %u bytes at %u\n", done, offset);
	return 0;
}

static int get_hex(struct session *s, int argc, char **argv)
{
	struct named_buffer *b;
	uint32_t offset, bytes, done = 0;
	unsigned char data[128];
	if (argc != 4 || !(b = named(s, argv[1])) || number(argv[2], &offset) ||
	    number(argv[3], &bytes) || offset > b->buffer.bytes || bytes > b->buffer.bytes - offset)
		return fail("gethex NAME OFFSET BYTES");
	while (done < bytes) {
		struct rm443_probe_transfer x = { .id = b->buffer.id, .offset = offset + done,
			.bytes = bytes - done, .data = (uintptr_t)data };
		if (x.bytes > sizeof(data)) x.bytes = sizeof(data);
		if (request(s, RM443_PROBE_DOWNLOAD, &x)) return -1;
		fprintf(status_stream(), "data %s %u ", b->name, x.offset);
		for (uint32_t i = 0; i < x.bytes; i++) fprintf(status_stream(), "%02x", data[i]);
		fputc('\n', status_stream());
		done += x.bytes;
	}
	return 0;
}

static int values(struct session *s, const char *text, __u32 *out, unsigned int capacity, int allow_virtual)
{
	char copy[MAX_LINE], *cursor, *comma;
	unsigned int count = 0;
	if (!*text || strlen(text) >= sizeof(copy)) return -1;
	strcpy(copy, text);
	cursor = copy;
	do {
		comma = strchr(cursor, ',');
		if (comma) *comma = 0;
		if (count == capacity || address_mode(s, cursor, &out[count], allow_virtual)) return -1;
		count++;
		cursor = comma ? comma + 1 : NULL;
	} while (cursor);
	return count;
}

static int descriptor(struct session *s, const char *text, __u32 out[4])
{
	FILE *f;
	unsigned char bytes[17];
	size_t n;
	unsigned int i;
	if (strncmp(text, "file:", 5)) return values(s, text, out, 4, 0) == 4 ? 0 : -1;
	f = fopen(text + 5, "rb");
	if (!f) return -1;
	n = fread(bytes, 1, sizeof(bytes), f);
	if (ferror(f)) n = 0;
	if (fclose(f)) n = 0;
	if (n != 16) return -1;
	for (i = 0; i < 4; i++)
		out[i] = (uint32_t)bytes[4*i] | (uint32_t)bytes[4*i+1] << 8 |
			 (uint32_t)bytes[4*i+2] << 16 | (uint32_t)bytes[4*i+3] << 24;
	return 0;
}

static void print_record(const struct rm443_probe_call *c)
{
	unsigned int i;
	fprintf(status_stream(), "sequence=%" PRIu32 " entry=0x%08" PRIx32 " stage=%" PRIu32 " elapsed_ns=%" PRIu64 " cpsr=0x%08" PRIx32 "\n",
	       (uint32_t)c->sequence, (uint32_t)c->entry, (uint32_t)c->stage, (uint64_t)c->elapsed_ns, (uint32_t)c->cpsr);
	for (i = 0; i < 13; i++) fprintf(status_stream(), "r%u=0x%08" PRIx32 "%c", i, (uint32_t)c->returned[i], i == 12 ? '\n' : ' ');
	fprintf(status_stream(), "callbacks=%" PRIu32 " overflow=%" PRIu32 "\n", (uint32_t)c->callback_count, (uint32_t)c->callback_overflow);
	for (i = 0; i < c->callback_count && i < RM443_PROBE_CALLBACKS; i++) {
		const struct rm443_probe_callback *cb = &c->callbacks[i];
		fprintf(status_stream(), "callback[%u] stage=%" PRIu32 " args=%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32 " result=%08" PRIx32 "\n",
		       i, (uint32_t)cb->stage, (uint32_t)cb->args[0], (uint32_t)cb->args[1],
		       (uint32_t)cb->args[2], (uint32_t)cb->args[3], (uint32_t)cb->result);
	}
}

static int execute(struct session *s, int argc, char **argv, int rom)
{
	struct rm443_probe_call call = {0};
	uint32_t seen = 0;
	int i, have_descriptor = 0, have_entry = 0, have_stack = 0;
	call.flags = rom ? RM443_PROBE_CALL_ROM : 0;
	call.entry = RM443_PROBE_ROM_ENTRY;
	if (!rom && (argc < 2 || address(s, argv[1], &call.entry))) return fail("exec requires an entry address");
	if (!rom && (argv[1][0] == '%' || !strcmp(argv[0], "execva")))
		call.flags |= RM443_PROBE_CALL_VIRTUAL;
	for (i = rom ? 1 : 2; i < argc; i++) {
		char *eq = strchr(argv[i], '=');
		uint32_t reg;
		char key[32];
		size_t len = eq ? (size_t)(eq - argv[i]) : 0;
		if (!eq || !len || len >= sizeof(key)) return fail("expected KEY=VALUE: %s", argv[i]);
		memcpy(key, argv[i], len); key[len] = 0;
		if (!strcmp(key, "descriptor")) {
			if (!rom || have_descriptor++ || descriptor(s, eq + 1, call.descriptor)) return fail("descriptor requires exactly four words or a 16-byte file");
		} else if (!strcmp(key, "entry")) {
			if (!rom || have_entry++ || address_mode(s, eq + 1, &call.entry, 0)) return fail("invalid or repeated entry");
		} else if (!strcmp(key, "stack")) {
			int count = values(s, eq + 1, call.stack, RM443_PROBE_STACK_WORDS, !rom);
			if (have_stack++ || count < 0) return fail("invalid or repeated stack arguments");
			call.stack_words = count;
		} else if (key[0] == 'r' && !number(key + 1, &reg) && reg < 13) {
			if ((rom && reg == 0) || (seen & (1U << reg)) || address_mode(s, eq + 1, &call.regs[reg], !rom)) return fail("invalid, duplicate or reserved register: %s", key);
			seen |= 1U << reg;
		} else return fail("unknown argument: %s", key);
	}
	if (rom && !have_descriptor) return fail("call requires an explicit descriptor");
	if (!call.entry || (!(call.entry & 1) && (call.entry & 3))) return fail("invalid entry alignment");
	if (request(s, RM443_PROBE_CALL, &call)) {
		fputs("Call was not retried. Use record to inspect the last recorded execution.\n", stderr);
		return -1;
	}
	print_record(&call);
	return 0;
}

static int read_memory(struct session *s, int argc, char **argv)
{
	struct rm443_probe_read r = {0};
	unsigned char *data;
	FILE *output = NULL;
	int fd, result;
	unsigned int i;
	if ((argc != 5 && argc != 6) || address_mode(s, argv[2], &r.address, !strcmp(argv[1], "bridge")) ||
	    number(argv[3], &r.width) || number(argv[4], &r.bytes) ||
	    (r.width != 1 && r.width != 2 && r.width != 4) || !r.bytes ||
	    r.bytes > RM443_PROBE_MAX_TRANSFER || r.address % r.width || r.bytes % r.width ||
	    (uint64_t)r.address + r.bytes > UINT64_C(0x100000000))
		return fail("read physical|bridge ADDRESS WIDTH BYTES [NEWFILE]: invalid range or width");
	if (!strcmp(argv[1], "physical")) r.space = RM443_PROBE_READ_PHYSICAL;
	else if (!strcmp(argv[1], "bridge")) r.space = RM443_PROBE_READ_BRIDGE;
	else return fail("read space must be physical or bridge");
	data = calloc(1, r.bytes);
	if (!data) return fail("out of memory");
	if (argc == 6) {
		fd = open(argv[5], O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		if (fd < 0) { free(data); return fail("%s: %s", argv[5], strerror(errno)); }
		output = fdopen(fd, "wb");
		if (!output) { close(fd); free(data); return fail("fdopen: %s", strerror(errno)); }
	}
	r.data = (uintptr_t)data;
	result = request(s, RM443_PROBE_READ, &r);
	if (!result && r.done > r.bytes) result = fail("driver returned invalid read length");
	if (!result) {
		if (output) {
			if (fwrite(data, 1, r.done, output) != r.done) result = fail("read output write failed");
		} else {
			for (i = 0; i < r.done; i++) {
				if (!(i % 16)) fprintf(status_stream(), "%08" PRIx32 ":", (uint32_t)(r.address + i));
				fprintf(status_stream(), " %02x", data[i]);
				if (i % 16 == 15 || i + 1 == r.done) fputc('\n', status_stream());
			}
		}
		fprintf(status_stream(), "read=%" PRIu32 "/%" PRIu32 " status=%" PRId32 " fault_address=0x%08" PRIx32 "\n",
		       (uint32_t)r.done, (uint32_t)r.bytes, (int32_t)r.status, (uint32_t)r.fault_address);
		if (r.status) result = -1;
	}
	if (output && fclose(output)) result = fail("read output close: %s", strerror(errno));
	free(data);
	return result;
}

/* 1 means orderly quit, 0 success, -1 error. */
static int command(struct session *s, int argc, char **argv)
{
	unsigned int i;
	if (!argc) return 0;
	if (!strcmp(argv[0], "alloc")) return alloc_buffer(s, argc, argv);
	if (!strcmp(argv[0], "paged")) return create_object(s, argc, argv, 0);
	if (!strcmp(argv[0], "flash")) return create_object(s, argc, argv, 1);
	if (!strcmp(argv[0], "map")) return map_object(s, argc, argv, 1);
	if (!strcmp(argv[0], "unmap")) return map_object(s, argc, argv, 0);
	if (!strcmp(argv[0], "pages")) return page_list(s, argc, argv);
	if (!strcmp(argv[0], "budget")) return budget(s, argc, argv);
	if (!strcmp(argv[0], "puthex")) return put_hex(s, argc, argv);
	if (!strcmp(argv[0], "gethex")) return get_hex(s, argc, argv);
	if (!strcmp(argv[0], "copy")) return copy_physical(s, argc, argv);
	if (!strcmp(argv[0], "import")) return stream_object(s, argc, argv, 1);
	if (!strcmp(argv[0], "export")) return stream_object(s, argc, argv, 0);
	if (!strcmp(argv[0], "upload")) return transfer_file(s, argc, argv, 1);
	if (!strcmp(argv[0], "download")) return transfer_file(s, argc, argv, 0);
	if (!strcmp(argv[0], "call")) return execute(s, argc, argv, 1);
	if (!strcmp(argv[0], "exec") || !strcmp(argv[0], "execva")) return execute(s, argc, argv, 0);
	if (!strcmp(argv[0], "read")) return read_memory(s, argc, argv);
	if (!strcmp(argv[0], "free")) {
		struct named_buffer *b;
		if (argc != 2 || !(b = named(s, argv[1]))) return fail("free requires an existing buffer name");
		if (request(s, RM443_PROBE_FREE, &b->buffer)) return -1;
		memset(b, 0, sizeof(*b));
		return 0;
	}
	if (argc != 1) return fail("unexpected command arguments");
	if (!strcmp(argv[0], "help")) { help(); return 0; }
	if (!strcmp(argv[0], "quit")) return 1;
	if (!strcmp(argv[0], "buffers")) {
		for (i = 0; i < RM443_PROBE_MAX_BUFFERS; i++)
			if (s->buffers[i].name[0]) {
				if (refresh_object(s, &s->buffers[i])) return -1;
				print_buffer(&s->buffers[i]);
			}
		return 0;
	}
	if (!strcmp(argv[0], "record")) {
		struct rm443_probe_call c = {0};
		if (request(s, RM443_PROBE_RECORD, &c)) return -1;
		print_record(&c);
		return 0;
	}
	if (!strcmp(argv[0], "info")) {
		struct rm443_probe_info *n = &s->info;
		if (request(s, RM443_PROBE_INFO, n)) return -1;
		fprintf(status_stream(), "abi=%u max_buffers=%u max_buffer=%u max_total=%u max_transfer=%u stack_words=%u callbacks=%u\n",
		       n->abi, n->max_buffers, n->max_buffer, n->max_total, n->max_transfer, n->stack_words, n->callbacks);
		fprintf(status_stream(), "rom_entry=%08x descriptor=%08x pa_header=%08x callback=%08x work=%08x+%x bridge=%08x+%x\n",
		       n->rom_entry, n->descriptor_pa, n->pa_header_pa, n->callback_va, n->work_pa, n->work_bytes, n->bridge_pa, n->bridge_bytes);
		return 0;
	}
	return fail("unknown command: %s", argv[0]);
}

int main(int argc, char **argv)
{
	struct session s = {0};
	FILE *input = stdin;
	const char *fifo = NULL;
	struct stat fifo_stat = {0};
	char line[MAX_LINE], *words[MAX_WORDS];
	int interactive, result = 0, rc;
	unsigned long line_number = 0;
	/* A disconnected export reader should reach EPIPE handling and release
	 * the session, including its owned command FIFO, through normal cleanup. */
	signal(SIGPIPE, SIG_IGN);
	status_output = stdout;
	if (argc > 1 && !strcmp(argv[1], "--binary")) {
		s.binary_stdout = 1;
		status_output = stderr;
		argv++; argc--;
	}
	if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "help"))) { help(); return 0; }
	if (argc > 1 && argv[1][0] == '-' && (argc != 3 || (strcmp(argv[1], "-f") && strcmp(argv[1], "-c") && strcmp(argv[1], "--fifo")))) {
		help(); return 2;
	}
	if (argc == 3 && !strcmp(argv[1], "-f")) {
		input = fopen(argv[2], "r");
		if (!input) { fail("%s: %s", argv[2], strerror(errno)); return 1; }
	}
	if (argc == 3 && !strcmp(argv[1], "--fifo")) {
		int fd;
		if (mkfifo(argv[2], 0600)) { fail("create FIFO %s: %s", argv[2], strerror(errno)); return 1; }
		if (lstat(argv[2], &fifo_stat) || !S_ISFIFO(fifo_stat.st_mode)) return fail("FIFO path changed") != 0;
		fifo = argv[2];
		/* Keep a writer open ourselves so separate shell writes do not end
		 * the session between commands. quit is the explicit close operation. */
		fd = open(fifo, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
		struct stat opened;
		if (fd < 0 || fstat(fd, &opened) || !S_ISFIFO(opened.st_mode) ||
		    opened.st_dev != fifo_stat.st_dev || opened.st_ino != fifo_stat.st_ino) {
			if (fd >= 0) close(fd);
			fail("cannot open owned FIFO"); result = 1; goto out;
		}
		input = fdopen(fd, "r");
		if (!input) { close(fd); input = stdin; fail("fdopen FIFO: %s", strerror(errno)); result = 1; goto out; }
	}
	s.input = input;
	s.fd = open("/dev/rm443_probe", O_RDWR | O_CLOEXEC);
	if (s.fd < 0) { fail("/dev/rm443_probe: %s", strerror(errno)); result = 1; goto out; }
	if (request(&s, RM443_PROBE_INFO, &s.info)) { result = 1; goto close_device; }
	if (s.info.abi != RM443_PROBE_ABI) { fail("unsupported ABI %u", s.info.abi); result = 1; goto close_device; }
	if (argc > 1 && strcmp(argv[1], "-f") && strcmp(argv[1], "--fifo")) {
		if (!strcmp(argv[1], "-c")) {
			if (strlen(argv[2]) >= sizeof(line)) { fail("command too long"); result = 1; goto close_device; }
			strcpy(line, argv[2]);
			rc = split(line, words);
			result = rc < 0 || command(&s, rc, words) < 0;
		} else result = command(&s, argc - 1, argv + 1) < 0;
		goto close_device;
	}
	if (fifo) { fprintf(status_stream(), "ready FIFO %s\n", fifo); fflush(status_stream()); }
	interactive = input == stdin && isatty(STDIN_FILENO);
	for (;;) {
		if (interactive) { fputs("probe> ", status_stream()); fflush(status_stream()); }
		if (!fgets(line, sizeof(line), input)) break;
		line_number++;
		if (!strchr(line, '\n') && !feof(input)) {
			int c;
			while ((c = fgetc(input)) != '\n' && c != EOF);
			rc = fail("line too long");
		} else {
			rc = split(line, words);
			if (rc >= 0) rc = command(&s, rc, words);
		}
		fflush(status_stream());
		if (rc > 0) break;
		if (rc < 0) {
			result = 1;
			if (!interactive) { fail("stopped at script line %lu", line_number); break; }
		}
	}
	if (ferror(input)) { fail("input read failed"); result = 1; }
close_device:
	if (close(s.fd)) { fail("device close: %s", strerror(errno)); result = 1; }
out:
	if (input != stdin) fclose(input);
	if (fifo) {
		struct stat now;
		if (!lstat(fifo, &now) && S_ISFIFO(now.st_mode) &&
		    now.st_dev == fifo_stat.st_dev && now.st_ino == fifo_stat.st_ino)
			unlink(fifo);
	}
	return result;
}
