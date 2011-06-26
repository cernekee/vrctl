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
#define NODEID_ALL		-1

#define __func__		__FUNCTION__

struct resp {
	char			type0;
	unsigned int		arg0;
	char			type1;
	unsigned int		arg1;
};

/*
 * COMMANDS
 */

static int read_resp(int fd, char *buf, int maxlen)
{
	int ret;
	ret = read_line(fd, buf, maxlen, TIMEOUT);

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

static void wait_resp(int fd, char expected_type, struct resp *r)
{
	char buf[BUFLEN];

	do {
		read_resp(fd, buf, BUFLEN);
		if (parse_resp(buf, r) < 0)
			die("error: received bad response '%s'\n", buf);

		if (r->type0 == 'E' && r->arg0 != 0)
			die("error: received E%03d while waiting for "
				"'%c' response\n", r->arg0, expected_type);
	} while (r->type0 != expected_type);
}

static int send_then_recv(int fd, char expected_type, char *fmt, ...)
{
	va_list ap;
	char buf[BUFLEN];
	struct resp r;

	va_start(ap, fmt);
	vsnprintf(buf, BUFLEN, fmt, ap);
	va_end(ap);

	write_line(fd, buf);
	wait_resp(fd, expected_type, &r);
	return r.arg0;
}

static void sync_interface(int fd)
{
	char buf[BUFLEN];
	int i, ret;

	usleep(25000);
	flush_bytes(fd);
	/* hit "enter" on the serial line until we get <E000 back */
	for (i = 0; i < 3; i++) {
		write_line(fd, "");

		ret = read_line(fd, buf, BUFLEN, TIMEOUT);

		if (ret > 0 && strcmp(buf, "<E000") == 0)
			return;
		sleep(1);
	}
	die("error: can't establish communication with VRC0P interface\n");
}

/*
 * COMMAND HANDLERS
 *
 * Unless otherwise specified (and there ARE a few exceptions), the return
 * value will be:
 *
 *  0 - success
 *  anything else - Xnnn error code from the VRC0P (0-255)
 */

static int handle_unimplemented(int devfd, int nodeid, char *arg)
{
	info(L_ERROR, "command is unimplemented\n");
	return -1;
}

static int handle_on(int devfd, int nodeid, char *arg)
{
	int ret;

	if (nodeid == NODEID_ALL)
		ret = send_then_recv(devfd, 'X', ">N,ON");
	else
		ret = send_then_recv(devfd, 'X', ">N%03dON", nodeid);
	if (ret != 0)
		info(L_ERROR, "node %d returned X%03x for ON command\n",
			nodeid, ret);
	return ret;
}

static int handle_off(int devfd, int nodeid, char *arg)
{
	int ret;

	if (nodeid == NODEID_ALL)
		ret = send_then_recv(devfd, 'X', ">N,OF");
	else
		ret = send_then_recv(devfd, 'X', ">N%03dOF", nodeid);
	if (ret != 0)
		info(L_ERROR, "node %d returned X%03x for OFF command\n",
			nodeid, ret);
	return ret;
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

/*
 * handle_status_quiet - query the remote node's status (on/off/dimlevel)
 *
 * Return value:
 *  <0 - error (Xnnn code)
 *   0 - node is OFF
 *  >0 - dim level (255 for relay ON)
 */
static int handle_status_quiet(int devfd, int nodeid, char *arg)
{
	int ret;
	struct resp r;

	if (nodeid == NODEID_ALL)
		die("error: can't check status of ALL nodes at once\n");

	ret = send_then_recv(devfd, 'X', ">?N%03d", nodeid);
	if (ret != 0) {
		info(L_ERROR, "node %d returned X%03x for STATUS command\n",
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
		info(L_ERROR, "node %d returned X%03x for LEVEL command\n",
			nodeid, ret);
	return ret;
}

static int handle_lock_level(int devfd, int nodeid, int level)
{
	int ret;
	struct resp r;

	if (nodeid == NODEID_ALL)
		die("error: can't lock/unlock ALL nodes at once\n");

	ret = send_then_recv(devfd, 'X', ">N%03dSS98,1,%d", nodeid, level);
	if (ret != 0)
		info(L_ERROR, "node %d returned X%03x for LOCK/UNLOCK "
			"command\n", nodeid, ret);
	return ret;
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
	int scene = parse_uint(arg, 0, "scene number", 232);

	if (nodeid == NODEID_ALL)
		ret = send_then_recv(devfd, 'X', ">N,S%d", scene);
	else
		ret = send_then_recv(devfd, 'X', ">N%03dS%d", nodeid, scene);
	if (ret != 0)
		info(L_ERROR, "node %d returned X%03x for SCENE command\n",
			nodeid, ret);
	return ret;
}


/*
 * UI
 */

static int lookup_node(char *nodename)
{
	unsigned long id;
	char *endptr;

	if (strcasecmp(nodename, "all") == 0)
		return NODEID_ALL;
	id = parse_uint(nodename, 0, "node ID", 232);

	info(L_DEBUG, "%s: '%s' => node %lu\n", __func__, nodename, id);
	return (int)id;
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
	printf("  -x, --port=PORT     set port to use (default: /dev/vrc0p)\n");
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

	while ((opt = getopt_long(argc, argv,
			optstring, longopts, NULL)) != -1) {
		switch (opt) {
		case 'v':
			g_loglevel++;
			break;
		case 'q':
			g_loglevel = L_ERROR;
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

	unlock_tty(dev);
	return 0;
}