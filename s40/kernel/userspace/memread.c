/* SPDX-License-Identifier: MIT */
/* Stream a physical-memory range through the kernel's read-only /dev/mem API. */
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

_Static_assert(sizeof(off_t) >= 8, "physical reads require 64-bit file offsets");

static int number(const char *text, uint64_t *value)
{
	char *end;
	if (!*text || *text == '-') return -1;
	errno = 0;
	*value = strtoull(text, &end,
		text[0] == '0' && (text[1] == 'x' || text[1] == 'X') ? 16 : 10);
	return errno || end == text || *end || *value > UINT32_MAX ? -1 : 0;
}

int main(int argc, char **argv)
{
	uint64_t address, length, offset = 0;
	unsigned char buffer[4096];
	long page_size = sysconf(_SC_PAGESIZE);
	int source, output = STDOUT_FILENO, result = 1;
	if ((argc != 3 && argc != 4) || number(argv[1], &address) ||
	    number(argv[2], &length) || address + length > (UINT64_C(1) << 32)) {
		fprintf(stderr, "Use: memread ADDRESS LENGTH [NEW_OUTPUT_FILE]\n"
			"ADDRESS and LENGTH are decimal or 0x-prefixed hexadecimal; range must fit 32-bit physical memory.\n");
		return 2;
	}
	if (page_size <= 0) { fprintf(stderr, "memread: cannot determine page size\n"); return 1; }
	source = open("/dev/mem", O_RDONLY | O_CLOEXEC);
	if (source < 0) { perror("/dev/mem"); return 1; }
	if (argc == 4) {
		output = open(argv[3], O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		if (output < 0) { perror(argv[3]); close(source); return 1; }
	}
	while (offset < length) {
		uint64_t current = address + offset;
		size_t in_page = current % page_size;
		size_t count = length - offset > sizeof(buffer) ? sizeof(buffer) : length - offset;
		if (count > (size_t)page_size - in_page) count = page_size - in_page;
		/* ARM's /dev/mem read() accepts DRAM only; read-only mmap also exposes
		 * ROM. Keep each mapping to one page and never request PROT_WRITE. */
		void *mapping = mmap(NULL, page_size, PROT_READ, MAP_SHARED, source,
				     (off_t)(current - in_page));
		if (mapping == MAP_FAILED) {
			fprintf(stderr, "memread: map at 0x%08" PRIx64 ": %s; output contains %" PRIu64 " bytes\n",
				current, strerror(errno), offset);
			goto done;
		}
		memcpy(buffer, (const unsigned char *)mapping + in_page, count);
		if (munmap(mapping, page_size)) { perror("memread munmap"); goto done; }
		for (size_t sent = 0; sent < count;) {
			ssize_t n = write(output, buffer + sent, count - sent);
			if (n < 0 && errno == EINTR) continue;
			if (n <= 0) { perror("memread output"); goto done; }
			sent += n;
		}
		offset += count;
	}
	if (argc == 4 && fsync(output)) { perror("memread fsync"); goto done; }
	result = 0;
done:
	if (argc == 4 && close(output)) { perror("memread close"); result = 1; }
	close(source);
	return result;
}
