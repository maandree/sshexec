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
	      argv0, "[ssh=command] [dir=directory] [cd=(strict|lax)] [[fd]{>,>>,>|,<>}[&]=file] "
	             "[asis=asis-marker [nasis=asis-count]]");
}


struct redirection {
	const char *asis;
	const char *escape;
};


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
	build_command(S, strlen(S))

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
	const char *cd = NULL;
	const char *asis = NULL;
	const char *nasis_str = NULL;
	struct redirection *redirections = NULL;
	size_t nredirections = 0;
	char *destination;
	char *dest_dir_delim;
	char **opts, *p;
	size_t nopts;
	enum optclass class;
	const char *arg;
	char opt;
	const char **args;
	size_t i;
	int strict_cd;
	unsigned long long int nasis = ULLONG_MAX;
	int next_asis;

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
			STORE_OPT(&asis, "asis")
			STORE_OPT(&nasis_str, "nasis")

			p = *argv;
			while (isdigit(*p))
				p++;
			if (p[0] == '>')
				p = &p[1 + (p[1] == '>' || p[1] == '|')];
			else if (p[0] == '<')
				p = &p[1 + (p[1] == '>')];
			else
				usage();
			if (p[p[0] == '&'] != '=')
				usage(); 
			redirections = realloc(redirections, (nredirections + 1U) * sizeof(*redirections));
			if (!redirections)
				exitf("%s: could not allocate enough memory\n", argv0);
			if (*p == '&') {
				p = &p[1];
				memmove(p, &p[1], strlen(&p[1]) + 1U);
				if (isdigit(*p)) {
					p++;
					while (isdigit(*p))
						p++;
				} else if (*p == '-') {
					p++;
				}
				if (*p)
					usage();
				redirections[nredirections].escape = NULL;
			} else {
				*p++ = '\0';
				redirections[nredirections].escape = p;
			}
			redirections[nredirections++].asis = *argv;
		}
		if (!*argv)
			usage();
		argv++;
	}

#undef STORE_OPT

	if (!ssh)
		ssh = "ssh";

	if (!cd || !strcmp(cd, "strict"))
		strict_cd = 1;
	else if (!strcmp(cd, "lax"))
		strict_cd = 0;
	else
		usage();

	if (nasis_str) {
		char *end;
		if (!asis || !isdigit(*nasis_str))
			usage();
		errno = 0;
		nasis = strtoull(nasis_str, &end, 10);
		if ((!nasis && errno) || *end)
			usage();
	}

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
	if (!destination || !*argv)
		usage();

	if (!strcmp(*argv, "-"))
		exitf("%s: the command argument must not be \"-\"\n", argv0);
	else if (strchr(*argv, '='))
		exitf("%s: the command argument must contain an \'=\'\n", argv0);

	if (!strncmp(destination, "sshexec://", sizeof("sshexec://") - 1U)) {
		memmove(&destination[sizeof("ssh") - 1U],
		        &destination[sizeof("sshexec") - 1U],
		        strlen(&destination[sizeof("sshexec") - 1U]) + 1U);
		goto using_ssh_prefix;
	} else if (!strncmp(destination, "ssh://", sizeof("ssh://") - 1U)) {
	using_ssh_prefix:
		dest_dir_delim = strchr(&destination[sizeof("ssh://")], '/');
	} else {
		dest_dir_delim = strchr(destination, ':');
	}
	if (dest_dir_delim) {
		if (dir)
			exitf("%s: directory specified both in 'dir' option and in destination operand\n", argv0);
		*dest_dir_delim++ = '\0';
		dir = dest_dir_delim;
	}
dest_dir_checked:

	if (dir) {
		build_command_asis("cd -- ");
		build_command_escape(dir);
		build_command_asis(strict_cd ? " && " : " ; ");
	}
	if (asis)
		build_command_asis("( env --");
	else
		build_command_asis("exec env --");
	next_asis = 0;
	for (; *argv; argv++) {
		if (asis && nasis && !strcmp(*argv, asis)) {
			nasis -= 1U;
			next_asis = 1;
		} else {
			build_command_asis(" ");
			if (next_asis) {
				next_asis = 0;
				build_command_asis(*argv);
			} else {
				build_command_escape(*argv);
			}
		}
	}
	if (asis)
		build_command_asis(" )");
	for (i = 0; i < nredirections; i++) {
		build_command_asis(" ");
		build_command_asis(redirections[i].asis);
		if (redirections[i].escape) {
			build_command_asis(" ");
			build_command_escape(redirections[i].escape);
		}
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
