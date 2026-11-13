/* SPDX-License-Identifier: MIT */
#include "runtime.h"
#include "usb.h"
#include <asm/termbits.h>
#include <linux/in.h>
#include <linux/sockios.h>

/* nolibc has no libc net/if.h. Linux ARM EABI ifreq is 32 bytes. */
struct rm_ifreq {
	char name[16];
	union { struct sockaddr_in addr; short flags; char storage[16]; } value;
};
_Static_assert(sizeof(struct rm_ifreq) == 32, "ARM32 ifreq ABI");
_Static_assert(sizeof(struct sockaddr_in) == 16, "IPv4 socket ABI");
static pid_t serial_pid, telnet_pid;
static int usb_network_ready;
static unsigned long usb_retry;

void usb_status(void)
{
	/* Fit the enumeration counters on the LCD. Full USB and network status
	 * remains available through the shell. */
	log_file("/sys/devices/platform/20036600.usb/enumeration", 13);
}

static void usb_network_start(void)
{
	struct rm_ifreq req = { .name = "usb0" };
	int fd = my_syscall3(__NR_socket, 2 /* AF_INET */, 2 /* SOCK_DGRAM */ | O_CLOEXEC, 0);
	if (fd < 0)
		return;
	req.value.addr.sin_family = 2;
	req.value.addr.sin_addr.s_addr = __builtin_bswap32(0xc0a80702); /* 192.168.7.2 */
	if (ioctl(fd, SIOCSIFADDR, &req) < 0)
		goto done;
	req.value.addr.sin_addr.s_addr = __builtin_bswap32(0xffffff00);
	if (ioctl(fd, SIOCSIFNETMASK, &req) < 0 || ioctl(fd, SIOCGIFFLAGS, &req) < 0)
		goto done;
	req.value.flags |= 1 /* IFF_UP */;
	if (ioctl(fd, SIOCSIFFLAGS, &req) < 0)
		goto done;
	usb_network_ready = 1;
	log_message("usb0: 192.168.7.2/24");
done:
	close(fd);
}

static void usb_spawn_serial(void)
{
	pid_t child;
	int fd = open("/dev/ttyGS0", O_RDWR | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
	if (fd < 0)
		return;
	close(fd);
	child = fork();
	if (!child) {
		struct termios mode;
		char *argv[] = { "/bin/sh", "-i", NULL };
		char *env[] = { "PATH=/bin:/sbin", "HOME=/root", "TERM=vt100",
			"USER=root", "ENV=/etc/profile", "PS1=rm443# ", NULL };
		my_syscall0(__NR_setsid);
		/* A blocking tty open is confined to this child, never PID 1. */
		fd = open("/dev/ttyGS0", O_RDWR);
		if (fd < 0)
			exit(111);
		ioctl(fd, TIOCSCTTY, NULL);
		if (!ioctl(fd, TCGETS, &mode)) {
			mode.c_iflag = ICRNL;
			mode.c_oflag = OPOST | ONLCR;
			mode.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK;
			mode.c_cflag = B115200 | CS8 | CREAD | CLOCAL;
			mode.c_cc[VINTR] = 3; mode.c_cc[VQUIT] = 28;
			mode.c_cc[VERASE] = 127; mode.c_cc[VKILL] = 21;
			mode.c_cc[VEOF] = 4; mode.c_cc[VSUSP] = 26;
			mode.c_cc[VMIN] = 1; mode.c_cc[VTIME] = 0;
			ioctl(fd, TCSETS, &mode);
		}
		dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
		if (fd > 2)
			close(fd);
		execve(argv[0], argv, env);
		write(2, "Cannot exec BusyBox sh\n", 22);
		exit(111);
	}
	if (child > 0) {
		serial_pid = child;
		log_message("USB serial shell starting");
	}
}

static void usb_spawn_telnet(void)
{
	pid_t child = fork();
	if (!child) {
		char *argv[] = { "/bin/busybox", "telnetd", "-F", "-b", "192.168.7.2",
			"-p", "23", "-l", "/bin/sh", NULL };
		char *env[] = { "PATH=/bin:/sbin", "HOME=/root", "TERM=vt100",
			"USER=root", "ENV=/etc/profile", "PS1=rm443# ", NULL };
		int fd = open("/dev/null", O_RDWR);
		if (fd >= 0) {
			dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
			if (fd > 2)
				close(fd);
		}
		execve(argv[0], argv, env);
		exit(111);
	}
	if (child > 0) {
		telnet_pid = child;
		log_message("Telnet starting on 192.168.7.2:23");
	}
}

void usb_child_exited(pid_t child)
{
	if (child == serial_pid) {
		serial_pid = 0;
		log_message("USB shell exited; restarting");
	}
	if (child == telnet_pid) {
		telnet_pid = 0;
		log_message("Telnet exited; restarting");
	}
	usb_retry = uptime_seconds() + 1;
}

void usb_service(void)
{
	unsigned long now = uptime_seconds();
	if (now < usb_retry)
		return;
	if (!usb_network_ready)
		usb_network_start();
	if (!serial_pid)
		usb_spawn_serial();
	if (usb_network_ready && !telnet_pid)
		usb_spawn_telnet();
	usb_retry = now + 2;
}
