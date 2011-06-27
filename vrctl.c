/*
 * vrctl - Z-Wave VRC0P utility
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

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
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

#define BUFLEN			64
#define DEFAULT_DEV		"/dev/vrc0p"
#define RC_NAME			".vrctlrc"
#define TIMEOUT			500000
#define NODEID_ALL		-2
#define MAX_NODEID		232

#define __func__		__FUNCTION__

struct resp {
	char			type0;
	unsigned int		arg0;
	char			type1;
	unsigned int		arg1;
};

struct node_alias {
	int			nodeid;
	char			nodename[BUFLEN];
	struct node_alias	*next;
};

static struct node_alias *alias_head = NULL, *alias_tail = NULL;
static char *rc_port = NULL;

/*
 * RC FILE
 */

static int lookup_alias(char *nodename)
{
	struct node_alias *a;

	/* check the alias list - first case-insensitive match wins */
	for (a = alias_head; a != NULL; a = a->next)
		if (strcasecmp(a->nodename, nodename) == 0) {
			info(L_DEBUG, "%s: '%s' => node %lu\n", __func__,
				nodename, a->nodeid);
			return a->nodeid;
		}
	return -1;
}

static void parse_rcline(char *filename, int linenum, char *line)
{
	char *p = line, tok[BUFLEN];

	if (next_token(&p, tok, BUFLEN) < 0)
		return;		/* empty line */

	if (strcmp(tok, "#") == 0)
		return;		/* comment */

	if (strcasecmp(tok, "alias") == 0) {
		struct node_alias *a;
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

		nodeid = lookup_alias(tok);
		if (nodeid == -1) {
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

static int read_resp(int devfd, char *buf, int maxlen)
{
	int ret;
	ret = read_line(devfd, buf, maxlen, TIMEOUT);

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

static int parse_resp(char *buf, struct resp *r)
{
	/*
	 * Some valid inputs look like:
	 * >E001
	 * >X000
	 * >N003L000
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

	r->type1 = buf[5];
	r->arg1 = parse_uint(&buf[6], 3, "arg1", 0);

	return 0;
}

static void wait_resp(int devfd, char expected_type, struct resp *r)
{
	char buf[BUFLEN];

	do {
		read_resp(devfd, buf, BUFLEN);
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
	/* possible workaround for VRC0P firmware hangs */
	usleep(700000);

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

	if (nodeid == NODEID_ALL)
		die("error: can't check status of ALL nodes at once\n");

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

	if (nodeid == NODEID_ALL)
		die("error: can't lock/unlock ALL nodes at once\n");

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


/*
 * UI
 */

static int lookup_node(char *nodename)
{
	int id;

	/* "all" keyword */
	if (strcasecmp(nodename, "all") == 0)
		return NODEID_ALL;

	id = lookup_alias(nodename);
	if (id > 0)
		return id;

	/* fall back to parsing it as an integer */
	return parse_uint(nodename, 0, "node ID", MAX_NODEID);
}

static const struct option longopts[] = {
	{ "verbose",	no_argument,		NULL, 'v' },
	{ "quiet",	no_argument,		NULL, 'q' },
	{ "port",	required_argument,	NULL, 'x' },
	{ "list",	no_argument,		NULL, 'l' },
	{ "help",	no_argument,		NULL, 'h' },
};
static const char optstring[] = "vqx:lh";

static void usage(void)
{
	printf("vrctl v0.01 - Z-Wave VRC0P utility\n");
	printf("Copyright 2011 Kevin Cernekee.  License: GPLv2+\n");
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
	printf("  -h, --help          this help\n");
	printf("\n");
	printf("<nodeid> is one of the following:\n");
	printf("  a decimal node number: 3 (use vrctl -l to list them)\n");
	printf("  an alias from $HOME/.vrctlrc\n");
	printf("  g<n>, where <n> is a previously stored group number: g7 or G7\n");
	printf("\n");
	printf("<command> is one of the following (case-insensitive):\n");
	printf("  on                  turn the device on\n");
	printf("  off                 turn the device off\n");
	printf("  bounce              turn the device off, then on again\n");
	printf("  toggle              invert the device's on/off state\n");
	printf("  level <n>           set brightness level\n");
	printf("  status              display the current on/off/dimmer status\n");
	printf("  lock                lock (door locks only)\n");
	printf("  unlock              unlock (door locks only)\n");
	printf("  scene <n>           Activate a previously stored scene\n");
	exit(1);
}

struct vrctl_cmd {
	char			*name;
	int			arg_required;
	int			(*handler)(int devfd, int nodeid, char *arg);
};

static struct vrctl_cmd cmd_table[] = {
	{ "on",		0,	handle_on },
	{ "off",	0,	handle_off },
	{ "bounce",	0,	handle_bounce },
	{ "toggle",	0,	handle_toggle },
	{ "level",	1,	handle_level },
	{ "status",	0,	handle_status },
	{ "lock",	0,	handle_lock },
	{ "unlock",	0,	handle_unlock },
	{ "scene",	1,	handle_scene },
};

int main(int argc, char **argv)
{
	int opt, do_list = 0, nodeid, synced = 0;
	char *dev = DEFAULT_DEV;
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
			break;
		case 'h':
			usage();
		}

	}

	if (lock_tty(dev, "vrctl") < 0)
		die("error: %s is locked\n", dev);
	g_locked_tty = dev;

	devfd = open(dev, O_RDWR | O_NOCTTY);
	if (devfd < 0)
		die("error: can't open %s: %s\n", dev, strerror(errno));
	if (set_tty_defaults(devfd, 9600) < 0)
		die("error: can't set termios on %s: %s\n",
			dev, strerror(errno));

	while (optind < argc) {
		int i;
		struct vrctl_cmd *entry = NULL;
		char *nodename, *command, *arg = NULL;

		nodename = argv[optind++];
		nodeid = lookup_node(nodename);

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
		entry->handler(devfd, nodeid, arg);
	}

	update_nodes(devfd);
	unlock_tty(dev);
	return 0;
}
