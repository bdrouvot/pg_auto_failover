/*
 * src/bin/pg_autoctl/cli_do_misc.c
 *     Implementation of a CLI which lets you run operations on the local
 *     postgres server directly.
 *
 * Copyright (c) Microsoft Corporation. All rights reserved.
 * Licensed under the PostgreSQL License.
 *
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <termios.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include "postgres_fe.h"
#include "pqexpbuffer.h"
#include "snprintf.h"

#include "cli_common.h"
#include "cli_do_root.h"
#include "cli_root.h"
#include "commandline.h"
#include "config.h"
#include "env_utils.h"
#include "log.h"
#include "pidfile.h"
#include "signals.h"
#include "string_utils.h"

#include "runprogram.h"

typedef struct TmuxOptions
{
	char root[MAXPGPATH];
	int firstPort;
	int nodes;
	char layout[BUFSIZE];
} TmuxOptions;

static TmuxOptions tmuxOptions = { 0 };

char *xdg[][3] = {
	{ "XDG_DATA_HOME", "share" },
	{ "XDG_CONFIG_HOME", "config" },
	{ "XDG_RUNTIME_DIR", "run" },
	{ NULL, NULL }
};


static void tmux_add_command(PQExpBuffer script, const char *fmt, ...)
__attribute__((format(printf, 2, 3)));

static void tmux_add_send_keys_command(PQExpBuffer script, const char *fmt, ...)
__attribute__((format(printf, 2, 3)));

static bool tmux_has_session(const char *tmux_path, const char *sessionName);
static void tmux_add_new_session(PQExpBuffer script,
								 const char *root, int pgport);

static void tmux_add_xdg_environment(PQExpBuffer script, const char *root);
static bool tmux_prepare_XDG_environment(const char *root,
										 bool createDirectories);

static void tmux_pg_autoctl_create_monitor(PQExpBuffer script,
										   const char *root,
										   int pgport);

static void tmux_pg_autoctl_create_postgres(PQExpBuffer script,
											const char *root,
											int pgport,
											const char *name);

static bool tmux_start_server(const char *root, const char *scriptName);
static bool pg_autoctl_getpid(const char *pgdata, pid_t *pid);
static bool tmux_stop_pg_autoctl(TmuxOptions *options);
static void tmux_process_options(TmuxOptions *options);
static void tmux_cleanup_stale_directory(TmuxOptions *options);


/*
 * cli_print_version_getopts parses the CLI options for the pg_autoctl version
 * command, which are the usual suspects.
 */
int
cli_do_tmux_script_getopts(int argc, char **argv)
{
	int c, option_index = 0, errors = 0;
	int verboseCount = 0;
	bool printVersion = false;

	TmuxOptions options = { 0 };

	static struct option long_options[] = {
		{ "root", required_argument, NULL, 'D' },
		{ "first-pgport", required_argument, NULL, 'p' },
		{ "nodes", required_argument, NULL, 'n' },
		{ "layout", required_argument, NULL, 'l' },
		{ "version", no_argument, NULL, 'V' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "quiet", no_argument, NULL, 'q' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	optind = 0;

	/* set our defaults */
	options.nodes = 2;
	options.firstPort = 5500;
	strlcpy(options.root, "/tmp/pgaf/tmux", sizeof(options.root));
	strlcpy(options.layout, "even-vertical", sizeof(options.layout));

	/*
	 * The only command lines that are using keeper_cli_getopt_pgdata are
	 * terminal ones: they don't accept subcommands. In that case our option
	 * parsing can happen in any order and we don't need getopt_long to behave
	 * in a POSIXLY_CORRECT way.
	 *
	 * The unsetenv() call allows getopt_long() to reorder arguments for us.
	 */
	unsetenv("POSIXLY_CORRECT");

	while ((c = getopt_long(argc, argv, "D:p:Vvqh",
							long_options, &option_index)) != -1)
	{
		switch (c)
		{
			case 'D':
			{
				strlcpy(options.root, optarg, MAXPGPATH);
				log_trace("--root %s", options.root);
				break;
			}

			case 'p':
			{
				if (!stringToInt(optarg, &options.firstPort))
				{
					log_error("Failed to parse --first-port number \"%s\"",
							  optarg);
					errors++;
				}
				log_trace("--first-port %d", options.firstPort);
				break;
			}

			case 'n':
			{
				if (!stringToInt(optarg, &options.nodes))
				{
					log_error("Failed to parse --nodes number \"%s\"",
							  optarg);
					errors++;
				}
				log_trace("--nodes %d", options.nodes);
				break;
			}

			case 'l':
			{
				strlcpy(options.layout, optarg, MAXPGPATH);
				log_trace("--layout %s", options.layout);
				break;
			}

			case 'h':
			{
				commandline_help(stderr);
				exit(EXIT_CODE_QUIT);
				break;
			}

			case 'V':
			{
				/* keeper_cli_print_version prints version and exits. */
				printVersion = true;
				break;
			}

			case 'v':
			{
				++verboseCount;
				switch (verboseCount)
				{
					case 1:
					{
						log_set_level(LOG_INFO);
						break;
					}

					case 2:
					{
						log_set_level(LOG_DEBUG);
						break;
					}

					default:
					{
						log_set_level(LOG_TRACE);
						break;
					}
				}
				break;
			}

			case 'q':
			{
				log_set_level(LOG_ERROR);
				break;
			}

			default:
			{
				/* getopt_long already wrote an error message */
				errors++;
				break;
			}
		}
	}

	if (errors > 0)
	{
		commandline_help(stderr);
		exit(EXIT_CODE_BAD_ARGS);
	}

	if (printVersion)
	{
		keeper_cli_print_version(argc, argv);
	}

	/* publish parsed options */
	tmuxOptions = options;

	return optind;
}


/*
 * tmux_add_command appends a tmux command to the given script buffer.
 */
static void
tmux_add_command(PQExpBuffer script, const char *fmt, ...)
{
	char buffer[BUFSIZE] = { 0 };
	va_list args;

	va_start(args, fmt);
	pg_vsprintf(buffer, fmt, args);
	va_end(args);

	appendPQExpBuffer(script, "%s\n", buffer);
}


/*
 * tmux_add_send_keys_command appends a tmux send-keys command to the given
 * script buffer, with an additional Enter command.
 */
static void
tmux_add_send_keys_command(PQExpBuffer script, const char *fmt, ...)
{
	char buffer[BUFSIZE] = { 0 };
	va_list args;

	va_start(args, fmt);
	pg_vsprintf(buffer, fmt, args);
	va_end(args);

	appendPQExpBuffer(script, "send-keys '%s' Enter\n", buffer);
}


/*
 * tmux_add_xdg_environment sets the environment variables that we need for the
 * whole session to be self-contained in the given root directory.
 */
static void
tmux_add_xdg_environment(PQExpBuffer script, const char *root)
{
	/*
	 * For demo/tests purposes, arrange a self-contained setup where everything
	 * is to be found in the given options.root directory.
	 */
	for (int i = 0; xdg[i][0] != NULL; i++)
	{
		char *var = xdg[i][0];
		char *dir = xdg[i][1];

		tmux_add_send_keys_command(script,
								   "export %s=\"%s/%s\"", var, root, dir);
	}
}


/*
 * tmux_prepare_XDG_environment set XDG environment variables in the current
 * process tree.
 */
static bool
tmux_prepare_XDG_environment(const char *root, bool createDirectories)
{
	log_info("Preparing XDG setting for self-contained session in \"%s\"",
			 root);

	for (int i = 0; xdg[i][0] != NULL; i++)
	{
		char *var = xdg[i][0];
		char *dir = xdg[i][1];
		char *env = (char *) malloc(MAXPGPATH * sizeof(char));

		if (env == NULL)
		{
			log_fatal("Failed to malloc MAXPGPATH bytes: %m");
			return false;
		}

		sformat(env, MAXPGPATH, "%s/%s", root, dir);

		if (createDirectories)
		{
			log_debug("mkdir -p \"%s\"", env);
			if (pg_mkdir_p(env, 0700) == -1)
			{
				log_error("mkdir -p \"%s\": %m", env);
				return false;
			}
		}

		if (!normalize_filename(env, env, MAXPGPATH))
		{
			/* errors have already been logged */
			return false;
		}

		log_info("export %s=\"%s\"", var, env);

		if (setenv(var, env, 1) != 0)
		{
			log_error("Failed to set environment variable %s to \"%s\": %m",
					  var, env);
		}

		/* also create our actual target directory for our files */
		if (createDirectories)
		{
			char targetPath[MAXPGPATH] = { 0 };

			sformat(targetPath, sizeof(targetPath),
					"%s/pg_config/%s",
					env,

			        /* skip first / in the root directory */
					root[0] == '/' ? root + 1 : root);

			log_debug("mkdir -p \"%s\"", targetPath);
			if (pg_mkdir_p(targetPath, 0700) == -1)
			{
				log_error("mkdir -p \"%s\": %m", targetPath);
				return false;
			}
		}
	}

	return true;
}


/*
 * tmux_add_new_session appends a new-session command with the
 * update-environment options for our XDG settings, as a series of tmux
 * send-keys commands, to the given script buffer.
 */
static void
tmux_add_new_session(PQExpBuffer script, const char *root, int pgport)
{
	char sessionName[BUFSIZE] = { 0 };

	sformat(sessionName, BUFSIZE, "pgautofailover-%d", pgport);

	/*
	 * For demo/tests purposes, arrange a self-contained setup where everything
	 * is to be found in the given options.root directory.
	 */
	for (int i = 0; xdg[i][0] != NULL; i++)
	{
		char *var = xdg[i][0];

		tmux_add_command(script, "set-option update-environment %s", var);
	}

	tmux_add_command(script, "new-session -s %s", sessionName);
}


/*
 * tmux_pg_autoctl_create_monitor appends a pg_autoctl create monitor command
 * to the given script buffer, and also the commands to set PGDATA and PGPORT.
 */
static void
tmux_pg_autoctl_create_monitor(PQExpBuffer script,
							   const char *root,
							   int pgport)
{
	char *pg_ctl_opts = "--hostname localhost --ssl-self-signed --auth trust";

	tmux_add_send_keys_command(script, "export PGPORT=%d", pgport);

	/* the monitor is always named monitor, and does not need --monitor */
	tmux_add_send_keys_command(script, "export PGDATA=\"%s/monitor\"", root);

	tmux_add_send_keys_command(script,
							   "%s create monitor %s --run",
							   pg_autoctl_argv0,
							   pg_ctl_opts);
}


/*
 * tmux_pg_autoctl_create_postgres appends a pg_autoctl create postgres command
 * to the given script buffer, and also the commands to set PGDATA and PGPORT.
 */
static void
tmux_pg_autoctl_create_postgres(PQExpBuffer script,
								const char *root,
								int pgport,
								const char *name)
{
	char monitor[BUFSIZE] = { 0 };
	char *pg_ctl_opts = "--hostname localhost --ssl-self-signed --auth trust";

	tmux_add_send_keys_command(script, "export PGPORT=%d", pgport);

	sformat(monitor, sizeof(monitor),
			"$(%s show uri --pgdata %s/monitor --monitor)",
			pg_autoctl_argv0,
			root);

	tmux_add_send_keys_command(script,
							   "export PGDATA=\"%s/%s\"",
							   root,
							   name);

	tmux_add_send_keys_command(script,
							   "%s create postgres %s "
							   "--monitor %s --name %s --run",
							   pg_autoctl_argv0,
							   pg_ctl_opts,
							   monitor,
							   name);
}


/*
 * prepare_tmux_script prepares a script for a tmux session with the given
 * nodes, root directory, first pgPort, and layout.
 */
static void
prepare_tmux_script(TmuxOptions *options, PQExpBuffer script)
{
	char *root = options->root;
	int pgport = options->firstPort;
	char sessionName[BUFSIZE] = { 0 };

	char previousName[NAMEDATALEN] = { 0 };

	sformat(sessionName, BUFSIZE, "pgautofailover-%d", options->firstPort);

	tmux_add_command(script, "set-option -g default-shell /bin/bash");

	(void) tmux_add_new_session(script, root, pgport);

	/* start a monitor */
	(void) tmux_add_xdg_environment(script, root);
	tmux_pg_autoctl_create_monitor(script, root, pgport++);

	/* start the Postgres nodes, using the monitor URI */
	sformat(previousName, sizeof(previousName), "monitor");

	for (int i = 0; i < options->nodes; i++)
	{
		char name[NAMEDATALEN] = { 0 };

		sformat(name, sizeof(name), "node%d", i + 1);

		tmux_add_command(script, "split-window -v");
		tmux_add_command(script, "select-layout even-vertical");

		(void) tmux_add_xdg_environment(script, root);

		/*
		 * Force node ordering to easy debugging of interactive sessions: each
		 * node waits until the previous one has been started or registered.
		 */
		tmux_add_send_keys_command(script,
								   "PG_AUTOCTL_DEBUG=1 "
								   "%s do tmux wait --root %s %s",
								   pg_autoctl_argv0,
								   options->root,
								   previousName);

		tmux_pg_autoctl_create_postgres(script, root, pgport++, name);
		tmux_add_send_keys_command(script, "pg_autoctl run");

		strlcpy(previousName, name, sizeof(previousName));
	}

	/* add a window for pg_autoctl show state */
	tmux_add_command(script, "split-window -v");
	tmux_add_command(script, "select-layout even-vertical");

	(void) tmux_add_xdg_environment(script, root);
	tmux_add_send_keys_command(script, "export PGDATA=\"%s/monitor\"", root);
	tmux_add_send_keys_command(script,
							   "PG_AUTOCTL_DEBUG=1 "
							   "%s do tmux wait --root %s %s",
							   pg_autoctl_argv0,
							   options->root,
							   "monitor");
	tmux_add_send_keys_command(script,
							   "watch -n 0.2 %s show state",
							   pg_autoctl_argv0);

	/* add a window for interactive pg_autoctl commands */
	tmux_add_command(script, "split-window -v");
	tmux_add_command(script, "select-layout even-vertical");
	(void) tmux_add_xdg_environment(script, root);
	tmux_add_send_keys_command(script, "cd \"%s\"", root);
	tmux_add_send_keys_command(script, "export PGDATA=\"%s/monitor\"", root);

	/* now select our target layout */
	tmux_add_command(script, "select-layout %s", options->layout);

	if (env_exists("TMUX_EXTRA_COMMANDS"))
	{
		char extra_commands[BUFSIZE] = { 0 };

		char *extraLines[BUFSIZE];
		int lineCount = 0;
		int lineNumber = 0;

		if (!get_env_copy("TMUX_EXTRA_COMMANDS", extra_commands, BUFSIZE))
		{
			/* errors have already been logged */
			exit(EXIT_CODE_INTERNAL_ERROR);
		}

		lineCount = splitLines(extra_commands, extraLines, BUFSIZE);

		for (lineNumber = 0; lineNumber < lineCount; lineNumber++)
		{
			appendPQExpBuffer(script, "%s\n", extraLines[lineNumber]);
		}
	}
}


/*
 * tmux_start_server starts a tmux session with the given script.
 */
static bool
tmux_start_server(const char *root, const char *scriptName)
{
	Program program;

	char *args[8];
	int argsIndex = 0;

	char tmux[MAXPGPATH] = { 0 };
	char command[BUFSIZE] = { 0 };

	if (setenv("PG_AUTOCTL_DEBUG", "1", 1) != 0)
	{
		log_error("Failed to set environment PG_AUTOCTL_DEBUG: %m");
		return false;
	}

	if (!search_path_first("tmux", tmux))
	{
		log_fatal("Failed to find program tmux in PATH");
		return false;
	}

	/*
	 * Run the tmux command with our script:
	 *   tmux start-server \; source-file ${scriptName}
	 */
	args[argsIndex++] = (char *) tmux;
	args[argsIndex++] = "-v";
	args[argsIndex++] = "start-server";
	args[argsIndex++] = ";";
	args[argsIndex++] = "source-file";
	args[argsIndex++] = (char *) scriptName;
	args[argsIndex] = NULL;

	/* we do not want to call setsid() when running this program. */
	program = initialize_program(args, false);

	program.capture = false;    /* don't capture output */
	program.tty = true;         /* allow sharing the parent's tty */

	/* log the exact command line we're using */
	(void) snprintf_program_command_line(&program, command, BUFSIZE);
	log_info("%s", command);

	(void) execute_subprogram(&program);

	/* we only get there when the tmux session is done */
	return true;
}


/*
 * pg_autoctl_signal sends the given signal to the pg_autoctl process that is
 * running for the given PGDATA localtion.
 */
static bool
pg_autoctl_getpid(const char *pgdata, pid_t *pid)
{
	ConfigFilePaths pathnames = { 0 };

	if (!keeper_config_set_pathnames_from_pgdata(&pathnames, pgdata))
	{
		exit(EXIT_CODE_INTERNAL_ERROR);
	}

	return read_pidfile(pathnames.pid, pid);
}


/*
 * tmux_stop_pg_autoctl stops all started pg_autoctl programs in a tmux
 * sessions.
 */
static bool
tmux_stop_pg_autoctl(TmuxOptions *options)
{
	bool success = true;

	int signals[] = { SIGTERM, SIGINT, SIGQUIT };
	int signalsCount = sizeof(signals) / sizeof(signals[0]);

	/* signal processes using increasing levels of urge to quit now */
	for (int s = 0; s < signalsCount; s++)
	{
		int countRunning = options->nodes + 1;

		for (int i = 0; i <= options->nodes; i++)
		{
			pid_t pid = 0;
			char name[MAXPGPATH] = { 0 };
			char pgdata[MAXPGPATH] = { 0 };

			if (i == options->nodes)
			{
				sformat(name, sizeof(name), "monitor");
			}
			else
			{
				sformat(name, sizeof(name), "node%d", i + 1);
			}

			sformat(pgdata, sizeof(pgdata), "%s/%s", options->root, name);

			if (!pg_autoctl_getpid(pgdata, &pid))
			{
				/* we don't have a pid */
				log_info("No pidfile for pg_autoctl for node \"%s\"", name);
				--countRunning;
				continue;
			}

			if (kill(pid, 0) == -1 && errno == ESRCH)
			{
				log_info("Pid %d for node \"%s\" is not running anymore",
						 pid, name);
				--countRunning;
			}
			else
			{
				log_info("Sending signal %s to pid %d for node \"%s\"",
						 signal_to_string(signals[s]), pid, name);

				if (kill(pid, signals[s]) != 0)
				{
					log_info("Failed to send %s to pid %d",
							 signal_to_string(signals[s]), pid);
				}
			}
		}

		if (countRunning == 0)
		{
			break;
		}

		/* sleep enough time that the processes might already be dead */
		sleep(1);
	}

	return success;
}


/*
 * tmux_has_session runs the command `tmux has-session -f sessionName`.
 */
static bool
tmux_has_session(const char *tmux_path, const char *sessionName)
{
	Program program;
	int returnCode;
	char command[BUFSIZE] = { 0 };

	program = run_program(tmux_path, "has-session", "-t", sessionName, NULL);
	returnCode = program.returnCode;

	(void) snprintf_program_command_line(&program, command, BUFSIZE);
	log_debug("%s", command);

	if (program.stdOut)
	{
		char *outLines[BUFSIZE] = { 0 };
		int lineCount = splitLines(program.stdOut, outLines, BUFSIZE);
		int lineNumber = 0;

		for (lineNumber = 0; lineNumber < lineCount; lineNumber++)
		{
			log_info("tmux has-session: %s", outLines[lineNumber]);
		}
	}

	if (program.stdErr)
	{
		char *errLines[BUFSIZE] = { 0 };
		int lineCount = splitLines(program.stdOut, errLines, BUFSIZE);
		int lineNumber = 0;

		for (lineNumber = 0; lineNumber < lineCount; lineNumber++)
		{
			log_error("tmux has-session: %s", errLines[lineNumber]);
		}
	}

	free_program(&program);

	/*
	 * From tmux has-session manual page:
	 *
	 * Report an error and exit with 1 if the specified session does not exist.
	 * If it does exist, exit with 0.
	 */
	return returnCode == 0;
}


/*
 * tmux_kill_session runs the command:
 *   tmux kill-session -t pgautofailover-${first-pgport}
 */
static bool
tmux_kill_session(TmuxOptions *options)
{
	Program program;
	char tmux[MAXPGPATH] = { 0 };
	char command[BUFSIZE] = { 0 };
	char sessionName[BUFSIZE] = { 0 };

	bool success = true;

	sformat(sessionName, BUFSIZE, "pgautofailover-%d", options->firstPort);

	if (!search_path_first("tmux", tmux))
	{
		log_fatal("Failed to find program tmux in PATH");
		return false;
	}

	if (!tmux_has_session(tmux, sessionName))
	{
		log_info("Tmux session \"%s\" does not exists", sessionName);
		return true;
	}

	program = run_program(tmux, "kill-session", "-t", sessionName, NULL);

	(void) snprintf_program_command_line(&program, command, BUFSIZE);
	log_info("%s", command);

	if (program.stdOut)
	{
		char *outLines[BUFSIZE] = { 0 };
		int lineCount = splitLines(program.stdOut, outLines, BUFSIZE);
		int lineNumber = 0;

		for (lineNumber = 0; lineNumber < lineCount; lineNumber++)
		{
			log_info("tmux kill-session: %s", outLines[lineNumber]);
		}
	}

	if (program.stdErr)
	{
		char *errLines[BUFSIZE] = { 0 };
		int lineCount = splitLines(program.stdErr, errLines, BUFSIZE);
		int lineNumber = 0;

		for (lineNumber = 0; lineNumber < lineCount; lineNumber++)
		{
			log_error("tmux kill-session: %s", errLines[lineNumber]);
		}
	}

	if (program.returnCode != 0)
	{
		success = false;
		log_warn("Failed to kill tmux sessions \"%s\"", sessionName);
	}

	free_program(&program);

	return success;
}


/*
 * tmux_process_options processes the tmux commands options. The main activity
 * here is to ensure that the "root" directory exists and normalize its
 * internal pathname in the options structure.
 */
static void
tmux_process_options(TmuxOptions *options)
{
	log_debug("tmux_process_options");
	log_debug("mkdir -p \"%s\"", options->root);

	if (pg_mkdir_p(options->root, 0700) == -1)
	{
		log_fatal("mkdir -p \"%s\": %m", options->root);
		exit(EXIT_CODE_INTERNAL_ERROR);
	}

	log_debug("normalize_filename \"%s\"", options->root);

	if (!normalize_filename(options->root, options->root, MAXPGPATH))
	{
		/* errors have already been logged. */
		exit(EXIT_CODE_INTERNAL_ERROR);
	}

	log_debug("Real path for root directory: \"%s\"", options->root);
}


/*
 * tmux_cleanup_stale_directory cleans-up the pg_autoctl processes and then the
 * root directory of a tmux session, and then kills the tmux session.
 */
static void
tmux_cleanup_stale_directory(TmuxOptions *options)
{
	if (!directory_exists(options->root))
	{
		log_info("Directory \"%s\" does not exists, nothing to clean-up",
				 options->root);
		return;
	}

	if (!normalize_filename(options->root, options->root, MAXPGPATH))
	{
		/* errors have already been logged. */
		exit(EXIT_CODE_INTERNAL_ERROR);
	}

	/* prepare the XDG environment */
	if (!tmux_prepare_XDG_environment(options->root, false))
	{
		exit(EXIT_CODE_INTERNAL_ERROR);
	}

	log_info("Checking for stale pg_autoctl process in \"%s\"", options->root);
	(void) tmux_stop_pg_autoctl(options);

	log_info("Removing stale directory: rm -rf \"%s\"", options->root);
	if (!rmtree(options->root, true))
	{
		log_error("Failed to remove directory \"%s\": %m", options->root);
		exit(EXIT_CODE_INTERNAL_ERROR);
	}

	log_info("Checking for stale tmux session \"pgautofailover-%d\"",
			 options->firstPort);

	if (!tmux_kill_session(options))
	{
		exit(EXIT_CODE_INTERNAL_ERROR);
	}
}


/*
 * keeper_cli_tmux_script generates a tmux script to run a test case or a demo
 * for pg_auto_failover easily.
 */
void
cli_do_tmux_script(int argc, char **argv)
{
	TmuxOptions options = tmuxOptions;
	PQExpBuffer script = createPQExpBuffer();

	(void) tmux_process_options(&options);

	/* prepare the XDG environment */
	if (!tmux_prepare_XDG_environment(options.root, true))
	{
		exit(EXIT_CODE_INTERNAL_ERROR);
	}

	if (script == NULL)
	{
		log_error("Failed to allocate memory");
		exit(EXIT_CODE_INTERNAL_ERROR);
	}

	/* prepare the tmux script */
	(void) prepare_tmux_script(&options, script);

	/* memory allocation could have failed while building string */
	if (PQExpBufferBroken(script))
	{
		log_error("Failed to allocate memory");
		destroyPQExpBuffer(script);

		exit(EXIT_CODE_INTERNAL_ERROR);
	}

	fformat(stdout, "%s", script->data);

	destroyPQExpBuffer(script);
}


/*
 * cli_do_tmux_session starts an interactive tmux session with the given
 * specifications for a cluster. When the session is detached, the pg_autoctl
 * processes are stopped.
 */
void
cli_do_tmux_session(int argc, char **argv)
{
	TmuxOptions options = tmuxOptions;
	PQExpBuffer script = createPQExpBuffer();

	char scriptName[MAXPGPATH] = { 0 };

	bool success = true;

	/*
	 * We need to make sure we start from a clean slate.
	 */
	(void) tmux_cleanup_stale_directory(&options);

	/*
	 * Write the script to "script-${first-pgport}.tmux" file in the root
	 * directory.
	 */
	(void) tmux_process_options(&options);

	/* prepare the XDG environment */
	if (!tmux_prepare_XDG_environment(options.root, true))
	{
		exit(EXIT_CODE_INTERNAL_ERROR);
	}

	/*
	 * Prepare the tmux script.
	 */
	if (script == NULL)
	{
		log_error("Failed to allocate memory");
		exit(EXIT_CODE_INTERNAL_ERROR);
	}

	(void) prepare_tmux_script(&options, script);

	/* memory allocation could have failed while building string */
	if (PQExpBufferBroken(script))
	{
		log_error("Failed to allocate memory");
		destroyPQExpBuffer(script);

		exit(EXIT_CODE_INTERNAL_ERROR);
	}

	/*
	 * Write the script to file.
	 */
	sformat(scriptName, sizeof(scriptName),
			"%s/script-%d.tmux", options.root, options.firstPort);

	log_info("Writing tmux session script \"%s\"", scriptName);

	if (!write_file(script->data, script->len, scriptName))
	{
		log_fatal("Failed to write tmux script at \"%s\"", scriptName);
		exit(EXIT_CODE_INTERNAL_ERROR);
	}

	destroyPQExpBuffer(script);

	/*
	 * Start a tmux session from the script.
	 */
	if (!tmux_start_server(options.root, scriptName))
	{
		success = false;
		log_fatal("Failed to start the tmux session, see above for details");
	}

	/*
	 * Stop our pg_autoctl processes and kill the tmux session.
	 */
	log_info("tmux session ended: kill pg_autoct processes");
	success = success && tmux_stop_pg_autoctl(&options);
	success = success && tmux_kill_session(&options);

	if (!success)
	{
		exit(EXIT_CODE_INTERNAL_ERROR);
	}
}


/*
 * cli_do_tmux_stop send termination signals on all the pg_autoctl process that
 * might be running in a tmux session.
 */
void
cli_do_tmux_stop(int argc, char **argv)
{
	TmuxOptions options = tmuxOptions;

	(void) tmux_process_options(&options);

	/* prepare the XDG environment */
	if (!tmux_prepare_XDG_environment(options.root, false))
	{
		exit(EXIT_CODE_INTERNAL_ERROR);
	}

	if (!tmux_stop_pg_autoctl(&options))
	{
		exit(EXIT_CODE_INTERNAL_ERROR);
	}
}


/*
 * cli_do_tmux_clean cleans-up the pg_autoctl processes and then the root
 * directory of a tmux session, and then kills the tmux session.
 */
void
cli_do_tmux_clean(int argc, char **argv)
{
	TmuxOptions options = tmuxOptions;

	(void) tmux_cleanup_stale_directory(&options);
}


/*
 * cli_do_tmux_wait waits until given node name has been registered. When the
 * target node name is the "monitor" just wait until Postgres is running.
 */
void
cli_do_tmux_wait(int argc, char **argv)
{
	TmuxOptions options = tmuxOptions;
	char nodeName[NAMEDATALEN] = { 0 };

	(void) tmux_process_options(&options);

	/* prepare the XDG environment */
	if (!tmux_prepare_XDG_environment(options.root, false))
	{
		exit(EXIT_CODE_INTERNAL_ERROR);
	}

	if (argc != 1)
	{
		log_fatal("Expected one argument for the target node name");
		exit(EXIT_CODE_INTERNAL_ERROR);
	}
	strlcpy(nodeName, argv[0], sizeof(nodeName));

	if (strcmp(nodeName, "monitor") == 0)
	{
		int timeout = 30;
		bool ready = false;
		Program program;
		char pgdata[MAXPGPATH] = { 0 };

		sformat(pgdata, sizeof(pgdata), "%s/%s", options.root, nodeName);

		/* leave some time for initdb and stuff */
		sleep(2);

		program = run_program(pg_autoctl_program, "do", "pgsetup", "wait",
							  "--pgdata", pgdata, NULL);

		if (program.returnCode != 0)
		{
			char command[BUFSIZE];

			(void) snprintf_program_command_line(&program, command, BUFSIZE);

			log_error("%s [%d]", command, program.returnCode);
			exit(EXIT_CODE_INTERNAL_ERROR);
		}

		log_info("Postgres is running at \"%s\"", pgdata);

		/* Postgres is running on the monitor, is it ready though? */
		while (!ready && timeout > 0)
		{
			char command[BUFSIZE];

			Program showUri = run_program(pg_autoctl_program,
										  "show", "uri", "--monitor",
										  "--pgdata", pgdata, NULL);

			(void) snprintf_program_command_line(&showUri, command, BUFSIZE);
			log_info("%s [%d]", command, showUri.returnCode);

			ready = showUri.returnCode == 0;
			--timeout;

			if (ready)
			{
				log_info("The monitor is ready at: %s", showUri.stdOut);
			}
		}

		if (!ready)
		{
			exit(EXIT_CODE_INTERNAL_ERROR);
		}
	}
	else
	{
		/*
		 * Not a monitor node: only wait until the node has been registered to
		 * the monitor. We know that the node has been registered when a state
		 * file exists.
		 */
		int timeout = 60;
		int countDots = 0;
		char pgdata[MAXPGPATH] = { 0 };
		ConfigFilePaths pathnames = { 0 };

		sformat(pgdata, sizeof(pgdata), "%s/%s", options.root, nodeName);

		log_info("Waiting for a node state file for PGDATA \"%s\"", pgdata);

		while (timeout > 0)
		{
			if (IS_EMPTY_STRING_BUFFER(pathnames.state))
			{
				if (keeper_config_set_pathnames_from_pgdata(&pathnames,
															pgdata))
				{
					log_info("Waiting for creation of a state file at \"%s\"",
							 pathnames.state);
				}
			}

			if (file_exists(pathnames.state))
			{
				KeeperStateData keeperState = { 0 };

				if (keeper_state_read(&keeperState, pathnames.state))
				{
					if (keeperState.assigned_role > INIT_STATE)
					{
						break;
					}
				}
			}

			fformat(stderr, ".");
			++countDots;

			sleep(1);
			--timeout;
		}

		if (countDots > 0)
		{
			fformat(stderr, "\n");
		}

		/* we might have reached the timeout */
		if (!file_exists(pathnames.state))
		{
			log_fatal("Reached timeout while waiting for state file \"%s\"",
					  pathnames.state);
			exit(EXIT_CODE_INTERNAL_ERROR);
		}
	}
}
