/*
 * dinit.c - Dumb interface, initialization
 *
 * This file is part of Frotz.
 *
 * Frotz is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Frotz is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 * Or visit http://www.fsf.org/
 */

#include <stdarg.h>

#include "dfrotz.h"
#include "dblorb.h"

#include <setjmp.h>
extern jmp_buf frotz_exit_env;

extern f_setup_t f_setup;
extern z_header_t z_header;

static void usage(void);
static void print_version(void);

#define INFORMATION "\
An interpreter for all Infocom and other Z-Machine games.\r\n\
\n\
Syntax: dfrotz [options] story-file [blorb file]\r\n\
  -a   watch attribute setting    \t -q   quiet mode (no startup messages)\r\n\
  -A   watch attribute testing    \t -r <option> Set runtime options\r\n\
  -f <type> type of format codes  \t -R <path> restricted read/write\r\n\
  -h # screen height              \t -s # random number seed value\r\n\
  -i   ignore fatal errors        \t -S # transcript width\r\n\
  -I # interpreter number         \t -t   set Tandy bit\r\n\
  -o   watch object movement      \t -T   start transcript on startup\r\n\
  -O   watch object locating      \t -u # slots for multiple undo\r\n\
  -L <file> load this save file   \t -v   show version information\r\n\
  -m   turn off MORE prompts      \t -w # screen width\r\n\
  -n <file> set transcript filename\t -x   expand abbreviations g/x/z\r\n\
  -p   plain ASCII output only    \t -Z # error checking (see below)\r\n\
  -P   alter piracy opcode\n"

#define INFO2 "\
Error checking: 0 none, 1 first only (default), 2 all, 3 exit after any error.\r\n\
For more options and explanations, please read the manual page.\r\n\n\
While running, enter \"\\help\" to list the runtime escape sequences.\r\n"


static int user_text_width = 80;
static int user_text_height = 24;
static int user_random_seed = -1;
static bool plain_ascii = FALSE;

bool quiet_mode;
bool do_more_prompts;

/*
 * os_process_arguments
 *
 * Handle command line switches.
 * Some variables may be set to activate special features of Frotz.
 *
 */
void os_process_arguments(int argc, char *argv[])
{
	int c, num;
	char *p = NULL;
	char *format_orig = NULL;

	zoptarg = NULL;

	do_more_prompts = TRUE;
	quiet_mode = FALSE;
	/* Parse the options */
	do {
		c = zgetopt(argc, argv, "aAf:h:iI:L:mn:oOpPqr:R:s:S:tTu:vw:xZ:");
		switch(c) {
		case 'a':
			f_setup.attribute_assignment = 1;
			break;
		case 'A':
			f_setup.attribute_testing = 1;
			break;
		case 'f':
#ifdef DISABLE_FORMATS
			f_setup.format = FORMAT_DISABLED;
			break;
#endif
			f_setup.format = FORMAT_NORMAL;
			format_orig = strdup(zoptarg);
			for (num = 0; zoptarg[num] != 0; num++)
				zoptarg[num] = tolower((int) zoptarg[num]);
			if (strcmp(zoptarg, "irc") == 0) {
				f_setup.format = FORMAT_IRC;
			} else if (strcmp(zoptarg, "ansi") == 0) {
				f_setup.format = FORMAT_ANSI;
			} else if (strcmp(zoptarg, "bbcode") == 0) {
				f_setup.format = FORMAT_BBCODE;
			} else if ((strcmp(zoptarg, "none") == 0) ||
				(strcmp(zoptarg, "normal") == 0)) {
			} else
				f_setup.format = FORMAT_UNKNOWN;
			break;
		case 'h':
			user_text_height = atoi(zoptarg);
			break;
		case 'i':
			f_setup.ignore_errors = 1;
			break;
		case 'I':
			f_setup.interpreter_number = atoi(zoptarg);
			break;
		case 'L':
			f_setup.restore_mode = 1;
			f_setup.tmp_save_name = strdup(zoptarg);
			break;
		case 'm':
			do_more_prompts = FALSE;
			break;
		case 'n':
			f_setup.script_name_override = strdup(zoptarg);
			break;
		case 'o':
			f_setup.object_movement = 1;
			break;
		case 'O':
			f_setup.object_locating = 1;
			break;
		case 'P':
			f_setup.piracy = 1;
			break;
		case 'p':
			plain_ascii = 1;
			break;
		case 'q':
			quiet_mode = 1;
			break;
		case 'r':
			dumb_handle_setting(zoptarg, FALSE, TRUE);
			break;
		case 'R':
			f_setup.restricted_path = strndup(zoptarg, PATH_MAX);
			break;
		case 's':
			user_random_seed = atoi(zoptarg);
			break;
		case 'S':
			f_setup.script_cols = atoi(zoptarg);
			break;
		case 't':
			f_setup.tandy = 1;
			break;
		case 'T':
			modzm_printf("Starting transcript from the beginning.\r\n");
			f_setup.script_now = 1;
			break;
		case 'u':
			f_setup.undo_slots = atoi(zoptarg);
			break;
		case 'v':
			print_version();
			os_quit(EXIT_SUCCESS);
			break;
		case 'w':
			user_text_width = atoi(zoptarg);
			break;
		case 'x':
			f_setup.expand_abbreviations = 1;
			break;
		case 'Z':
			f_setup.err_report_mode = atoi(zoptarg);
			if ((f_setup.err_report_mode < ERR_REPORT_NEVER) ||
			 	(f_setup.err_report_mode > ERR_REPORT_FATAL))
				f_setup.err_report_mode =
					ERR_DEFAULT_REPORT_MODE;
			break;
		case '?':
			usage();
			os_quit(EXIT_FAILURE);
			break;
		}
	} while (c != EOF);

	if (argv[zoptind] == NULL) {
		usage();
		os_quit(EXIT_SUCCESS);
	}

	if (!quiet_mode) {
		switch (f_setup.format) {
		case FORMAT_NORMAL:
			modzm_printf("Using normal formatting.\r\n");
			break;
		case FORMAT_IRC:
			modzm_printf("Using IRC formatting.\r\n");
			break;
		case FORMAT_ANSI:
			modzm_printf("Using ANSI formatting.\r\n");
			break;
		case FORMAT_BBCODE:
			modzm_printf("Using Discourse BBCode formatting.\r\n");
			f_setup.format = FORMAT_BBCODE;
			break;
		case FORMAT_UNKNOWN:
			modzm_printf("Unknown formatting \"%s\".  Using normal formatting instead.\r\n", format_orig);
			break;
		case FORMAT_DISABLED:
			modzm_printf("Format selection disabled at compile time.\r\n");
			break;
		default:
			modzm_printf("Something else happened with format selection.\r\n");
			modzm_printf("This should not happen.\r\n");
			break;
		}

		if (f_setup.script_now)
			modzm_printf("Starting transcript from the beginning.\r\n");

	}
	if (f_setup.format == FORMAT_UNKNOWN || f_setup.format == FORMAT_DISABLED)
		f_setup.format = FORMAT_NORMAL;

	/* Save the story file name */
	f_setup.story_file = strdup(argv[zoptind]);

#ifdef NO_BASENAME
	f_setup.story_name = strdup(f_setup.story_file);
#else
	f_setup.story_name = strdup(modzm_basename(argv[zoptind]));
#endif
	if (argv[zoptind+1] != NULL)
		f_setup.blorb_file = strdup(argv[zoptind+1]);

	if (!quiet_mode) {
		modzm_printf("Loading %s.\r\n", f_setup.story_file);

#ifndef NO_BLORB
	if (f_setup.blorb_file != NULL)
		modzm_printf("Also loading %s.\r\n", f_setup.blorb_file);
#endif
	}

	/* Now strip off the extension */
	p = strrchr(f_setup.story_name, '.');
	if ( p != NULL )
		*p = '\0';	/* extension removed */

	/* Create nice default file names */
	f_setup.script_name = malloc((strlen(f_setup.story_name) + strlen(EXT_SCRIPT) + 1) * sizeof(char));
	memcpy(f_setup.script_name, f_setup.story_name, (strlen(f_setup.story_name) + strlen(EXT_SCRIPT)) * sizeof(char));
	strncat(f_setup.script_name, EXT_SCRIPT, strlen(EXT_SCRIPT)+1);

	f_setup.command_name = malloc((strlen(f_setup.story_name) + strlen(EXT_COMMAND) + 1) * sizeof(char));
	memcpy(f_setup.command_name, f_setup.story_name, (strlen(f_setup.story_name) + strlen(EXT_COMMAND)) * sizeof(char));
	strncat(f_setup.command_name, EXT_COMMAND, strlen(EXT_COMMAND)+1);

	if (!f_setup.restore_mode) {
		f_setup.save_name = malloc((strlen(f_setup.story_name) + strlen(EXT_SAVE) + 1) * sizeof(char));
		memcpy(f_setup.save_name, f_setup.story_name, (strlen(f_setup.story_name) + strlen(EXT_SAVE)) * sizeof(char));
		strncat(f_setup.save_name, EXT_SAVE, strlen(EXT_SAVE) + 1);
	} else { /* Set our auto load save as the name save */
		f_setup.save_name = malloc((strlen(f_setup.tmp_save_name) + strlen(EXT_SAVE) + 1) * sizeof(char));
                memcpy(f_setup.save_name, f_setup.tmp_save_name, (strlen(f_setup.tmp_save_name) + strlen(EXT_SAVE)) * sizeof(char));
                free(f_setup.tmp_save_name);
	}

	f_setup.aux_name = malloc((strlen(f_setup.story_name) + strlen(EXT_AUX) + 1) * sizeof(char));
	memcpy(f_setup.aux_name, f_setup.story_name, (strlen(f_setup.story_name) + strlen(EXT_AUX)) * sizeof(char));
	strncat(f_setup.aux_name, EXT_AUX, strlen(EXT_AUX) + 1);
} /* os_process_arguments */


void os_init_screen(void)
{
	if (z_header.version == V3 && f_setup.tandy)
		z_header.config |= CONFIG_TANDY;

	if (z_header.version >= V5 && f_setup.undo_slots == 0)
		z_header.flags &= ~UNDO_FLAG;

	z_header.screen_rows = user_text_height;
	z_header.screen_cols = user_text_width;

	/* Use the ms-dos interpreter number for v6, because that's the
	 * kind of graphics files we understand.  Otherwise, use DEC.  */
	if (f_setup.interpreter_number == INTERP_DEFAULT)
		z_header.interpreter_number = z_header.version ==
			6 ? INTERP_MSDOS : INTERP_DEC_20;
	else
		z_header.interpreter_number = f_setup.interpreter_number;

	z_header.interpreter_version = 'F';

	dumb_init_input();
	dumb_init_output();
	dumb_init_pictures();
} /* os_init_screen */


int os_random_seed (void)
{
	if (user_random_seed == -1)	/* Use the epoch as seed value */
		return (time(0) & 0x7fff);
	return user_random_seed;
} /* os_random_seed */


/*
 * os_quit
 *
 * Immediately and cleanly exit, passing along exit status.
 *
 */
void os_quit(int status)
{
  longjmp(frotz_exit_env, 1);
	//exit(status);
} /* os_quit */


void os_restart_game (int UNUSED (stage)) {}


/*
 * os_warn
 *
 * Display a warning message and continue with the game.
 *
 */
void os_warn (const char *s, ...)
{
	va_list m;
	int style;

	os_beep(BEEP_HIGH);
	style = os_get_text_style();
	os_set_text_style(BOLDFACE_STYLE);
	fprintf(stderr, "Warning: ");
	os_set_text_style(NORMAL_STYLE);
	va_start(m, s);
	vfprintf(stderr, s, m);
	va_end(m);
	fprintf(stderr, "\n");
	os_set_text_style(style);
	return;
} /* os_warn */


/*
 * os_fatal
 *
 * Display error message and exit program.
 *
 */
void os_fatal (const char *s, ...)
{
	dumb_show_screen(FALSE);
	modzm_printf("\nFatal error: %s\r\n", s);
	if (f_setup.ignore_errors)
		modzm_printf("Continuing anyway...\r\n");
	else
		os_quit(EXIT_FAILURE);
} /* os_fatal */


FILE *os_load_story(void)
{
#ifndef NO_BLORB
	FILE *fp;

	switch (dumb_blorb_init(f_setup.story_file)) {
	case bb_err_NoBlorb:
		/* printf("No blorb file found.\n\n"); */
		break;
	case bb_err_Format:
		modzm_printf("Blorb file loaded, but unable to build map.\r\n\n");
		break;
	case bb_err_NotFound:
		modzm_printf("Blorb file loaded, but lacks executable chunk.\r\n\n");
		break;
	case bb_err_None:
		/* printf("No blorb errors.\n\n"); */
		break;
	}

	fp = modzm_fopen(f_setup.story_file, "rb");

	/* Is this a Blorb file containing Zcode? */
	if (f_setup.exec_in_blorb)
		modzm_fseek(fp, blorb_res.data.startpos, SEEK_SET);

	return fp;
#else
	return modzm_fopen(f_setup.story_file, "rb");
#endif
} /* os_load_story */


/*
 * Seek into a storyfile, either a standalone file or the
 * ZCODE chunk of a blorb file.
 */
int os_storyfile_seek(FILE * fp, long offset, int whence)
{
#ifndef NO_BLORB
	/* Is this a Blorb file containing Zcode? */
	if (f_setup.exec_in_blorb) {
		switch (whence) {
		case SEEK_END:
			return modzm_fseek(fp, blorb_res.data.startpos + blorb_res.length + offset, SEEK_SET);
			break;
		case SEEK_CUR:
			return modzm_fseek(fp, offset, SEEK_CUR);
			break;
		case SEEK_SET:
			/* SEEK_SET falls through to default */
		default:
			return modzm_fseek(fp, blorb_res.data.startpos + offset, SEEK_SET);
			break;
		}
	} else
		return modzm_fseek(fp, offset, whence);
#else
	return modzm_fseek(fp, offset, whence);
#endif
} /* os_storyfile_seek */


/*
 * Tell the position in a storyfile, either a standalone file
 * or the ZCODE chunk of a blorb file.
 */
int os_storyfile_tell(FILE * fp)
{
#ifndef NO_BLORB
	/* Is this a Blorb file containing Zcode? */
	if (f_setup.exec_in_blorb)
		return modzm_ftell(fp) - blorb_res.data.startpos;
	else
		return modzm_ftell(fp);
#else
	return modzm_ftell(fp);
#endif
} /* os_storyfile_tell */


void os_init_setup(void)
{
	/* Nothing here */
} /* os_init_setup */


static void usage(void)
{
	modzm_printf("FROTZ V%s - Dumb interface.\r\n", VERSION);
	modzm_printf(INFORMATION);
	modzm_printf(INFO2);
	return;
} /* usage */


static void print_version(void)
{
	modzm_printf("FROTZ V%s     Dumb interface.\r\n", VERSION);
	modzm_printf("Commit date:    %s\r\n", GIT_DATE);
	modzm_printf("Git commit:     %s\r\n", GIT_HASH);
	modzm_printf("Notes:          %s\r\n", RELEASE_NOTES);
	modzm_printf("  Frotz was originally written by Stefan Jokisch.\r\n");
	modzm_printf("  It complies with standard 1.1 of the Z-Machine Standard.\r\n");
	modzm_printf("  It was ported to Unix by Galen Hazelwood.\r\n");
	modzm_printf("  It is distributed under the GNU General Public License version 2 or\r\n");
	modzm_printf("    (at your option) any later version.\r\n");
	modzm_printf("  This software is offered as-is with no warranty or liability.\r\n");
	modzm_printf("  The core and dumb port are maintained by David Griffith.\r\n");
	modzm_printf("  Frotz's homepage is https://661.org/proj/if/frotz.\r\n\n");
	return;
} /* print_version */
