/* SPDX-License-Identifier: MIT */
/* Handset-only read check; build with the kernel's ARM nolibc headers. */
static unsigned char reference[4096] __attribute__((aligned(4096)));
static unsigned char sample[4096] __attribute__((aligned(4096)));

int main(void)
{
	int fd = open("/dev/mmcblk0", O_RDONLY | O_DIRECT);
	unsigned int i;
	if (fd < 0) {
		perror("open SD read-only/direct");
		return 1;
	}
	if (read(fd, reference, sizeof(reference)) != sizeof(reference)) {
		perror("reference read");
		close(fd);
		return 1;
	}
	for (i = 0; i < 50; ++i) {
		if (lseek(fd, 0, SEEK_SET) != 0 ||
		    read(fd, sample, sizeof(sample)) != sizeof(sample)) {
			printf("SD_DIRECT_READ_FAIL iteration=%u errno=%d\n", i + 1, errno);
			close(fd);
			return 1;
		}
		if (memcmp(reference, sample, sizeof(sample))) {
			printf("SD_DIRECT_READ_MISMATCH iteration=%u\n", i + 1);
			close(fd);
			return 1;
		}
	}
	close(fd);
	printf("SD_DIRECT_READ_PASS repeats=%u bytes_each=%u\n", i, (unsigned)sizeof(sample));
	return 0;
}
