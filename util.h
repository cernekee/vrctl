#ifndef _UTIL_H_
#define _UTIL_H_

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <termios.h>
#include <signal.h>
#include <sys/fcntl.h>
#include <sys/types.h>

#define L_ERROR			0
#define L_NORMAL		1
#define L_VERBOSE		2
#define L_DEBUG			3

#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))

extern int g_loglevel;
extern char *g_locked_tty;

void die(const char *fmt, ...);
void info(int level, char *fmt, ...);

int lock_tty(char *name, char *caller);
void unlock_tty(char *name);
int set_tty_defaults(int fd, int baud);

unsigned char read_byte(int fd);
void read_bytes(int fd, unsigned char *buff, int maxlen);
void flush_bytes(int fd);
int read_line(int fd, char *buf, int maxlen, int timeout);
void write_line(int fd, char *buf);

#endif /* _UTIL_H_ */
