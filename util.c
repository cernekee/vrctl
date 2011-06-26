#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <termios.h>
#include <signal.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/fcntl.h>
#include <sys/types.h>
#include "util.h"

#define BUFLEN			256

int g_loglevel = L_NORMAL;
char *g_locked_tty = NULL;

void die(const char *fmt, ...)
{
	va_list ap;

	/* not pretty but it lets us safely die() from basically anywhere */
	if (g_locked_tty)
		unlock_tty(g_locked_tty);

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	exit(1);
}

void info(int level, char *fmt, ...)
{
	va_list ap;

	if (g_loglevel >= level) {
		va_start(ap, fmt);
		vprintf(fmt, ap);
		va_end(ap);
	}
}

static int get_lockname(char *dev, char *buf)
{
	/* example: /dev/ttyS0 -> /var/lock/LCK..ttyS0 */
	char *tmp = strrchr(dev, '/');

	if (tmp)
		dev = tmp + 1;
	if (snprintf(buf, BUFLEN, "/var/lock/LCK..%s", dev) >= BUFLEN)
		return -1;
	return 0;
}

static int write_lockfile(char *lockname, char *caller)
{
	int fd, len;
	char buf[BUFLEN];

	fd = open(lockname, O_RDWR | O_CREAT | O_EXCL, 0644);
	if (fd < 0)
		return -1;
	len = snprintf(buf, BUFLEN, "%10d %s root\n", getpid(), caller);
	write(fd, buf, len);
	close(fd);
	return 0;
}

int lock_tty(char *name, char *caller)
{
	char lockname[BUFLEN], buf[BUFLEN];
	int fd, len, pid = 0;

	/* /var/lock might not even be accessible */
	if (access("/var/lock", R_OK | W_OK) < 0)
		return 0;

	if (get_lockname(name, lockname) == -1)
		return -1;
	
	fd = open(lockname, O_RDONLY);
	if (fd < 0)
		return write_lockfile(lockname, caller);

	len = read(fd, buf, BUFLEN - 1);
	close(fd);

	if (len < 0)
		return -1;

	buf[len] = 0;

	sscanf(buf, "%d", &pid);
	if (pid != 0) {
		/* somebody else really locked the device */
		if (kill(pid, 0) >= 0)
			return -1;
	}
	/* stale lock - take it over */
	unlink(lockname);
	return write_lockfile(lockname, caller);
}

void unlock_tty(char *name)
{
	char lockname[BUFLEN];

	if (get_lockname(name, lockname) != -1)
		unlink(lockname);
}

int set_tty_defaults(int fd, int baud)
{
	struct termios termios;

	if (tcgetattr(fd, &termios) != 0)
		return -1;

	termios.c_iflag = 0;
	termios.c_oflag = 0;
	termios.c_cflag = CS8 | CLOCAL | CREAD;
	termios.c_lflag = 0;
	switch (baud) {
		case 115200: cfsetspeed(&termios, B115200); break;
		case 57600: cfsetspeed(&termios, B57600); break;
		case 38400: cfsetspeed(&termios, B38400); break;
		case 19200: cfsetspeed(&termios, B19200); break;
		case 9600: cfsetspeed(&termios, B9600); break;
		default:
			return -1;
	}

	if (tcsetattr(fd, TCSANOW, &termios) != 0)
		return -1;
	return 0;
}

unsigned char read_byte(int fd)
{
	char c;
	if (read(fd, &c, 1) != 1)
		die("EOF or read error on tty\n");
	return c;
}

void read_bytes(int fd, unsigned char *buf, int maxlen)
{
	while (maxlen) {
		int bytes = read(fd, buf, maxlen);
		if (bytes <= 0)
			die("EOF or read error on tty\n");
		maxlen -= bytes;
		buf += bytes;
	}
}

void flush_bytes(int fd)
{
	fd_set s;
	struct timeval tv;

	FD_ZERO(&s);
	FD_SET(fd, &s);
	tv.tv_sec = tv.tv_usec = 0;

	while (select(fd + 1, &s, NULL, NULL, &tv) > 0)
		read_byte(fd);
}

void write_line(int fd, char *buf)
{
	int len = strlen(buf);

	while (len) {
		int bytes = write(fd, buf, len);
		if (bytes <= 0)
			die("EOF or write error on tty\n");
		len -= bytes;
		buf += bytes;
	}
}

int read_line(int fd, char *buf, int maxlen, int timeout)
{
	char c;
	int len = 0;

	while (len < maxlen) {
		/* XXX need to add timeout */
		c = read_byte(fd);
		if (c == '\r' || c == '\n') {
			if (len != 0) {
				*buf = 0;
				return len;
			}
			/* ignore empty lines or leading [\r\n] */
			continue;
		}
		*(buf++) = c;
		len++;
	}

	/* out of buffer space and still no EOL */
	*buf = 0;
	return -ENOSPC;
}
