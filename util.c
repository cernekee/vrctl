/*
 * Common utility functions
 * Copyright 2011 Kevin Cernekee
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

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
		fflush(stdout);
		va_end(ap);
	}
}

int next_token(char **in, char *tok, int maxlen)
{
	int len;

	for (len = 0; len < BUFLEN - 1; (*in)++) {
		if (**in == 0 || **in == '\r' || **in == '\n') {
			if (len == 0)
				return -1;
			goto done;
		}
		if (**in == ' ' || **in == '\t') {
			if (len != 0)
				goto done;
			continue;
		}
		*(tok++) = **in;
		len++;
	}

	/* if the loop terminates here, truncate the line and return success */

done:
	*tok = 0;
	return 0;
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
	fd_set s;

	FD_ZERO(&s);
	FD_SET(fd, &s);
	select(fd + 1, &s, NULL, NULL, NULL);

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

static void write_loop(int fd, char *buf, int len)
{
	while (len) {
		int bytes = write(fd, buf, len);
		if (bytes <= 0)
			die("EOF or write error on tty\n");
		len -= bytes;
		buf += bytes;
	}
}

void write_line(int fd, char *buf)
{
	int len = strlen(buf);
	char eol[] = "\r";

	info(L_DEBUG, "%s: sending '%s'\n", __func__, buf);
	write_loop(fd, buf, len);
	write_loop(fd, eol, 2);
}

int read_line(int fd, char *buf, int maxlen, int timeout_us)
{
	char c, *ptr = buf;
	int len = 0;

	while (len < maxlen) {
		struct timeval tv;
		fd_set s;

		FD_ZERO(&s);
		FD_SET(fd, &s);
		tv.tv_sec = timeout_us / 1000000;
		tv.tv_usec = timeout_us % 1000000;

		if (select(fd + 1, &s, NULL, NULL, &tv) != 1) {
			info(L_DEBUG, "%s: timed out\n", __func__);
			return -ETIMEDOUT;
		}
		c = read_byte(fd);
		if (c == '\r' || c == '\n') {
			if (len != 0) {
				*ptr = 0;
				info(L_DEBUG, "%s: got '%s'\n", __func__, buf);
				return len;
			}
			/* ignore empty lines or leading [\r\n] */
			continue;
		}
		*(ptr++) = c;
		len++;
	}

	*ptr = 0;
	info(L_DEBUG, "%s: out of buffer space\n", __func__);
	return -ENOSPC;
}
