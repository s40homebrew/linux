/* SPDX-License-Identifier: MIT */
/* Exercise actual anonymous pages from an interactive shell. No /dev/mem,
 * firmware access, or assumptions about the kernel's physical page placement.
 */
int main(int argc, char **argv)
{
	unsigned long mib = 20, words, i, pass;
	volatile unsigned int *area;
	char *end;
	if (argc > 2) {
		printf("usage: memcheck [MiB, 1..24]\n");
		return 2;
	}
	if (argc == 2) {
		mib = strtoul(argv[1], &end, 10);
		if (!*argv[1] || *end || mib < 1 || mib > 24) {
			printf("memcheck: choose 1..24 MiB\n");
			return 2;
		}
	}
	words = mib * 1024 * 1024 / sizeof(*area);
	area = mmap(NULL, words * sizeof(*area), PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (area == MAP_FAILED) {
		printf("memcheck: mmap failed errno=%d\n", errno);
		return 1;
	}
	for (pass = 0; pass < 2; ++pass) {
		unsigned int seed = pass ? 0x5a5a5a5a : 0xa5a5a5a5;
		for (i = 0; i < words; ++i)
			area[i] = (unsigned int)(i * 2654435761u) ^ seed;
		for (i = words; i-- > 0;) {
			unsigned int expected = (unsigned int)(i * 2654435761u) ^ seed;
			if (area[i] != expected) {
				printf("memcheck: FAIL word=%lu pass=%lu got=%x expected=%x\n",
				       i, pass, area[i], expected);
				munmap((void *)area, words * sizeof(*area));
				return 1;
			}
		}
	}
	munmap((void *)area, words * sizeof(*area));
	printf("MEMCHECK PASS: %lu MiB, every word written/read, 2 patterns\n", mib);
	return 0;
}
