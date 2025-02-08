/* See LICENSE file for copyright and license details. */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


/**
 * The name of the process
 */
static const char *argv0 = "sshexec";

/**
 * Whether the process is sshcd(1),
 * otherwise it's sshexec(1)
 */
static int is_sshcd = 0;


/**
 * Print an error message and exit
 *
 * @param  ...  Message format string and arguments
 */
#define exitf(...) (fprintf(stderr, __VA_ARGS__), exit(255))


/**
 * Print usage synopsis and exit
 */
static void
usage(void)
{
	exitf("usage: %s [{ [ssh=command]%s [cd=(strict|lax)]%s }] [ssh-option] ... destination%s\n",
	      argv0,
	      is_sshcd ? "" : " [dir=directory]",
	      is_sshcd ? "" : " [[fd]{>,>>,>|,<>}[&]=file] [asis=asis-marker [nasis=asis-count]]",
	      is_sshcd ? "" : " command [argument] ...");
}


/**
 * File descriptor redirection
 */
struct redirection {
	/**
	 * First part of the redirection string, shall not be escaped
	 */
	const char *asis;

	/**
	 * Second part of the redirection string, shall be escaped;
	 * or `NULL` if the entire string is contained in `.asis`
	 */
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


/**
 * Command being constructor for ssh(1)
 */
static char *command = NULL;

/**
 * The number of bytes allocated to `command`
 */
static size_t command_size = 0;

/**
 * The number of bytes written to `command`
 */
static size_t command_len = 0;


/**
 * The directory to use as the remote working directory
 */
static const char *dir = NULL;

/**
 * The ssh command to use
 */
static const char *ssh = NULL;

/**
 * Whether the command shall fail on failure to
 * set remote working directory
 */
static int strict_cd;

/**
 * The string used to mark the next argument
 * for unescaped inclusion in the command string
 */
static const char *asis = NULL;

/**
 * The number of times `asis` may be recognised
 */
static unsigned long long int nasis = ULLONG_MAX;

/**
 * File redirections to apply after directory switch
 */
static struct redirection *redirections = NULL;

/**
 * The number of elements in `redirections`
 */
static size_t nredirections = 0;

/**
 * The destination command line operand
 */
static char *destination;


/**
 * Add text to `command`
 *
 * @param  TEXT:const char *  The text to add to `command`
 * @param  N:size_t           The number of bytes to write
 */
#define build_command(TEXT, N)\
	do {\
		build_command_reserve(N);\
		memcpy(&command[command_len], (TEXT), (N));\
		command_len += (N);\
	} while (0)

/**
 * Add text to `command`
 *
 * @param  TEXT:const char *  The text to add to `command`
 */
#define build_command_asis(S)\
	build_command(S, strlen(S))

/**
 * NUL-byte terminate `command`
 */
#define finalise_command()\
	build_command("", 1)


/**
 * Allocate space for `command`
 * 
 * @param  n  The number of additional bytes (from what has currently
 *            been written, rather than currently allocated) `command`
 *            should have room for
 */
static void
build_command_reserve(size_t n)
{
	if (n <= command_size - command_len)
		return;

	if (n < 512)
		n = 512;
	if (command_len + n > SIZE_MAX)
		exitf("%s: could not allocate enough memory\n", argv0);
	command_size = command_len + n;
	command = realloc(command, command_size);
	if (!command)
		exitf("%s: could not allocate enough memory\n", argv0);
}


/**
 * Escape a string and add it to `command`
 *
 * @param  arg  The string to escape and add
 */
static void
build_command_escape(const char *arg)
{
#define IS_ALWAYS_SAFE(C)   (isalnum((C)) || (C) == '_' || (C) == '/')
#define IS_INITIAL_SAFE(C)  (isalpha((C)) || (C) == '_' || (C) == '/')

	size_t n = 0;
	size_t lfs = 0;

	/* Quote empty string */
	if (!*arg) {
		build_command_asis("''");
		return;
	}

	/* If the string only contains safe characters, add it would escaping */
	while (IS_ALWAYS_SAFE(arg[n]))
		n += 1;
	if (!arg[n]) {
		build_command(arg, n);
		return;
	}

	/* Escape string, using quoted printf(1) statement */
	build_command_asis("\"$(printf '");
	goto start; /* already have a count of safe initial characters, let's add them immidately */
	while (*arg) {
		/* Since process substation removes at least one terminal
		 * LF, we must hold of on adding them */
		if (*arg == '\n') {
			lfs += 1;
			arg = &arg[1];
			continue;
		}

		/* Adding any held back LF */
		if (lfs) {
			build_command_reserve(lfs * 2U);
			for (; lfs; lfs--) {
				command[command_len++] = '\\';
				command[command_len++] = 'n';
			}
		}

		/* Character is unsafe, escape it */
		if (!IS_ALWAYS_SAFE(*arg)) {
			build_command_reserve(4);
			command[command_len++] = '\\';
			command[command_len] = (((unsigned char)*arg >> 6) & 7) + '0';
			command_len += (command[command_len] != '0');
			command[command_len] = (((unsigned char)*arg >> 3) & 7) + '0';
			command_len += (command[command_len] != '0');
			command[command_len++] = ((unsigned char)*arg & 7) + '0';
			arg = &arg[1];
		}

		/* Add any safe characters as is */
		n = 0;
		while (IS_INITIAL_SAFE(arg[n]) || arg[n] == '_' || arg[n] == '/')
			n += 1;
		if (n) {
			while (IS_ALWAYS_SAFE(arg[n]))
				n += 1;
		start:
			build_command(arg, n);
			arg = &arg[n];
		}
	}
	build_command_asis("\\n')\"");
	/* Add any held back terminal LF's */
	if (lfs) {
		build_command_reserve(lfs + 2U);
		command[command_len++] = '\'';
		memset(&command[command_len], '\n', lfs);
		command_len += lfs;
		command[command_len++] = '\'';
	}

#undef IS_ALWAYS_SAFE
#undef IS_INITIAL_SAFE
}


/**
 * Construct `command`
 *
 * @param  argv  The command to execute remotely and its arguments (`NULL` terminated)
 */
static void
construct_command(char *argv[])
{
	int next_asis = 0;
	size_t i;

	/* Change directory */
	if (dir) {
		build_command_asis("cd -- ");
		build_command_escape(dir);
		build_command_asis(strict_cd ? " && " : " ; ");
	}

	/* Execute command */
	if (is_sshcd) {
		build_command_asis("exec \"$SHELL\" -l");
		goto command_constructed;
	}
	if (asis)
		build_command_asis("( env --");
	else
		build_command_asis("exec env --");
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

	/* Set redirections */
	for (i = 0; i < nredirections; i++) {
		build_command_asis(" ");
		build_command_asis(redirections[i].asis);
		if (redirections[i].escape) {
			build_command_asis(" ");
			build_command_escape(redirections[i].escape);
		}
	}

	/* NUL-terminate `command` */
command_constructed:
	finalise_command();
}


/**
 * Replace "sshexec://" prefix in `destination`
 * with "ssh://" and remove the directory
 * component and, if it present, store the
 * directory to `directory`
 */
static void
extract_directory_from_destination(void)
{
	char *dest_dir_delim;
	if (!is_sshcd && !strncmp(destination, "sshexec://", sizeof("sshexec://") - 1U)) {
		memmove(&destination[sizeof("ssh") - 1U],
		        &destination[sizeof("sshexec") - 1U],
		        strlen(&destination[sizeof("sshexec") - 1U]) + 1U);
		goto using_ssh_prefix;
	} else if (is_sshcd && !strncmp(destination, "sshcd://", sizeof("sshcd://") - 1U)) {
		memmove(&destination[sizeof("ssh") - 1U],
		        &destination[sizeof("sshcd") - 1U],
		        strlen(&destination[sizeof("sshcd") - 1U]) + 1U);
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
}


/**
 * Read and parse the sshexec options
 *
 * @param   argv  The arguments from the command line after
 *                the zeroth argument
 * @return        `argv` offset to skip pass any sshexec options
 */
static char **
parse_sshexec_options(char *argv[])
{
	const char *cd = NULL;
	const char *nasis_str = NULL;
	char *p;

	/* Check that we have any arguments to parse, and that we
	 * have the "{" mark the beginning of sshexec options. */
	if (!*argv || strcmp(*argv, "{"))
		goto end_of_options;
	argv++;

#define STORE_OPT(VARP, OPT)\
	if (!strncmp(*argv, OPT"=", sizeof(OPT))) {\
		if (*(VARP) || !*(*(VARP) = &(*argv)[sizeof(OPT)]))\
			usage();\
		continue;\
	}

	/* Get options */
	for (; *argv && strcmp(*argv, "}"); argv++) {
		/* Options recognised in both sshexec(1) and sshcd(1) */
		STORE_OPT(&ssh, "ssh")
		STORE_OPT(&cd, "cd")

		/* The rest of the options only recognised in sshexec(1) */
		if (is_sshcd)
			usage();

		/* Normal options */
		STORE_OPT(&dir, "dir")
		STORE_OPT(&asis, "asis")
		STORE_OPT(&nasis_str, "nasis")

		/* File descriptor redirections */
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

	/* Check that we have the "}" argument that marks the end of sshexec options */
	if (!*argv)
		usage();
	argv++;

#undef STORE_OPT

end_of_options:
	/* Parse options and set defaults */

	if (!ssh)
		ssh = "ssh";

	if (!cd)
		strict_cd = !is_sshcd;
	else if (!strcmp(cd, "strict"))
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

	return argv;
}


/**
 * Read and parse the ssh(1) options
 *
 * @param   argv       The return value of `parse_sshexec_options`
 * @param   nopts_out  Output parameter for the number of option
 *                     arguments, this includes all arguments the
 *                     return value offsets `argv` except the "--"
 *                     (if there is one), so this includes both
 *                     the options themselves and their arguments
 * @return             `argv` offset to skip pass any ssh(1) options,
 *                     and any "--" immediately after them
 */
static char **
parse_ssh_options(char *argv[], size_t *nopts_out)
{
	enum optclass class;
	const char *arg;
	char opt;
	size_t nopts = 0;

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

	*nopts_out = nopts;
	return argv;
}


int
main(int argc_unused, char *argv[])
{
	char **opts, *p;
	size_t nopts;
	const char **args;
	size_t i;

	(void) argc_unused;

	/* Identify used command name */
	if (*argv)
		argv0 = *argv++;
	p = strrchr(argv0, '/');
	is_sshcd = !strcmp(p ? &p[1] : argv0, "sshcd");

	/* Parse options */
	argv = parse_sshexec_options(argv);
	argv = parse_ssh_options(opts = argv, &nopts);

	/* Check operand count and separate out `destination` from the command and arguments */
	destination = *argv++;
	if (!destination || (is_sshcd ? !!*argv : !*argv))
		usage();

	/* Validate `destination` */
	if (!strcmp(destination, "-"))
		exitf("%s: the command argument must not be \"-\"\n", argv0);
	else if (strchr(destination, '='))
		exitf("%s: the command argument must contain an \'=\'\n", argv0);

	/* Parse command line operands */
	extract_directory_from_destination();
	construct_command(argv);

	/* Construct arguments for ssh(1) and execute */
	i = 0;
	args = calloc(5U + (size_t)is_sshcd + nopts, sizeof(*args));
	if (!args)
		exitf("%s: could not allocate enough memory\n", argv0);
	args[i++] = ssh;
	if (is_sshcd) {
		const char *pty_alloc_flag = getenv("SSHCD_PTY_ALLOC_FLAG");
		if (!pty_alloc_flag)
			args[i++] = "-t";
		else if (*pty_alloc_flag)
			args[i++] = pty_alloc_flag;
	}
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
