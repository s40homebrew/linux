/* SPDX-License-Identifier: MIT */
/* PID 1: watchdog servicing, shell supervision and the keypad menu. */
#include <linux/input-event-codes.h>
#include <linux/watchdog.h>
#include "runtime.h"
#include "usb.h"

/* Linux's ARM EABI input_event uses 32-bit timeval fields. Avoid libc-only
 * headers pulled in by linux/input.h when compiling with nolibc. */
struct input_event {
	long sec, usec;
	unsigned short type, code;
	int value;
};
_Static_assert(sizeof(struct input_event) == 16, "ARM32 evdev ABI");

static int watchdog_fd = -1;
static int watchdog_expire_test;
static unsigned long watchdog_pings;
static pid_t sdlog_pid;
static void watchdog_ping(void);

/* SD mount/write/fsync may block. Only the child does storage I/O; PID1 keeps
 * feeding both watchdogs while waiting for a durable logging checkpoint. */
static int sdlog_wait(pid_t child, int ready_fd)
{
	unsigned long deadline = uptime_seconds() + 65;
	for (;;) {
		int status;
		if (ready_fd >= 0) {
			struct pollfd pfd = { .fd = ready_fd, .events = POLLIN };
			if (poll(&pfd, 1, 0) > 0) {
				char answer;
				if (read(ready_fd, &answer, 1) == 1 && answer == 'R') return 1;
				return 0;
			}
		}
		pid_t result = waitpid(child, &status, WNOHANG);
		if (result == child)
			return ready_fd < 0 && WIFEXITED(status) && !WEXITSTATUS(status);
		if (result < 0 || uptime_seconds() >= deadline) {
			kill(child, SIGTERM);
			return 0;
		}
		watchdog_ping();
		poll(NULL, 0, 100);
	}
}

static pid_t sdlog_spawn(const char *option, int output)
{
	pid_t child = fork();
	if (!child) {
		char *args[] = { "/bin/sdlog", (char *)option, NULL };
		char *env[] = { "PATH=/bin:/sbin", NULL };
		int null = open("/dev/null", O_RDWR);
		int errors = log_descriptor();
		if (null < 0) exit(126);
		if (errors < 0) errors = null;
		dup2(null, 0);
		dup2(errors, 2);
		dup2(output >= 0 ? output : null, 1);
		if (null > 2) close(null);
		if (output > 2) close(output);
		execve(args[0], args, env);
		exit(127);
	}
	return child;
}

static int sdlog_start(void)
{
	int ready[2], result;
	log_message("SDLOG starting; display remains PIO until durable checkpoint");
	if (pipe2(ready, O_CLOEXEC)) return 0;
	sdlog_pid = sdlog_spawn(NULL, ready[1]);
	close(ready[1]);
	result = sdlog_pid > 0 && sdlog_wait(sdlog_pid, ready[0]);
	close(ready[0]);
	if (!result) {
		log_message("SDLOG unavailable; keeping display on PIO");
		return 0;
	}
	log_message("SDLOG ready; checkpoint before selected display settings");
	pid_t child = sdlog_spawn("--checkpoint", -1);
	result = child > 0 && sdlog_wait(child, -1);
	if (!result) log_message("SDLOG checkpoint failed; keeping display on PIO");
	return result;
}

static int command_line_has(const char *option)
{
	char buf[1024], *token, *end;
	int fd = open("/proc/cmdline", O_RDONLY);
	ssize_t count;
	if (fd < 0)
		return 0;
	count = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (count <= 0)
		return 0;
	buf[count] = 0;
	for (token = buf; *token; token = end) {
		while (*token == ' ' || *token == '\n')
			token++;
		end = token;
		while (*end && *end != ' ' && *end != '\n')
			end++;
		if ((size_t)(end - token) == strlen(option) &&
		    !strncmp(token, option, end - token))
			return 1;
	}
	return 0;
}

static int display_option(const char *path, const char *value)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	ssize_t written;

	if (fd < 0)
		return -1;
	written = write(fd, value, strlen(value));
	close(fd);
	return written == (ssize_t)strlen(value) ? 0 : -1;
}

static void display_start(int durable_log)
{
	const char *format = command_line_has("rm443.lcd=rgb888") ? "rgb888" :
		command_line_has("rm443.lcd=rgb666") ? "rgb666" : "rgb565";
	const char *transport = durable_log && command_line_has("rm443.display=dma") ? "dma" : "pio";

	if (display_option("/sys/class/drm/card0/device/pixel_format", format))
		log_message("Display color setting failed");
	if (display_option("/sys/class/drm/card0/device/transport", transport))
		log_message("Display transport setting failed");
	log_file("/sys/class/drm/card0/device/transfer_stats", 2);
}

static void watchdog_start(void)
{
	int timeout;
	watchdog_expire_test = command_line_has("rm443.wdt-test=expire");
	watchdog_fd = open("/dev/watchdog0", O_WRONLY | O_CLOEXEC);
	if (watchdog_fd < 0) {
		log_message("WATCHDOG unavailable");
		return;
	}
	if (watchdog_expire_test || command_line_has("rm443.wdt-test=bus")) {
		timeout = 5;
		if (ioctl(watchdog_fd, WDIOC_SETTIMEOUT, &timeout) < 0) {
			log_message("WATCHDOG expiry test setup FAIL");
			watchdog_expire_test = 0;
		} else if (watchdog_expire_test) {
			log_message("WATCHDOG TEST: keepalives withheld");
		}
	}
	if (!ioctl(watchdog_fd, WDIOC_GETTIMEOUT, &timeout)) {
		log_begin("WATCHDOG owned by PID1 timeout=");
		log_decimal(timeout); log_emit();
	}
}

static void watchdog_bounds_test(void)
{
	static const int invalid[] = {-1, 0, 64, 128, 2147483647};
	int before, after, value, failed = 0;
	unsigned i;
	if (watchdog_fd < 0 || ioctl(watchdog_fd, WDIOC_GETTIMEOUT, &before)) {
		log_message("WATCHDOG bounds FAIL: unavailable");
		return;
	}
	for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		value = invalid[i];
		if (!ioctl(watchdog_fd, WDIOC_SETTIMEOUT, &value) || errno != EINVAL)
			failed = 1;
		if (ioctl(watchdog_fd, WDIOC_GETTIMEOUT, &after) || after != before)
			failed = 1;
	}
	log_message(failed ? "WATCHDOG bounds FAIL" : "WATCHDOG bounds PASS: invalid timeouts rejected");
}

static void watchdog_ping(void)
{
	if (watchdog_fd < 0 || watchdog_expire_test)
		return;
	if (ioctl(watchdog_fd, WDIOC_KEEPALIVE, NULL) < 0)
		log_message("WATCHDOG keepalive FAIL");
	else
		watchdog_pings++;
}

static void watchdog_status(void)
{
	int remaining;
	log_begin("WATCHDOG successful pings="); log_decimal(watchdog_pings); log_emit();
	if (watchdog_fd >= 0 && !ioctl(watchdog_fd, WDIOC_GETTIMELEFT, &remaining)) {
		log_begin("WATCHDOG remaining="); log_decimal(remaining); log_emit();
	}
	log_file("/sys/class/watchdog/watchdog0/identity", 1);
	log_file("/sys/class/watchdog/watchdog0/timeout", 1);
	log_file("/sys/class/watchdog/watchdog0/state", 1);
	log_file("/sys/kernel/rm443_secure_watchdog/status", 8);
}

static void process_test(void)
{
	int status = 0;
	pid_t child = fork();
	if (!child) {
		/* Exercise a user page write, process scheduling and exit/reaping. */
		volatile char *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
				 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (page == MAP_FAILED)
			exit(2);
		page[0] = 42;
		page[4095] = 99;
		if (page[0] != 42 || page[4095] != 99)
			exit(3);
		munmap((void *)page, 4096);
		exit(42);
	}
	if (child < 0) {
		log_message("SELFTEST FAIL: fork");
		return;
	}
	if (waitpid(child, &status, 0) == child && WIFEXITED(status) &&
	    WEXITSTATUS(status) == 42)
		log_message("SELFTEST PASS: fork mmap waitpid");
	else
		log_message("SELFTEST FAIL: child status");
}

static void timer_irq_count(void)
{
	char buf[1024], *start, *end, *scan;
	int fd = open("/proc/interrupts", O_RDONLY);
	ssize_t n;
	if (fd < 0)
		return;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return;
	buf[n] = 0;
	for (start = buf; *start; start = end + 1) {
		end = strchr(start, '\n');
		if (!end)
			break;
		*end = 0;
		for (scan = start; *scan; scan++)
			if (!strncmp(scan, "raps-timer2", 11)) {
				log_begin(start); log_emit();
				return;
			}
	}
}

static int storage_shutdown(void)
{
	pid_t child = fork();
	int status;
	unsigned long deadline = uptime_seconds() + 60;
	if (!child) {
		char *args[] = {"/bin/card", "unmount", NULL};
		char *env[] = {"PATH=/bin:/sbin", NULL};
		/* /dev/console may be the disconnected USB TTY. A short-lived
		 * shell must not wait for its terminal to drain on exit. Report
		 * unmount errors through kmsg and give the helper no terminal. */
		int input = open("/dev/null", O_RDONLY);
		int output = log_descriptor();
		if (output < 0)
			output = open("/dev/null", O_WRONLY);
		if (input < 0 || output < 0)
			exit(126);
		dup2(input, 0);
		dup2(output, 1);
		dup2(output, 2);
		if (input > 2)
			close(input);
		if (output > 2)
			close(output);
		execve(args[0], args, env);
		exit(127);
	}
	if (child < 0)
		return 0;
	/* Sync and unmount can wait for card I/O. PID1 must continue feeding
	 * both supervisors while the child does that work. Never power off
	 * after a failed/busy unmount or an unresponsive storage request. */
	for (;;) {
		pid_t result = waitpid(child, &status, WNOHANG);
		if (result == child)
			return WIFEXITED(status) && WEXITSTATUS(status) == 0;
		if (result < 0 || uptime_seconds() >= deadline) {
			kill(child, SIGTERM);
			return 0;
		}
		watchdog_ping();
		poll(NULL, 0, 100);
	}
}

static void help(void)
{
	log_message("System menu");
	log_message("1 Status  2 Interrupts");
	log_message("3 Process test");
	log_message("4 Hardware  5 Watchdog");
	log_message("6 USB");
	log_message("8 Power off  9 Restart");
	log_message("0 / Enter: Menu");
}

static void key(unsigned code)
{
	log_begin("key code="); log_decimal(code); log_emit();
	switch (code) {
	case KEY_1:
		log_file("/proc/uptime", 1);
		log_file("/proc/meminfo", 7);
		break;
	case KEY_2:
		log_file("/proc/interrupts", 12);
		break;
	case KEY_3:
		process_test();
		break;
	case KEY_4:
		log_file("/sys/devices/platform/20030700.power/status", 32);
		break;
	case KEY_5:
		watchdog_status();
		break;
	case KEY_6:
		usb_status();
		break;
	case KEY_POWER:
		log_message("power key pressed");
		break;
	case KEY_8:
		log_message("POWER OFF requested");
		if (!storage_shutdown()) {
			log_message("POWER OFF cancelled: unmount card first");
			break;
		}
		log_message("Storage ready; kernel power off");
		watchdog_ping();
		reboot(LINUX_REBOOT_CMD_POWER_OFF);
		log_message("POWER OFF syscall failed");
		break;
	case KEY_9:
		log_message("RESTART requested");
		if (!storage_shutdown()) {
			log_message("RESTART cancelled: unmount card first");
			break;
		}
		log_message("Storage ready; kernel restart");
		watchdog_ping();
		reboot(LINUX_REBOOT_CMD_RESTART);
		log_message("RESTART syscall failed");
		break;
	case KEY_0:
	case KEY_ENTER:
		help();
		break;
	}
}

int main(void)
{
	struct pollfd fds[4];
	unsigned long next, beats = 0;
	int proc_ok, sys_ok, dev_ok, heartbeat;
	unsigned inputs = 0, slot;
	dev_ok = mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
	log_open();
	proc_ok = mount("proc", "/proc", "proc", 0, NULL);
	heartbeat = command_line_has("rm443.heartbeat=1");
	sys_ok = mount("sysfs", "/sys", "sysfs", 0, NULL);
	if (!sys_ok && mount("debugfs", "/sys/kernel/debug", "debugfs", 0, NULL))
		log_message("debugfs mount failure");
	log_begin("USERSPACE READY pid="); log_decimal(getpid()); log_emit();
	if (dev_ok || proc_ok || sys_ok)
		log_message("filesystem mount failure");
	else
		log_message("proc sysfs devtmpfs mounted");
	mkdir("/dev/pts", 0755);
	if (mount("devpts", "/dev/pts", "devpts", 0, NULL))
		log_message("devpts mount failure");
	/* tmpfs gives scratch files accounting and an explicit limit, unlike
	 * unbounded initramfs writes. Its default size scales with physical RAM. */
	if (mount("tmpfs", "/tmp", "tmpfs", 0, "mode=1777"))
		log_message("tmpfs mount failure");
	mkdir("/dev/shm", 01777);
	if (mount("tmpfs", "/dev/shm", "tmpfs", 0, "mode=1777,size=25%"))
		log_message("shared-memory tmpfs mount failure");
	watchdog_start();
	if (command_line_has("rm443.wdt-test=bounds"))
		watchdog_bounds_test();
	watchdog_ping();
	process_test();
	usb_service();
	if (command_line_has("rm443.sdlog=0")) {
		log_message("SDLOG disabled by boot option; keeping display on PIO");
		display_start(0);
	} else {
		display_start(sdlog_start());
	}
	help();
	for (slot = 0; slot < 4; slot++) {
		char path[] = "/dev/input/event0";
		int fd;
		path[sizeof(path) - 2] += slot;
		fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd >= 0) {
			fds[inputs].fd = fd;
			fds[inputs++].events = POLLIN;
			log_begin("input ready "); log_append(path); log_emit();
		}
	}
	if (inputs)
		log_message("keypad ready");
	else
		log_message("keypad unavailable");
	next = uptime_seconds() + 5;
	for (;;) {
		struct input_event events[8];
		ssize_t n;
		unsigned i;
		pid_t child;
		poll(fds, inputs, 1000);
		watchdog_ping();
		usb_service();
		for (slot = 0; slot < inputs; slot++) {
			if (!(fds[slot].revents & POLLIN))
				continue;
			while ((n = read(fds[slot].fd, events, sizeof(events))) > 0)
				for (i = 0; i < (unsigned)n / sizeof(events[0]); i++)
					if (events[i].type == EV_KEY && events[i].value == 1)
						key(events[i].code);
		}
		if (heartbeat && uptime_seconds() >= next) {
			log_begin("HEARTBEAT "); log_decimal(++beats);
			log_append(" uptime="); log_decimal(uptime_seconds()); log_emit();
			timer_irq_count();
			log_begin("WATCHDOG pings="); log_decimal(watchdog_pings); log_emit();
			next = uptime_seconds() + 5;
		}
		while ((child = waitpid(-1, NULL, WNOHANG)) > 0) {
			if (child == sdlog_pid) {
				sdlog_pid = 0;
				log_message("SDLOG child exited; persistent capture stopped");
			}
			usb_child_exited(child);
		}
	}
}
