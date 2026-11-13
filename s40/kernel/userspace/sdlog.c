/* SPDX-License-Identifier: MIT */
/* Persistent printk capture. All potentially blocking SD I/O stays out of PID1. */
#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif

#define LOG_LIMIT (1024 * 1024)
#define TOTAL_LIMIT (16 * LOG_LIMIT)
#define DEFAULT_SOCKET "/tmp/sdlog.sock"
static volatile sig_atomic_t stopping;
static int output = -1, source = -1, capture_error;
static size_t written, limit = LOG_LIMIT;

static void stop_signal(int sig) { (void)sig; stopping = 1; }

static int write_all(int fd, const void *buffer, size_t size)
{
	const char *p = buffer;
	while (size) {
		ssize_t n = write(fd, p, size);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) return -1;
		p += n; size -= n;
	}
	return 0;
}

static int save(const char *data, size_t size)
{
	if (size > limit - written) { errno = EFBIG; return -1; }
	if (write_all(output, data, size)) return -1;
	written += size;
	return 0;
}

static int drain(int barrier)
{
	char buffer[8192];
	unsigned count;
	size_t before = written;
	int caught_up = 0;
	if (capture_error) { errno = capture_error; return -1; }
	/* Bound each batch even if the kernel produces logs continuously. */
	for (count = 0; count < (barrier ? 4096U : 256U); count++) {
		if (stopping) { errno = ECANCELED; goto fail; }
		ssize_t n = read(source, buffer, sizeof(buffer));
		if (n > 0) {
			if (save(buffer, n)) goto fail;
		} else if (n == 0 || errno == EAGAIN) {
			caught_up = 1;
			break;
		} else if (errno == EINTR) {
			continue;
		} else if (errno == EPIPE) {
			const char loss[] = "SDLOG: kernel ring overrun; records lost\n";
			if (save(loss, sizeof(loss) - 1)) goto fail;
			/* A lost record might be the requested checkpoint. */
			if (barrier) { fsync(output); errno = EPIPE; goto fail; }
		} else goto fail;
	}
	if (barrier && !caught_up) { errno = EOVERFLOW; goto fail; }
	if (written != before && fsync(output)) goto fail;
	return 0;
fail:
	capture_error = errno ? errno : EIO;
	/* Preserve bytes already received even when this checkpoint is refused. */
	if (written != before) fsync(output);
	errno = capture_error;
	return -1;
}

static int connect_control(const char *path)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	int fd;
	if (strlen(path) >= sizeof(addr.sun_path)) { errno = ENAMETOOLONG; return -1; }
	strcpy(addr.sun_path, path);
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) return -1;
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr))) {
		int error = errno; close(fd); errno = error; return -1;
	}
	return fd;
}

static int read_request(int fd, char *buffer, size_t capacity)
{
	struct timespec start, now;
	size_t used = 0;
	clock_gettime(CLOCK_MONOTONIC, &start);
	while (used + 1 < capacity && !stopping) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		clock_gettime(CLOCK_MONOTONIC, &now);
		long elapsed = (now.tv_sec - start.tv_sec) * 1000 +
			(now.tv_nsec - start.tv_nsec) / 1000000;
		if (elapsed >= 1000 || poll(&pfd, 1, 1000 - elapsed) <= 0) return -1;
		ssize_t n = read(fd, buffer + used, capacity - used - 1);
		if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
		if (n <= 0) return -1;
		used += n; buffer[used] = 0;
		if (strchr(buffer, '\n')) return 0;
	}
	return -1;
}

static int control(const char *path, const char *request, int stop)
{
	struct pollfd pollfd;
	char response[128];
	int fd = connect_control(path), ret = 1;
	if (fd < 0) {
		if (stop && (errno == ENOENT || errno == ECONNREFUSED)) return 0;
		perror("sdlog control connect"); return 1;
	}
	if (write_all(fd, request, strlen(request))) goto done;
	pollfd = (struct pollfd){ .fd = fd, .events = POLLIN };
	if (poll(&pollfd, 1, 60000) <= 0) {
		fprintf(stderr, "sdlog: control acknowledgement timed out\n"); goto done;
	}
	ssize_t n = read(fd, response, sizeof(response) - 1);
	if (n > 0) {
		response[n] = 0;
		if (!strncmp(response, "OK\n", 3)) ret = 0;
		else fprintf(stderr, "sdlog: %s", response);
	}
done:
	close(fd); return ret;
}

static int mount_card(void)
{
	if (stopping) { errno = ECANCELED; return -1; }
	FILE *mounts = fopen("/proc/mounts", "r");
	char device[256], target[256], type[32], options[512];
	if (mounts) {
		while (fscanf(mounts, "%255s %255s %31s %511s %*d %*d\n",
			      device, target, type, options) == 4) {
			if (!strcmp(target, "/mnt/card")) {
				fclose(mounts);
				if (!strcmp(type, "vfat") && !strncmp(options, "rw,", 3)) return 0;
				errno = EROFS; return -1;
			}
		}
		fclose(mounts);
	}
	pid_t child = fork();
	if (child < 0) return -1;
	if (!child) {
		setpgid(0, 0);
		/* Never let mount progress accidentally satisfy the READY pipe. */
		dup2(STDERR_FILENO, STDOUT_FILENO);
		execl("/bin/card", "card", "mount", "rw", (char *)NULL);
		_exit(127);
	}
	int status;
	for (;;) {
		if (stopping) { kill(-child, SIGTERM); kill(child, SIGTERM); }
		if (waitpid(child, &status, 0) >= 0) break;
		if (errno != EINTR) return -1;
	}
	if (stopping) { errno = ECANCELED; return -1; }
	if (!WIFEXITED(status) || WEXITSTATUS(status)) { errno = EIO; return -1; }
	return 0;
}

static int make_directory(const char *path)
{
	if (mkdir(path, 0755) && errno != EEXIST) return -1;
	int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) return -1;
	int ret = fsync(fd); close(fd); return ret;
}

static int create_log(const char *directory)
{
	struct statvfs space;
	struct dirent *entry;
	struct stat st;
	unsigned count = 0, index;
	unsigned long long total = 0;
	DIR *dir = opendir(directory);
	if (!dir) return -1;
	while ((entry = readdir(dir))) {
		if (strncmp(entry->d_name, "boot-", 5)) continue;
		if (fstatat(dirfd(dir), entry->d_name, &st, AT_SYMLINK_NOFOLLOW)) goto fail;
		if (!S_ISREG(st.st_mode)) continue;
		total += st.st_size; count++;
	}
	if (count >= 128 || total + limit > TOTAL_LIMIT) { errno = ENOSPC; goto fail; }
	if (fstatvfs(dirfd(dir), &space)) goto fail;
	if ((unsigned long long)space.f_bavail * space.f_frsize < limit + LOG_LIMIT) {
		errno = ENOSPC; goto fail;
	}
	for (index = 1; index <= 999999; index++) {
		char name[64];
		snprintf(name, sizeof(name), "boot-%06u.kmsg", index);
		output = openat(dirfd(dir), name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
		if (output >= 0) {
			const char header[] = "SDLOG v1: raw /dev/kmsg records; bounded 1 MiB per boot\n";
			if (save(header, sizeof(header) - 1) || fsync(output) || fsync(dirfd(dir))) goto fail;
			fprintf(stderr, "sdlog: capturing %s/%s\n", directory, name);
			closedir(dir); return 0;
		}
		if (errno != EEXIST) goto fail;
	}
	errno = ENOSPC;
fail:
	closedir(dir); return -1;
}

int main(int argc, char **argv)
{
	const char *directory = "/mnt/card/Linux/logs", *input = "/dev/kmsg";
	const char *socket_path = DEFAULT_SOCKET, *request = NULL;
	int no_mount = 0, is_stop = 0, server = -1, result = 1, bound = 0;
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--stop")) { request = "STOP\n"; is_stop = 1; }
		else if (!strcmp(argv[i], "--checkpoint")) request = "SYNC\n";
		else if (!strcmp(argv[i], "--directory") && i + 1 < argc) { directory = argv[++i]; no_mount = 1; }
		else if (!strcmp(argv[i], "--input") && i + 1 < argc) input = argv[++i];
		else if (!strcmp(argv[i], "--socket") && i + 1 < argc) socket_path = argv[++i];
		else { fprintf(stderr, "sdlog: invalid option\n"); return 2; }
	}
	if (request) return control(socket_path, request, is_stop);
	signal(SIGTERM, stop_signal); signal(SIGINT, stop_signal); signal(SIGPIPE, SIG_IGN);
	if (strlen(socket_path) >= sizeof(addr.sun_path)) { errno = ENAMETOOLONG; goto done; }
	strcpy(addr.sun_path, socket_path);
	server = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (server < 0) goto done;
	/* An existing pathname is intentionally not unlinked: never steal a live logger. */
	if (bind(server, (struct sockaddr *)&addr, sizeof(addr))) goto done;
	bound = 1;
	if (listen(server, 4)) goto done;
	chmod(socket_path, 0600);
	if (stopping) { errno = ECANCELED; goto cleanup; }
	if (!no_mount && (mount_card() || make_directory("/mnt/card/Linux"))) goto cleanup;
	if (stopping) { errno = ECANCELED; goto cleanup; }
	if (make_directory(directory) || create_log(directory)) goto cleanup;
	/* Persist newly created directories in their parent, too. */
	if (!no_mount && (make_directory("/mnt/card/Linux") || make_directory("/mnt/card"))) goto cleanup;
	source = open(input, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (source < 0 || drain(1)) goto cleanup;
	if (stopping) { errno = ECANCELED; goto cleanup; }
	if (write_all(STDOUT_FILENO, "R", 1)) goto cleanup;
	close(STDOUT_FILENO);
	while (!stopping) {
		struct pollfd pfd = { .fd = server, .events = POLLIN };
		if (poll(&pfd, 1, 100) > 0) {
			int client = accept(server, NULL, NULL);
			if (client >= 0) {
				fcntl(client, F_SETFD, FD_CLOEXEC);
				fcntl(client, F_SETFL, O_NONBLOCK);
				char command[16] = {0};
				if (!read_request(client, command, sizeof(command))) {
					if (!strcmp(command, "STOP\n") || !strcmp(command, "SYNC\n")) {
						int error = drain(1);
						if (!strcmp(command, "STOP\n")) {
							if (close(output)) error = -1;
							output = -1; stopping = 1;
						}
						const char *reply = error ? "ERROR: capture flush failed\n" : "OK\n";
						write_all(client, reply, strlen(reply));
					} else write_all(client, "ERROR: invalid command\n", 23);
				}
				close(client);
			}
		}
		if (!stopping && drain(0)) { perror("sdlog capture"); break; }
	}
	result = capture_error ? 1 : 0;
cleanup:
done:
	if (result) perror("sdlog");
	if (output >= 0) {
		if (source >= 0 && drain(1)) result = 1;
		if (close(output)) result = 1;
	}
	if (source >= 0) close(source);
	if (server >= 0) close(server);
	if (bound) unlink(socket_path);
	return result;
}
