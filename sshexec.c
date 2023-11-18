/* See LICENSE file for copyright and license details. */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


static const char *argv0 = "sshexec";


#define exitf(...) (fprintf(stderr, __VA_ARGS__), exit(255))


static void
usage(void)
{
	exitf("usage: %s [{ %s }] [ssh-option] ... destination command [argument] ...\n",
	      argv0, "[ssh=command] [dir=directory]");
}

static const enum optclass {
	NO_RECOGNISED = 0,
	NO_ARGUMENT,
	MANDATORY_ARGUMENT
} sshopts[(size_t)1 << CHAR_BIT] = {
#define X(F) [F] = NO_ARGUMENT
	X('4'), X('6'), X('A'), X('a'), X('C'), X('f'), X('G'),
	X('g'), X('K'), X('k'), X('M'), X('N'), X('n'), X('q'),
	X('s'), X('T'), X('t'), X('V'), X('v'), X('X'), X('x'),
	X('Y'), X('y'),
#undef X
#define X(F) [F] = MANDATORY_ARGUMENT
	X('B'), X('b'), X('c'), X('D'), X('E'), X('e'), X('F'),
	X('I'), X('i'), X('J'), X('L'), X('l'), X('m'), X('O'),
	X('o'), X('P'), X('p'), X('Q'), X('R'), X('S'), X('W'),
	X('w')
#undef X
};

static char *command = NULL;
static size_t command_size = 0;
static size_t command_len = 0;


#define build_command(DATA, N)\
	do {\
		build_command_reserve(N);\
		memcpy(&command[command_len], (DATA), (N));\
		command_len += (N);\
	} while (0)

#define build_command_asis(S)\
	build_command(S, sizeof(S) - 1)

#define finalise_command()\
	build_command("", 1)


static void
build_command_reserve(size_t n)
{
	if (n > command_size - command_len) {
		if (n < 512)
			n = 512;
		if (command_len + n > SIZE_MAX)
			exitf("%s: could not allocate enough memory\n", argv0);
		command_size = command_len + n;
		command = realloc(command, command_size);
		if (!command)
			exitf("%s: could not allocate enough memory\n", argv0);
	}
}


static void
build_command_escape(const char *arg)
{
	size_t n = 0;
	if (!*arg) {
		build_command_asis("''");
		return;
	}
	while (isalnum(arg[n]) || arg[n] == '_' || arg[n] == '/')
		n += 1;
	if (!arg[n]) {
		build_command(arg, n);
		return;
	}
	build_command_asis("\"$(printf '");
	goto start;
	while (*arg) {
		build_command_reserve(4);
		command[command_len++] = '\\';
		command[command_len] = (((unsigned char)*arg >> 6) & 7) + '0';
		command_len += (command[command_len] != '0');
		command[command_len] = (((unsigned char)*arg >> 3) & 7) + '0';
		command_len += (command[command_len] != '0');
		command[command_len++] = ((unsigned char)*arg & 7) + '0';
		arg = &arg[1];

		n = 0;
		while (isalpha(arg[n]) || arg[n] == '_' || arg[n] == '/')
			n += 1;
		if (n) {
			while (isalnum(arg[n]) || arg[n] == '_' || arg[n] == '/')
				n += 1;
		start:
			build_command(arg, n);
			arg = &arg[n];
		}
	}
	build_command_asis("\\n')\"");
}


int
main(int argc_unused, char *argv[])
{
	const char *dir = NULL;
	const char *ssh = NULL;
	const char *destination;
	char **opts;
	size_t nopts;
	enum optclass class;
	const char *arg;
	char opt;
	const char **args;
	size_t i;

	(void) argc_unused;

	if (*argv)
		argv0 = *argv++;

#define STORE_OPT(VARP, OPT)\
	if (!strncmp(*argv, OPT"=", sizeof(OPT))) {\
		if (*(VARP) || !*(*(VARP) = &(*argv)[sizeof(OPT)]))\
			usage();\
		continue;\
	}
	if (*argv && !strcmp(*argv, "{")) {
		argv++;
		for (; *argv && strcmp(*argv, "}"); argv++) {
			STORE_OPT(&ssh, "ssh")
			STORE_OPT(&dir, "dir")
			usage();
		}
		if (!*argv)
			usage();
		argv++;
	}
#undef STORE_OPT

	if (!ssh)
		ssh = "ssh";

	opts = argv;
	nopts = 0;
	while (*argv) {
		if (!strcmp(*argv, "--")) {
			argv++;
			break;
		} else if ((*argv)[0] != '-' || !(*argv)[1]) {
			break;
		}
		arg = &(*argv++)[1];
		nopts++;
		while (*arg) {
			opt = *arg++;
			class = sshopts[(unsigned char)opt];
			if (class == MANDATORY_ARGUMENT) {
				if (*arg) {
					break;
				} else if (*argv) {
					argv++;
					nopts++;
					break;
				} else {
					exitf("%s: argument missing for option -%c\n", argv0, opt);
				}
			} else if (class == NO_RECOGNISED) {
				exitf("%s: unrecognised option -%c\n", argv0, opt);
			}
		}
	}

	destination = *argv++;
	if (!destination && !*argv)
		usage();

	if (dir) {
		build_command_asis("cd -- ");
		build_command_escape(dir);
		build_command_asis(" && ");
	}
	build_command_asis("exec --");
	for (; *argv; argv++) {
		build_command_asis(" ");
		build_command_escape(*argv);
	}
	finalise_command();

	i = 0;
	args = calloc(5 + nopts, sizeof(*args));
	if (!args)
		exitf("%s: could not allocate enough memory\n", argv0);
	args[i++] = ssh;
	memcpy(&args[i], opts, nopts * sizeof(*opts));
	i += nopts;
	args[i++] = "--";
	args[i++] = destination;
	args[i++] = command;
	args[i++] = NULL;

#if defined(__GNUC__)
# pragma GCC diagnostic ignored "-Wcast-qual"
#endif
	execvp(ssh, (char **)args);
	exitf("%s: failed to execute %s: %s\n", argv0, ssh, strerror(errno));
}
