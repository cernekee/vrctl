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
#include <sys/select.h>
#include <sys/fcntl.h>
#include <sys/types.h>
#include "util.h"

#define BUFLEN			64
#define DEFAULT_DEV		"/dev/vrc0p"
#define RC_NAME			".vrctlrc"
#define TIMEOUT			500000

#define __func__		__FUNCTION__

struct resp {
	char			code0;
	unsigned char		arg0;
	char			code1;
	unsigned char		arg1;
};

/*
 * COMMANDS
 */

static int send_cmd(int fd, char *cmd);
static int read_resp(int fd, char *buf, int maxlen);
static int parse_resp(char *buf, struct resp *r);
static int wait_resp(int fd, char expected_code);
static int send_then_recv(int fd, char *buf, char expected_code);

static void sync_interface(int fd)
{
	char buf[BUFLEN];
	int i, ret;

	flush_bytes(fd);
	/* hit "enter" on the serial line until we get <E000 back */
	for (i = 0; i < 3; i++) {
		write_line(fd, "\r\n");
		ret = read_line(fd, buf, BUFLEN, TIMEOUT);
		if (ret > 0 && strcmp(buf, "<E000") == 0)
			return;
		sleep(1);
	}
	die("error: can't establish communication with VRC0P interface\n");
}

/*
 * COMMAND HANDLERS
 */

static int handle_unimplemented(int devfd, int nodeid, char *arg)
{
	info(L_ERROR, "command is unimplemented\n");
	return -1;
}

static int handle_on(int devfd, int nodeid, char *arg)
{
	return 0;
}

/*
 * UI
 */

static int lookup_node(char *nodename)
{
	unsigned long id;
	char *endptr;

	id = strtoul(nodename, &endptr, 10);
	if (*nodename == 0 || *endptr != 0)
		die("invalid node '%s'\n", nodename);

	info(L_DEBUG, "%s: '%s' => node %d\n", __func__, nodename, id);
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
	{ "off",	0,	handle_unimplemented },
	{ "bounce",	0,	handle_unimplemented },
	{ "toggle",	0,	handle_unimplemented },
	{ "level",	1,	handle_unimplemented },
	{ "status",	0,	handle_unimplemented },
	{ "lock",	0,	handle_unimplemented },
	{ "unlock",	0,	handle_unimplemented },
	{ "scene",	1,	handle_unimplemented },
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
