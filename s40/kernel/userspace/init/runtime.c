/* SPDX-License-Identifier: MIT */
#include "runtime.h"

static int logfd = -1;
static char line[512];
static unsigned used;

void log_append(const char *s)
{
	while (*s && used + 2 < sizeof(line))
		line[used++] = *s++;
}

void log_decimal(unsigned long value)
{
	char digits[24];
	unsigned n = 0;
	do {
		digits[n++] = '0' + value % 10;
		value /= 10;
	} while (value);
	while (n && used + 2 < sizeof(line))
		line[used++] = digits[--n];
}

void log_begin(const char *text)
{
	used = 0;
	log_append("<6>init: ");
	log_append(text);
}

void log_emit(void)
{
	line[used++] = '\n';
	if (logfd >= 0)
		write(logfd, line, used);
	else
		write(1, line + 3, used - 3);
}

void log_message(const char *text)
{
	log_begin(text);
	log_emit();
}

unsigned long uptime_seconds(void)
{
	struct timespec now;
	if (my_syscall2(__NR_clock_gettime, 1 /* CLOCK_MONOTONIC */, &now) < 0)
		return 0;
	return now.tv_sec;
}

void log_file(const char *path, unsigned maxlines)
{
	char buf[1024];
	int fd = open(path, O_RDONLY);
	ssize_t count;
	unsigned i, rows = 0;
	if (fd < 0) {
		log_begin("cannot read "); log_append(path); log_emit();
		return;
	}
	log_begin(path); log_emit();
	log_begin("");
	while ((count = read(fd, buf, sizeof(buf))) > 0) {
		for (i = 0; i < (unsigned)count; i++) {
			if (buf[i] == '\n') {
				log_emit();
				if (++rows >= maxlines)
					goto done;
				log_begin("");
			} else if (used + 2 < sizeof(line)) {
				line[used++] = buf[i];
			}
		}
	}
done:
	close(fd);
}

void log_open(void)
{
	logfd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
}

int log_descriptor(void)
{
	return logfd;
}
