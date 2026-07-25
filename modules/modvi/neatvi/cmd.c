/*
 * cmd.c -- replaces neatvi's original process-spawning implementation.
 * Copyright (c) 2026 8bitmcu
 * License: MIT
 *
 * The original backed `:!cmd`, `:r !cmd`, `:w !cmd`, and command
 * filtering with fork()/pipe()/execvp()/waitpid() (cmd_pipe()/cmd_exec())
 * plus an AF_UNIX socket helper (cmd_unix()) for an external command
 * server -- none of which has an ESP32 equivalent (no fork()). Public
 * signatures are unchanged so ex.c's/vi.c's calls into this file don't
 * need to change; each body now just reports "not supported" via
 * ex_show() (the same status-line mechanism every other ex error uses)
 * and returns failure, so callers see a clear message instead of a
 * silent no-op or a hang.
 *
 * Explicitly out of scope for this port (a possible future idea, not
 * committed to): wiring `:!cmd` to vtOS's own shell command dispatcher
 * -- running vtOS's registered commands, not real Unix binaries -- via
 * the same mp_task-callback-bridge pattern used elsewhere in this
 * codebase.
 */

#include "vi.h"

char *cmd_pipe(char *cmd, char *ibuf, int oproc)
{
	(void)cmd;
	(void)ibuf;
	(void)oproc;
	ex_show("external commands are not supported on this platform");
	return NULL;
}

int cmd_exec(char *cmd)
{
	(void)cmd;
	ex_show("external commands are not supported on this platform");
	return 1;
}

char *cmd_unix(char *path, char *ibuf)
{
	(void)path;
	(void)ibuf;
	ex_show("external commands are not supported on this platform");
	return NULL;
}
