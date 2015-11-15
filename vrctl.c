/*
 * vrctl - Z-Wave VRC0P utility
 * Copyright 2012 Kevin Cernekee <cernekee@gmail.com>
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

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <stdarg.h>
#include <termios.h>
#include <signal.h>
#include <getopt.h>
#include <ctype.h>
#include <sys/select.h>
#include <sys/fcntl.h>
#include <sys/types.h>
#include "util.h"

#define VERSION			"0.1"
#define BUFLEN			64
#define DEFAULT_DEV		"/dev/vrc0p"
#define RC_NAME			".vrctlrc"
#define TIMEOUT			3000000
#define TIMEOUT_UPGRADE		4000000
#define NODEID_ALL		-2
#define MAX_NODEID		232

#define __func__		__FUNCTION__

struct resp {
	char			type0;
	unsigned int		arg0;
	char			type1;
	unsigned int		arg1;
	unsigned int		arg1_precision;
};

struct node_alias {
	int			nodeid;
	char			nodename[BUFLEN];
	struct node_alias	*next;
};

static struct node_alias *alias_head = NULL, *alias_tail = NULL;
static char *rc_port = NULL;

typedef int (*cmd_handler_t)(int devfd, int nodeid, char *arg);

struct vrctl_cmd {
	char			*name;
	int			arg_required;
	int			is_unicast;
	cmd_handler_t		handler;
};

/*
 * RC FILE
 */

/* check the alias list - next case-insensitive match wins */
static struct node_alias *lookup_next_alias(char *nodename,
	struct node_alias *start)
{
	struct node_alias *a = start ? start->next : alias_head;

	for (; a != NULL; a = a->next)
		if (strcasecmp(a->nodename, nodename) == 0) {
			info(L_DEBUG, "%s: '%s' => node %lu\n", __func__,
				nodename, a->nodeid);
			return a;
		}
	return NULL;
}

/* scan the alias list for a nodeid; first match wins */
static const char *nodeid_to_nodename(int nodeid)
{
	struct node_alias *a = alias_head;

	for (; a != NULL; a = a->next)
		if (nodeid == a->nodeid)
			return a->nodename;
	return NULL;
}

static void parse_rcline(char *filename, int linenum, char *line)
{
	char *p = line, tok[BUFLEN];

	if (next_token(&p, tok, BUFLEN) < 0)
		return;		/* empty line */

	if (tok[0] == '#')
		return;		/* comment */

	if (strcasecmp(tok, "alias") == 0) {
		struct node_alias *a, *chain;
		unsigned long nodeid;
		char *endp;

		a = malloc(sizeof(*a));
		if (!a)
			die("out of memory\n");
		a->next = NULL;

		if (next_token(&p, tok, BUFLEN) < 0) {
			info(L_WARNING, "%s:%d: missing node name\n",
				filename, linenum);
			free(a);
			return;
		}
		strncpy(a->nodename, tok, BUFLEN);

		if (next_token(&p, tok, BUFLEN) < 0) {
			info(L_WARNING, "%s:%d: missing node number\n",
				filename, linenum);
			free(a);
			return;
		}

		/* it is legal to refer to a previously defined alias */
		chain = lookup_next_alias(tok, NULL);
		if (chain)
			nodeid = chain->nodeid;
		else {
			nodeid = strtoul(tok, &endp, 10);
			if (tok[0] == 0 || *endp != 0 || nodeid > MAX_NODEID) {
				info(L_WARNING, "%s:%d: invalid node number\n",
					filename, linenum);
				free(a);
				return;
			}
		}
		a->nodeid = nodeid;

		/* note: g_loglevel is probably not set yet */
		info(L_DEBUG, "%s: adding alias '%s' for nodeid %d\n",
			__func__, a->nodename, a->nodeid);

		if (alias_tail != NULL) {
			alias_tail->next = a;
			alias_tail = a;
		} else {
			alias_head = alias_tail = a;
		}
		return;
	}

	if (strcasecmp(tok, "port") == 0) {
		if (next_token(&p, tok, BUFLEN) < 0) {
			info(L_WARNING, "%s:%d: missing device name\n",
				filename, linenum);
			return;
		}
		rc_port = strdup(tok);
		return;
	}

	info(L_WARNING, "%s:%d: unrecognized option '%s'\n",
		filename, linenum, tok);
}

static void read_rcfile(void)
{
	FILE *f;
	char filename[BUFLEN], buf[BUFLEN];
	char *homedir;
	int linenum = 1;

	homedir = getenv("HOME");
	if (!homedir) {
		info(L_WARNING, "warning: HOME is not set so I can't read "
			"$HOME/%s\n", RC_NAME);
		return;
	}

	/* .vrctlrc does not necessarily exist at all */
	snprintf(filename, BUFLEN, "%s/%s", homedir, RC_NAME);
	f = fopen(filename, "r");
	if (f == NULL)
		return;

	while (fgets(buf, BUFLEN, f) != NULL)
		parse_rcline(filename, linenum++, buf);

	if (ferror(f))
		info(L_WARNING, "warning: errors detected reading %s\n",
			filename);
	fclose(f);
}

/*
 * VRC0P COMMANDS
 */

static int read_resp(int devfd, char *buf, int maxlen, int timeout_us)
{
	int ret;
	ret = read_line(devfd, buf, maxlen, timeout_us);

	if (ret == -ENOSPC)
		die("error: input overflow from VRC0P\n");
	if (ret == -ETIMEDOUT)
		die("error: timeout waiting for command response\n");
	return ret;
}

static int parse_uint(char *str, int maxlen, char *name, int maxval)
{
	int i, ret = 0;

	for (i = 0; !maxlen || i < maxlen; i++) {
		if (str[i] == 0)
			break;
		if (!isdigit(str[i]))
			die("error: invalid %s '%s'\n", name, str);
		ret = ret * 10 + str[i] - '0';
	}
	if (ret < 0 || (maxval != 0 && ret > maxval))
		die("error: %s must be lower than %d\n", name, maxval);
	return ret;
}

static void parse_temp(char *buf, struct resp *r)
{
	int fmt = parse_uint(&buf[0], 3, "temp format", 0);
	int bytes = fmt & 0x07;

	r->type1 = (fmt & 0x18) ? 'F' : 'C';
	r->arg1_precision = (fmt >> 5) & 0x07;

	if (!bytes || bytes > 2 || strlen(buf) < 3 + bytes*4)
		die("error: malformed temp response: %s\n", buf);

	if (bytes == 1) {
		r->arg1 = parse_uint(&buf[4], 3, "temp", 0);
	} else {
		r->arg1 = (parse_uint(&buf[4], 3, "temp", 0) << 8) |
			  (parse_uint(&buf[8], 3, "temp", 0) << 0);
	}
}

static int parse_resp(char *buf, struct resp *r)
{
	/*
	 * Some valid inputs look like:
	 * <E001
	 * <X000
	 * <N003L000 (light level)
	 * <N004:049,005,001,009,075 (temp sensor report)
	 *
	 * Numbers are in decimal notation, typically 0-255.
	 */

	if (buf[0] != '<')
		return -1;
	if (buf[1] < 'A' || buf[1] > 'Z')
		return -1;

	r->type0 = buf[1];
	r->arg0 = parse_uint(&buf[2], 3, "arg0", 0);

	if (buf[5] == 0) {
		r->type1 = 0;
		return 0;
	}

	if (!strncmp(&buf[5], ":049,005,001,", 13) ||
	    !strncmp(&buf[5], ":067,003,002,", 13)) {
		/* parse temperature sensor result */
		parse_temp(&buf[18], r);
	} else if (!strncmp(&buf[5], ":064,003,", 9)) {
		/* parse thermostat mode */
		r->arg1 = parse_uint(&buf[14], 3, "thermostat mode", 0);
	} else {
		/* parse other result types (typically light level) */
		r->type1 = buf[5];
		r->arg1 = parse_uint(&buf[6], 3, "arg1", 0);
	}

	return 0;
}

static void wait_resp(int devfd, char expected_type, struct resp *r)
{
	char buf[BUFLEN];

	do {
		read_resp(devfd, buf, BUFLEN, TIMEOUT);
		if (parse_resp(buf, r) < 0)
			die("error: received bad response '%s'\n", buf);

		if (r->type0 == 'E' && r->arg0 != 0)
			die("error: received E%03d while waiting for "
				"'%c' response\n", r->arg0, expected_type);
	} while (r->type0 != expected_type);
}

static int send_then_recv(int devfd, char expected_type, char *fmt, ...)
{
	va_list ap;
	char buf[BUFLEN];
	struct resp r;

	va_start(ap, fmt);
	vsnprintf(buf, BUFLEN, fmt, ap);
	va_end(ap);

	write_line(devfd, buf);
	wait_resp(devfd, expected_type, &r);
	return r.arg0;
}

static void sync_interface(int devfd)
{
	char buf[BUFLEN];
	int i, ret;

	usleep(25000);
	flush_bytes(devfd);
	/* hit "enter" on the serial line until we get <E000 back */
	for (i = 0; i < 3; i++) {
		write_line(devfd, "");

		ret = read_line(devfd, buf, BUFLEN, TIMEOUT);

		if (ret > 0 && strcmp(buf, "<E000") == 0)
			return;
		sleep(1);
	}
	die("error: can't establish communication with VRC0P interface\n");
}

static void update_nodes(int devfd)
{
	send_then_recv(devfd, 'E', ">UP");
}

/*
 * USER COMMAND HANDLERS
 *
 * Unless otherwise specified, the return value will be:
 *
 *   0 - success
 *  <0 - Xnnn error code from the VRC0P (0-255)
 *  >0 - dim level (0-255 - handle_status() only)
 */

static int handle_on(int devfd, int nodeid, char *arg)
{
	int ret;

	if (nodeid == NODEID_ALL)
		ret = send_then_recv(devfd, 'X', ">N,ON");
	else
		ret = send_then_recv(devfd, 'X', ">N%03dON", nodeid);
	if (ret != 0)
		info(L_WARNING, "node %d returned X%03x for ON command\n",
			nodeid, ret);
	return -ret;
}

static int handle_off(int devfd, int nodeid, char *arg)
{
	int ret;

	if (nodeid == NODEID_ALL)
		ret = send_then_recv(devfd, 'X', ">N,OF");
	else
		ret = send_then_recv(devfd, 'X', ">N%03dOF", nodeid);
	if (ret != 0)
		info(L_WARNING, "node %d returned X%03x for OFF command\n",
			nodeid, ret);
	return -ret;
}

static int handle_bounce(int devfd, int nodeid, char *arg)
{
	int ret;

	ret = handle_off(devfd, nodeid, arg);
	if (ret != 0)
		return ret;

	usleep(500000);

	return handle_on(devfd, nodeid, arg);
}

static int handle_status_quiet(int devfd, int nodeid, char *arg)
{
	int ret;
	struct resp r;

	ret = send_then_recv(devfd, 'X', ">?N%03d", nodeid);
	if (ret != 0) {
		info(L_WARNING, "node %d returned X%03x for STATUS command\n",
			nodeid, ret);
		return -ret;
	}

	do {
		wait_resp(devfd, 'N', &r);
	} while (r.arg0 != nodeid || r.type1 != 'L');

	return r.arg1;
}

static int handle_status(int devfd, int nodeid, char *arg)
{
	int ret = handle_status_quiet(devfd, nodeid, arg);
	info(L_NORMAL, "%03d\n", ret);
	return ret;
}

static int handle_toggle(int devfd, int nodeid, char *arg)
{
	int ret;

	ret = handle_status_quiet(devfd, nodeid, arg);
	if (ret < 0)
		return ret;
	if (ret == 0)
		return handle_on(devfd, nodeid, arg);
	else
		return handle_off(devfd, nodeid, arg);
}

static int handle_level(int devfd, int nodeid, char *arg)
{
	int ret;
	int level = parse_uint(arg, 0, "brightness level", 255);

	if (nodeid == NODEID_ALL)
		ret = send_then_recv(devfd, 'X', ">N,L%03d", level);
	else
		ret = send_then_recv(devfd, 'X', ">N%03dL%03d", nodeid, level);
	if (ret != 0)
		info(L_WARNING, "node %d returned X%03x for LEVEL command\n",
			nodeid, ret);
	return ret;
}

static int handle_lock_level(int devfd, int nodeid, int level)
{
	int ret;

	ret = send_then_recv(devfd, 'X', ">N%03dSS98,1,%d", nodeid, level);
	if (ret != 0)
		info(L_WARNING, "node %d returned X%03x for LOCK/UNLOCK "
			"command\n", nodeid, ret);
	return -ret;
}

static int handle_lock(int devfd, int nodeid, char *arg)
{
	return handle_lock_level(devfd, nodeid, 255);
}

static int handle_unlock(int devfd, int nodeid, char *arg)
{
	return handle_lock_level(devfd, nodeid, 0);
}

static int handle_scene(int devfd, int nodeid, char *arg)
{
	int ret;
	int scene = parse_uint(arg, 0, "scene number", MAX_NODEID);

	if (nodeid == NODEID_ALL)
		ret = send_then_recv(devfd, 'X', ">N,S%d", scene);
	else
		ret = send_then_recv(devfd, 'X', ">N%03dS%d", nodeid, scene);
	if (ret != 0)
		info(L_WARNING, "node %d returned X%03x for SCENE command\n",
			nodeid, ret);
	return -ret;
}

static int handle_temp_common(int devfd, int nodeid, char *arg)
{
	struct resp r;
	int precision;

	do {
		wait_resp(devfd, 'N', &r);
	} while (r.arg0 != nodeid || (r.type1 != 'F' && r.type1 != 'C'));

	for (precision = 1; r.arg1_precision; r.arg1_precision--)
		precision *= 10;
	info(L_NORMAL, "%d.%d%c\n",
		r.arg1 / precision, r.arg1 % precision, r.type1);

	return r.arg1;
}

static int handle_temp(int devfd, int nodeid, char *arg)
{
	int ret;

	ret = send_then_recv(devfd, 'X', ">N%03dSE49,4", nodeid);
	if (ret != 0) {
		info(L_WARNING, "node %d returned X%03x for TEMP command\n",
			nodeid, ret);
		return -ret;
	}

	return handle_temp_common(devfd, nodeid, arg);
}

static int handle_setpoint(int devfd, int nodeid, char *arg)
{
	int ret;
	struct resp r;

	/* get thermostat mode */
	ret = send_then_recv(devfd, 'X', ">N%03dSE64,2", nodeid);
	do {
		wait_resp(devfd, 'N', &r);
	} while (r.arg0 != nodeid);

	if (r.arg1 == 0) {
		info(L_NORMAL, "OFF\n");
		return 0;
	}

	/* get setpoint temperature */
	ret = send_then_recv(devfd, 'X', ">N%03dSE67,2,2", nodeid);
	if (ret != 0) {
		info(L_WARNING, "node %d returned X%03x for SETPOINT command\n",
			nodeid, ret);
		return -ret;
	}

	return handle_temp_common(devfd, nodeid, arg);
}

static int handle_fan(int devfd, int nodeid, char *arg)
{
	int ret;
	int enable = parse_uint(arg, 0, "fan enable", 1);

	ret = send_then_recv(devfd, 'X', ">N%03dSE68,1,%d", nodeid, enable);
	if (ret != 0)
		info(L_WARNING, "node %d returned X%03x for FAN command\n",
			nodeid, ret);
	return ret;
}

static int handle_heat_common(int devfd, int nodeid, char *arg, int mode)
{
	int setpoint = parse_uint(arg, 2, "setpoint", 99);
	int units = 1, ret;

	if (strlen(arg) >= 3 && tolower(arg[2]) == 'c')
		units = 0;

	if (setpoint) {
		if (units)
			ret = send_then_recv(devfd, 'X', ">N%03dSE67,1,2,9,%d",
					     nodeid, setpoint);
		else
			ret = send_then_recv(devfd, 'X', ">N%03dSE67,1,2,17,%d",
					     nodeid, setpoint);
		if (ret != 0) {
			info(L_WARNING,
				"node %d returned X%03x for SETPOINT command\n",
				nodeid, ret);
			return -ret;
		}
	} else {
		mode = 0;
	}

	ret = send_then_recv(devfd, 'X', ">N%03dSE64,1,%d", nodeid, mode);
	if (ret != 0) {
		info(L_WARNING, "node %d returned X%03x for MODE command\n",
			nodeid, ret);
		return -ret;
	}

	return 0;
}

static int handle_heat(int devfd, int nodeid, char *arg)
{
	return handle_heat_common(devfd, nodeid, arg, 1);
}

static int handle_cool(int devfd, int nodeid, char *arg)
{
	return handle_heat_common(devfd, nodeid, arg, 2);
}

static void search_by_type(int devfd, int gen_class, char *class_name)
{
	int ret, i;
	const char *nodename;

	info(L_VERBOSE, "%s: searching for type %d (%s)\n", __func__,
		gen_class, class_name);

	for (i = 1; i <= MAX_NODEID; i++) {
		ret = send_then_recv(devfd, 'F', ">?FI0,%d,0,%d", gen_class, i);
		if (ret <= 0)
			break;

		nodename = nodeid_to_nodename(ret);
		if (nodename)
			info(L_NORMAL, "%03d ('%s'): %s "
				"(generic class %d, instance %d)\n",
				ret, nodename, class_name, gen_class, i);
		else
			info(L_NORMAL, "%03d (unnamed): %s "
				"(generic class %d, instance %d)\n",
				ret, class_name, gen_class, i);
	}
}

static int handle_list(int devfd)
{
	search_by_type(devfd, 16, "switch/appliance");
	search_by_type(devfd, 17, "dimmer");
	search_by_type(devfd, 8, "thermostat");
	search_by_type(devfd, 1, "controller");
	return 0;
}

/*
 * FIRMWARE UPGRADES
 *
 * Note that the Zensys (EEPROM) upgrade uses a completely different protocol
 * and procedure than the ST (flash) upgrade.
 *
 * A failed Zensys upgrade will not brick the unit.  The Zensys code does not
 * even run on the ST microcontroller.
 *
 * A failed flash upgrade might brick the unit (although there is some logic
 * in the bootloader which puts the target in "recovery mode" if either the
 * first or last block looks empty).
 */

static int upgrade_zensys(int devfd, FILE *f)
{
	char buf[BUFLEN];
	int ret = 0;

	info(L_NORMAL, "Zensys upgrade: syncing up with the target...\n");

	sync_interface(devfd);
	write_line(devfd, ">ZB");

	/* ">ZB" generates three responses (and the last one takes a moment) */
	read_resp(devfd, buf, BUFLEN, TIMEOUT);
	if (strncmp(buf, "<E000", 5) != 0)
		die("error: bad response '%s'\n", buf);

	read_resp(devfd, buf, BUFLEN, TIMEOUT);
	if (strncmp(buf, ":7F7F7F7F1F00", 13) != 0)
		die("error: bad response '%s'\n", buf);

	read_resp(devfd, buf, BUFLEN, TIMEOUT_UPGRADE);
	if (strncmp(buf, "<B000", 5) != 0)
		die("error: bad response '%s'\n", buf);

	info(L_NORMAL, "Programming...\n");

	while (fgets(buf, BUFLEN, f) != NULL && buf[0] == ':') {
		char *newline;

		newline = index(buf, '\n');
		if (*newline)
			*newline = 0;

		newline = index(buf, '\r');
		if (*newline)
			*newline = 0;

		info(L_DEBUG, "processing: '%s'\n", buf);
		write_line(devfd, buf);
		read_resp(devfd, buf, BUFLEN, TIMEOUT_UPGRADE);
		if (strncmp(buf, "<E000", 5) != 0) {
			info(L_WARNING, "unexpected response: '%s'\n", buf);
			ret = 1;
		}

		read_resp(devfd, buf, BUFLEN, TIMEOUT_UPGRADE);
		if (strncmp(buf, "<B", 2) == 0)
			continue;

		/* final line */
		if (buf[0] == ':')
			break;

		info(L_WARNING, "unexpected response: '%s'\n", buf);
		ret = 1;
	}

	/* this is a no-op (for now) */
	info(L_NORMAL, "Verifying... (or at least pretending to)\n");

	while (buf[0] != ':')
		read_resp(devfd, buf, BUFLEN, TIMEOUT_UPGRADE);

	do {
		read_resp(devfd, buf, BUFLEN, TIMEOUT_UPGRADE);
	} while (strncmp(buf, "<B000", 5) != 0);

	return ret;
}

static int st_getbytes(int devfd, char *buf, int len)
{
	fd_set s;
	struct timeval tv;
	int ret;

	FD_ZERO(&s);
	FD_SET(devfd, &s);

	while (len) {
		tv.tv_sec = TIMEOUT_UPGRADE / 1000000;
		tv.tv_usec = TIMEOUT_UPGRADE % 1000000;
		if (select(devfd + 1, &s, NULL, NULL, &tv) != 1)
			return -1;

		ret = read(devfd, buf, len);
		if (ret <= 0)
			return -1;
		len -= ret;
	}
	return 0;
}

static void st_cmd(int devfd, const char *out, int outlen, int inlen)
{
	char buf[BUFLEN];

	write(devfd, out, outlen);
	if (inlen && st_getbytes(devfd, buf, inlen) < 0)
		die("error: target quit responding.  "
			"Cycle power and try again.\n");
}

static void st_parsehex(unsigned char *out, char *in)
{
	if (*in >= 'A')
		*out = (0x0a + *in - 'A') << 4;
	else
		*out = (0x00 + *in - '0') << 4;
	in++;

	if (*in >= 'A')
		*out |= (0x0a + *in - 'A');
	else
		*out |= (0x00 + *in - '0');
}

static void st_xor(unsigned char *out, int len)
{
	int i;
	unsigned char res = 0;

	for (i = 0; i < len; i++)
		res ^= out[i];
	out[len] = res;
}

static void st_termsetup(int fd)
{
	struct termios termios;

	if (tcgetattr(fd, &termios) != 0)
		die("can't set termios\n");

	/* 57600bps 8E1 */
	termios.c_iflag = 0;
	termios.c_oflag = 0;
	termios.c_cflag = CS8 | CLOCAL | CREAD | PARENB;
	termios.c_lflag = 0;
	cfsetspeed(&termios, B57600);

	if (tcsetattr(fd, TCSANOW, &termios) != 0)
		die("can't set termios\n");
}

static int upgrade_st(int devfd, FILE *f)
{
	char buf[BUFLEN];
	int i, ret = 0;

	st_termsetup(devfd);

	info(L_NORMAL, "ST upgrade: attempting to sync up with target...\n");
	for (i = 0; ; i++) {
		flush_bytes(devfd);
		write(devfd, "\x7f", 1);
		if (st_getbytes(devfd, buf, 1) == 0 && buf[0] == 0x79)
			break;

		/*
		 * It's easy to confuse the target when it's in recovery
		 * mode.  So try recovery mode first, then fall back to
		 * normal mode if that does not work.
		 */
		if (i == 2) {
			set_tty_defaults(devfd, 9600);
			write_line(devfd, "");
			write_line(devfd, ">CB");
			usleep(20000);
			flush_bytes(devfd);
			st_termsetup(devfd);
			usleep(20000);
		}
		if (i == 4)
			die("error: can't establish communication with "
				"target.  Cycle power and try again.\n");
	}

	info(L_NORMAL, "Erasing...\n");

	st_cmd(devfd, "\x01\xfe", 2, 5);
	st_cmd(devfd, "\x02\xfd", 2, 5);
	st_cmd(devfd, "\x43\xbc", 2, 1);
	st_cmd(devfd, "\x3e\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b"
		"\x0c\x0d\x0e\x0f\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19"
		"\x1a\x1b\x1c\x1d\x1e\x1f\x20\x21\x22\x23\x24\x25\x26\x27"
		"\x28\x29\x2a\x2b\x2c\x2d\x2e\x2f\x30\x31\x32\x33\x34\x35"
		"\x36\x37\x38\x39\x3a\x3b\x3c\x3d\x3e\x3f\x3e", 65, 1);

	info(L_NORMAL, "Programming...\n");
	while (fgets(buf, BUFLEN, f) != NULL && buf[0] == ':') {
		unsigned char binbuf[18];
		int len, j;
		char *newline;

		newline = index(buf, '\n');
		if (*newline)
			*newline = 0;

		newline = index(buf, '\r');
		if (*newline)
			*newline = 0;

		info(L_DEBUG, "processing: '%s'\n", buf);
		if (buf[7] != '0' || buf[8] != '0')
			continue;

		st_cmd(devfd, "\x31\xce", 2, 1);

		/* set address */
		memcpy(binbuf, "\x08\x00\x00\x00", 5);
		st_parsehex(&binbuf[2], &buf[3]);
		st_parsehex(&binbuf[3], &buf[5]);
		st_xor(binbuf, 4);
		st_cmd(devfd, (char *)binbuf, 5, 1);

		len = strlen(buf);
		/* each line needs to have 1-16 data bytes */
		if (len < 13 || len > 43) {
			info(L_WARNING, "warning: malformed line '%s'\n",
				buf);
			ret = 1;
			continue;
		}
		len = (len - 11) >> 1;

		binbuf[0] = len - 1;
		for (j = 0; j < len; j++)
			st_parsehex(&binbuf[j + 1], &buf[9 + j * 2]);
		st_xor(binbuf, len + 1);
		st_cmd(devfd, (char *)binbuf, len + 2, 1);
	}
	info(L_NORMAL, "\n");

	st_cmd(devfd, "\x21\xde", 2, 1);
	st_cmd(devfd, "\x08\x00\x00\x00\x08", 5, 0);

	return ret;
}

static int handle_upgrade(int devfd, char *firmware)
{
	FILE *f;
	char buf[BUFLEN];
	int ret;

	f = fopen(firmware, "r");
	if (f == NULL)
		die("error: can't open '%s'\n", firmware);

	if (fgets(buf, BUFLEN, f) == NULL || buf[0] != ':')
		die("error: bad firmware image\n");
	fseek(f, 0, SEEK_SET);
	
	if (buf[7] == '0' && buf[8] == '0')
		ret = upgrade_zensys(devfd, f);
	else
		ret = upgrade_st(devfd, f);
	fclose(f);

	if (ret == 0)
		info(L_NORMAL, "Operation was successful.  "
			"Please reboot the VRC0P.\n");
	else
		info(L_WARNING, "Operation completed with warnings.\n");

	return ret;
}

/*
 * UI
 */

static int run_command(int devfd, char *nodename, struct vrctl_cmd *entry,
	char *arg)
{
	int id, ret;
	struct node_alias *a;

	/* "all" keyword */
	if (strcasecmp(nodename, "all") == 0) {
		if (entry->is_unicast)
			die("error: this command cannot operate on ALL nodes at once\n");
		return entry->handler(devfd, NODEID_ALL, arg);
	}

	/* single or multiple alias match */
	a = lookup_next_alias(nodename, NULL);
	if (a) {
		while (1) {
			/* note: return status only reflects the LAST command */
			ret = entry->handler(devfd, a->nodeid, arg);

			a = lookup_next_alias(nodename, a);
			if (a == NULL)
				return ret;
		}
	}

	/* fall back to parsing it as an integer */
	id = parse_uint(nodename, 0, "node ID", MAX_NODEID);
	return entry->handler(devfd, id, arg);
}

static const struct option longopts[] = {
	{ "verbose",	no_argument,		NULL, 'v' },
	{ "quiet",	no_argument,		NULL, 'q' },
	{ "port",	required_argument,	NULL, 'x' },
	{ "list",	no_argument,		NULL, 'l' },
	{ "upgrade",	required_argument,	NULL, 'u' },
	{ "help",	no_argument,		NULL, 'h' },
	{ NULL,		0,			NULL,  0  },
};
static const char optstring[] = "vqx:lu:h";

static void usage(void)
{
	printf("vrctl v" VERSION " - Z-Wave VRC0P utility\n");
	printf("Copyright 2012 Kevin Cernekee.  License: GPLv2+\n");
	printf("This is free software with ABSOLUTELY NO WARRANTY.\n");
	printf("\n");
	printf("Usage:\n");
	printf("  vrctl [<options>] <nodeid> <command> [ <nodeid> <command> ... ]\n");
	printf("  vrctl [<options>] all { on | off }\n");
	printf("  vrctl [<options>] --list\n");
	printf("\n");
	printf("Options:\n");
	printf("  -v, --verbose       add v's to increase verbosity\n");
	printf("  -q, --quiet         only display errors\n");
	printf("  -x, --port=PORT     set port to use (default: " DEFAULT_DEV ")\n");
	printf("  -l, --list          list all devices in the network\n");
	printf("  -u, --upgrade=FILE  upgrade firmware from FILE\n");
	printf("  -h, --help          this help\n");
	printf("\n");
	printf("<nodeid> is one of the following:\n");
	printf("  a decimal node number: 3 (use vrctl -l to list them)\n");
	printf("  an alias from $HOME/.vrctlrc\n");
	printf("\n");
	printf("<command> is one of the following (case-insensitive):\n");
	printf("  on                  turn the device on\n");
	printf("  off                 turn the device off\n");
	printf("  bounce              turn the device off, then on again\n");
	printf("  toggle              invert the device's on/off state\n");
	printf("  level <n>           set brightness level\n");
	printf("  status              display the current on/off/dimmer status\n");
	printf("  scene <n>           activate a previously stored scene\n");
	printf("\n");
	printf("For door locks only:\n");
	printf("  lock                lock\n");
	printf("  unlock              unlock\n");
	printf("\n");
	printf("For thermostats only:\n");
	printf("  temp                read current temperature\n");
	printf("  setpoint            read current setpoint\n");
	printf("  fan <n>             set fan to on (1) or auto (0)\n");
	printf("  heat <n>            enable heater with setpoint N\n");
	printf("  cool <n>            enable A/C with setpoint N\n");
	printf("  heat 0              disable heater and A/C\n");
	exit(1);
}

static struct vrctl_cmd cmd_table[] = {
	{ "on",		0,	0,	handle_on },
	{ "off",	0,	0,	handle_off },
	{ "bounce",	0,	0,	handle_bounce },
	{ "toggle",	0,	1,	handle_toggle },
	{ "level",	1,	0,	handle_level },
	{ "status",	0,	1,	handle_status },
	{ "lock",	0,	1,	handle_lock },
	{ "unlock",	0,	1,	handle_unlock },
	{ "scene",	1,	0,	handle_scene },
	{ "temp",	0,	1,	handle_temp },
	{ "setpoint",	0,	1,	handle_setpoint },
	{ "fan",	1,	1,	handle_fan },
	{ "heat",	1,	1,	handle_heat },
	{ "cool",	1,	1,	handle_cool },
};

int main(int argc, char **argv)
{
	int opt, do_list = 0, synced = 0, no_cmdlist = 0, ret = 0;
	char *dev = DEFAULT_DEV, *firmware = NULL;
	int devfd;

	read_rcfile();
	if (rc_port != NULL)
		dev = rc_port;

	while ((opt = getopt_long(argc, argv,
			optstring, longopts, NULL)) != -1) {
		switch (opt) {
		case 'v':
			g_loglevel++;
			break;
		case 'q':
			g_loglevel = L_WARNING;
			break;
		case 'x':
			dev = optarg;
			break;
		case 'l':
			do_list = 1;
			no_cmdlist = 1;
			break;
		case 'u':
			firmware = optarg;
			no_cmdlist = 1;
			break;
		case 'h':
		default:
			usage();
		}

	}

	if (no_cmdlist ^ !!(optind >= argc))
		usage();

	if (lock_tty(dev, "vrctl") < 0)
		die("error: %s is locked\n", dev);
	g_locked_tty = dev;

	devfd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (devfd < 0)
		die("error: can't open %s: %s\n", dev, strerror(errno));
	if (fcntl(devfd, F_SETFL, 0) < 0)
		die("error: can't clear NONBLOCK flag: %s\n", strerror(errno));
	if (set_tty_defaults(devfd, 9600) < 0)
		die("error: can't set termios on %s: %s\n",
			dev, strerror(errno));
	
	if (firmware) {
		ret = handle_upgrade(devfd, firmware);
		goto out;
	}

	if (do_list) {
		ret = handle_list(devfd);
		goto out;
	}

	while (optind < argc) {
		int i;
		struct vrctl_cmd *entry = NULL;
		char *nodename, *command, *arg = NULL;

		nodename = argv[optind++];

		/* parse the command first */

		if (optind >= argc)
			die("error: command for node '%s' was not specified\n",
				nodename);
		command = argv[optind++];

		for (i = 0; i < ARRAY_SIZE(cmd_table); i++) {
			if (strcasecmp(cmd_table[i].name, command) == 0)
				entry = &cmd_table[i];
		}

		if (!entry)
			die("error: bad command '%s'\n", command);

		if (entry->arg_required) {
			if (optind >= argc)
				die("error: %s requires an argument\n",
					command);
			arg = argv[optind++];
		}

		if (!synced) {
			sync_interface(devfd);
			synced = 1;
		}

		/* parse the nodeid(s) and execute the command */
		run_command(devfd, nodename, entry, arg);
	}

	update_nodes(devfd);

out:
	unlock_tty(dev);
	return ret;
}
