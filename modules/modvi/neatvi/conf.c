#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "vi.h"
#include "conf.h"
#include "kmap.h"

int conf_dirmark(int idx, char **pat, int *ctx, int *dir, int *grp)
{
	if (idx < 0 || idx >= LEN(dirmarks))
		return 1;
	if (pat)
		*pat = dirmarks[idx].pat;
	if (ctx)
		*ctx = dirmarks[idx].ctx;
	if (dir)
		*dir = dirmarks[idx].dir;
	if (grp)
		*grp = dirmarks[idx].grp;
	return 0;
}

int conf_dircontext(int idx, char **pat, int *ctx)
{
	if (idx < 0 || idx >= LEN(dircontexts))
		return 1;
	if (pat)
		*pat = dircontexts[idx].pat;
	if (ctx)
		*ctx = dircontexts[idx].dir;
	return 0;
}

static int conf_hldefs[384] = {
	/* general text */
	['.'] = 0,		/* all text */
	['^'] = SYN_BGMK(11),	/* current line highlight (hll option) */
	['~'] = SYN_RV,		/* text in reverse direction */
	['&'] = 4,		/* mapped characters (mc command) */
	[','] = 0 | 252,	/* lines after EOF */
	/* for programming languages  */
	// Bumped to bold and/or the bright palette (8-15, rendered via the
	// 256-color escape in modvi_term.c's term_seqattr) across the board
	// here: plain, non-bold ANSI colors (esp. blue=4) read as too dim
	// against this device's black background. Hues are unchanged from
	// upstream, only made higher-contrast.
	['k'] = SYN_BD | 178,	/* general keywords -- muted cube gold (256-color) */
	['r'] = SYN_BD | 2,	/* control flow keywords */
	['o'] = SYN_BD | 3,	/* operators */
	['p'] = SYN_BD | 2,	/* preprocessor directives */
	['n'] = SYN_BD | 12,	/* include directives */
	['m'] = SYN_BD | 12,	/* imported packages */
	['t'] = SYN_BD | 3,	/* built-in types and values */
	['b'] = SYN_BD | 3,	/* built-in functions */
	['c'] = SYN_IT | 71,	/* comments -- muted cube green (256-color), softer than 10 */
	['d'] = SYN_BD | 12,	/* top-level definitions */
	['f'] = SYN_BD,		/* called functions */
	['0'] = 5,		/* numerical constants */
	['h'] = 5,		/* character constants */
	['s'] = 216,		/* string literals -- cube warm peach (256-color) */
	['v'] = SYN_BD | 3,	/* macros */
	['i'] = 0,		/* identifiers */
	['x'] = SYN_BD | 6,	/* identifier context */
	/* ex-mode and status line */
	['_'] = SYN_BGMK(0) | 7,	/* status line */
	['Z'] = SYN_BGMK(0) | 7,	/* status line of inactive windows */
	['M'] = SYN_BD,		/* status line mode word (NORMAL/INSERT) */
	['N'] = 2 | SYN_BD,		/* status line normal mode */
	['I'] = 4 | SYN_BD,		/* status line insert mode */
	['F'] = 3 | SYN_BD,		/* status line file name */
	['L'] = 5 | SYN_BD,		/* status line line number */
	['W'] = 1 | SYN_BD,		/* status line w/r flags */
	['K'] = 0,			/* keymap name */
	['C'] = 5,			/* status line column number */
	['T'] = 0,			/* status line line count */
	['X'] = SYN_BGMK(0) | 7 | SYN_BD,	/* ex-mode output lines */
	[':'] = SYN_BGMK(0) | 7,		/* ex-mode prompts */
	['!'] = SYN_BGMK(0) | 7 | SYN_BD,	/* ex-mode warnings */
	['Q'] = SYN_BGMK(0) | 7 | SYN_BD,	/* quick leap header */
	['U'] = 0,		/* quick leap list */
	['D'] = 0,		/* quick leap item dirname */
	['B'] = 3 | SYN_BD,	/* quick leap item basename */
	['1'] = 1 | SYN_BD,	/* quick leap item identifier */
	['2'] = 4,		/* quick leap item description */
	/* diff file type */
	[SX('D')] = SYN_BD,		/* diff header */
	[SX('@')] = 6,		/* diff hunk information line */
	[SX('-')] = 1,		/* diff deleted lines  */
	[SX('+')] = 2,		/* diff added lines */
	[SX('=')] = 0,		/* diff preserved lines */
	/* file listings */
	[SX('d')] = 8 | SYN_BD,	/* path dirname */
	[SX('b')] = 1 | SYN_BD,	/* path basename */
	[SX('n')] = 2,		/* path line number */
	[SX('s')] = 6,		/* path symbol name */
	[SX('l')] = 8,		/* the rest of the line */
	[SX('c')] = 4,		/* comments */
	/* messages */
	[SM('h')] = 6 | SYN_BD,	/* mail headers */
	[SM('s')] = 4 | SYN_BD,	/* subject */
	[SM('f')] = 2 | SYN_BD,	/* from */
	[SM('t')] = 5 | SYN_BD,	/* to */
	[SM('c')] = 5 | SYN_BD,	/* cc */
	[SM('r')] = 2 | SYN_IT,	/* replied text */
	[SM('l')] = SYN_BGMK(3) | SYN_BD,	/* label */
	/* neatmail listing */
	[SM('N')] = SYN_BD | SYN_FGMK(0) | SYN_BGMK(6),	/* new mails */
	[SM('A')] = SYN_BD | SYN_FGMK(0) | SYN_BGMK(5),	/* high priority */
	[SM('B')] = SYN_BD | SYN_FGMK(0) | SYN_BGMK(3),	/* priority level 2 */
	[SM('C')] = SYN_BD | SYN_FGMK(0) | SYN_BGMK(3),	/* priority level 3 */
	[SM('D')] = 8,		/* low priority */
	[SM('F')] = SYN_BD | SYN_BGMK(7),	/* flagged */
	[SM('I')] = 7 | SYN_IT,	/* ignored lines */
	[SM('X')] = SYN_BD,	/* commands */
	[SM('S')] = 6 | SYN_BD,	/* status flags */
	[SM('0')] = 12,		/* ignored zeros of message numbers */
	[SM('1')] = 4 | SYN_BD,	/* message numbers */
	[SM('M')] = 3,		/* mbox paths */
	[SM('R')] = 5,		/* message from */
	[SM('U')] = 5,		/* message subject */
};

/* Compile-time truecolor themes for conf_hldefs[] above. conf_hldefs
 * itself can't hold a #RRGGBB-style literal directly: it's a plain static
 * initializer, and resolving a hex triple into a real slot means calling
 * syn_tcslot() (see syn.c), which is a function call and so isn't a valid
 * constant expression in C. Instead, each theme below lists overrides by
 * conf_hldefs index (the same 'x'/SX()/SM() ids used above) and an
 * 0xRRGGBB literal (which *is* a compile-time constant -- only the
 * syn_tcslot() call itself is deferred). -1 for fgrgb/bgrgb means "no
 * truecolor override for that channel".
 *
 * Example: a true-orange keyword highlight, bold, background unchanged:
 *   {'k', SYN_BD, 0xff8000, -1},
 */
struct conf_tcdef {
	int idx;
	int extra;
	long fgrgb;
	long bgrgb;
};

/* Gruvbox Dark (morhetz/gruvbox) */
static const struct conf_tcdef gruvbox_tcdefs[] = {
	{'k', SYN_BD, 0xfe8019, -1},		/* general keywords -- orange */
	{'r', SYN_BD, 0xfb4934, -1},		/* control flow -- bright red */
	{'o', 0,      0xfe8019, -1},		/* operators -- orange */
	{'p', SYN_BD, 0x8ec07c, -1},		/* preprocessor directives -- bright aqua */
	{'n', SYN_BD, 0x8ec07c, -1},		/* include directives -- bright aqua */
	{'m', SYN_BD, 0x8ec07c, -1},		/* imported packages -- bright aqua */
	{'t', SYN_BD, 0xfabd2f, -1},		/* built-in types -- bright yellow */
	{'b', 0,      0xfabd2f, -1},		/* built-in functions -- bright yellow */
	{'c', 0,      0x928374, -1},		/* comments -- gray */
	{'d', SYN_BD, 0xb8bb26, -1},		/* top-level definitions -- bright green */
	{'f', SYN_BD, 0xb8bb26, -1},		/* called functions -- bright green */
	{'0', 0,      0xd3869b, -1},		/* numerical constants -- bright purple */
	{'h', 0,      0xd3869b, -1},		/* character constants -- bright purple */
	{'s', 0,      0xb8bb26, -1},		/* string literals -- bright green */
	{'v', SYN_BD, 0xfabd2f, -1},		/* macros -- bright yellow */
	{'x', SYN_BD, 0x83a598, -1},		/* identifier context -- bright blue */
};

/* Solarized Dark (Ethan Schoonover, ethanschoonover.com/solarized) */
static const struct conf_tcdef solarized_tcdefs[] = {
	{'k', SYN_BD, 0x859900, -1},		/* general keywords -- green (Statement) */
	{'r', SYN_BD, 0x859900, -1},		/* control flow -- green (Statement) */
	{'p', SYN_BD, 0xcb4b16, -1},		/* preprocessor directives -- orange (PreProc) */
	{'n', SYN_BD, 0xcb4b16, -1},		/* include directives -- orange (PreProc) */
	{'m', SYN_BD, 0xcb4b16, -1},		/* imported packages -- orange (PreProc) */
	{'t', SYN_BD, 0xb58900, -1},		/* built-in types -- yellow (Type) */
	{'b', 0,      0xb58900, -1},		/* built-in functions -- yellow (Type) */
	{'c', 0,      0x586e75, -1},		/* comments -- base01 */
	{'d', SYN_BD, 0x268bd2, -1},		/* top-level definitions -- blue (Function) */
	{'f', SYN_BD, 0x268bd2, -1},		/* called functions -- blue (Function) */
	{'0', 0,      0x2aa198, -1},		/* numerical constants -- cyan (Constant) */
	{'h', 0,      0x2aa198, -1},		/* character constants -- cyan (Constant) */
	{'s', 0,      0x2aa198, -1},		/* string literals -- cyan (Constant) */
	{'v', SYN_BD, 0xcb4b16, -1},		/* macros -- orange (PreProc) */
	{'x', SYN_BD, 0x6c71c4, -1},		/* identifier context -- violet */
};

/* Catppuccin Mocha (catppuccin.com/palette */
static const struct conf_tcdef catppuccin_tcdefs[] = {
	{'k', SYN_BD, 0xcba6f7, -1},		/* general keywords -- mauve */
	{'r', SYN_BD, 0xcba6f7, -1},		/* control flow -- mauve */
	{'o', 0,      0x89dceb, -1},		/* operators -- sky */
	{'p', SYN_BD, 0xf5c2e7, -1},		/* preprocessor directives -- pink */
	{'n', SYN_BD, 0xf5c2e7, -1},		/* include directives -- pink */
	{'m', SYN_BD, 0xf5c2e7, -1},		/* imported packages -- pink */
	{'t', SYN_BD, 0xf9e2af, -1},		/* built-in types -- yellow */
	{'b', 0,      0x89b4fa, -1},		/* built-in functions -- blue */
	{'c', 0,      0x9399b2, -1},		/* comments -- overlay2 */
	{'d', SYN_BD, 0x89b4fa, -1},		/* top-level definitions -- blue */
	{'f', SYN_BD, 0x89b4fa, -1},		/* called functions -- blue */
	{'0', 0,      0xfab387, -1},		/* numerical constants -- peach */
	{'h', 0,      0xfab387, -1},		/* character constants -- peach */
	{'s', 0,      0xa6e3a1, -1},		/* string literals -- green */
	{'v', SYN_BD, 0xf9e2af, -1},		/* macros -- yellow */
	{'x', SYN_BD, 0xb4befe, -1},		/* identifier context -- lavender */
};

/* Tokyo Night "Storm" (folke/tokyonight.nvim's default variant) */
static const struct conf_tcdef tokyonight_tcdefs[] = {
	{'k', SYN_BD, 0x9d7cd8, -1},		/* general keywords -- purple (@keyword) */
	{'r', SYN_BD, 0x9d7cd8, -1},		/* control flow -- purple (@keyword) */
	{'o', 0,      0x7dcfff, -1},		/* operators -- cyan */
	{'p', SYN_BD, 0x7dcfff, -1},		/* preprocessor directives -- cyan */
	{'n', SYN_BD, 0x7dcfff, -1},		/* include directives -- cyan */
	{'m', SYN_BD, 0x7dcfff, -1},		/* imported packages -- cyan */
	{'t', SYN_BD, 0xe0af68, -1},		/* built-in types -- yellow */
	{'b', 0,      0x7aa2f7, -1},		/* built-in functions -- blue */
	{'c', 0,      0x565f89, -1},		/* comments -- dedicated comment tone */
	{'d', SYN_BD, 0xbb9af7, -1},		/* top-level definitions -- magenta (@keyword.function) */
	{'f', SYN_BD, 0x7aa2f7, -1},		/* called functions -- blue */
	{'0', 0,      0xff9e64, -1},		/* numerical constants -- orange */
	{'h', 0,      0xff9e64, -1},		/* character constants -- orange */
	{'s', 0,      0x9ece6a, -1},		/* string literals -- green */
	{'v', SYN_BD, 0xe0af68, -1},		/* macros -- yellow */
	{'x', SYN_BD, 0x1abc9c, -1},		/* identifier context -- teal */
};

/* Nord (nordtheme.com, Arctic Ice Studio)  */
static const struct conf_tcdef nord_tcdefs[] = {
	{'k', SYN_BD, 0x81a1c1, -1},		/* general keywords -- nord9 */
	{'r', SYN_BD, 0x81a1c1, -1},		/* control flow -- nord9 */
	{'o', 0,      0x81a1c1, -1},		/* operators -- nord9 */
	{'p', SYN_BD, 0x5e81ac, -1},		/* preprocessor directives -- nord10 */
	{'n', SYN_BD, 0x5e81ac, -1},		/* include directives -- nord10 */
	{'m', SYN_BD, 0x5e81ac, -1},		/* imported packages -- nord10 */
	{'t', SYN_BD, 0x8fbcbb, -1},		/* built-in types -- nord7 */
	{'b', 0,      0x88c0d0, -1},		/* built-in functions -- nord8 */
	{'c', 0,      0x4c566a, -1},		/* comments -- nord3 */
	{'d', SYN_BD, 0x88c0d0, -1},		/* top-level definitions -- nord8 */
	{'f', SYN_BD, 0x88c0d0, -1},		/* called functions -- nord8 */
	{'0', 0,      0xb48ead, -1},		/* numerical constants -- nord15 */
	{'h', 0,      0xb48ead, -1},		/* character constants -- nord15 */
	{'s', 0,      0xa3be8c, -1},		/* string literals -- nord14 */
	{'v', SYN_BD, 0xd08770, -1},		/* macros -- nord12 */
	{'x', SYN_BD, 0xd8dee9, -1},		/* identifier context -- nord4 */
};

static const struct conf_theme {
	const char *name;
	const struct conf_tcdef *defs;
	int ndefs;
} conf_themes[] = {
	{"gruvbox", gruvbox_tcdefs, LEN(gruvbox_tcdefs)},
	{"solarized", solarized_tcdefs, LEN(solarized_tcdefs)},
	{"catppuccin", catppuccin_tcdefs, LEN(catppuccin_tcdefs)},
	{"tokyonight", tokyonight_tcdefs, LEN(tokyonight_tcdefs)},
	{"nord", nord_tcdefs, LEN(nord_tcdefs)},
};

#define CONF_THEME_DEFAULT "gruvbox"

/* Every index ANY theme above might touch -- reset to neutral (no color,
 * no bold/italic) before applying a theme's own overrides, so switching
 * from a theme that styles more entries (e.g. gruvbox's 'o') to one that
 * doesn't (solarized) can't leave a stale value behind from whichever
 * theme was active before. */
static const int conf_theme_idxs[] = {
	'.', 'k', 'r', 'o', 'p', 'n', 'm', 't', 'b', 'c', 'd', 'f', '0', 'h', 's', 'v', 'x'
};

static char conf_theme_name[32] = CONF_THEME_DEFAULT;

char *conf_theme_current(void)
{
	return conf_theme_name;
}

int conf_theme_apply(char *name)
{
	const struct conf_theme *th = NULL;
	int i;
	for (i = 0; i < LEN(conf_themes); i++) {
		if (!strcmp(conf_themes[i].name, name)) {
			th = &conf_themes[i];
			break;
		}
	}
	if (!th)
		return 1;
	for (i = 0; i < LEN(conf_theme_idxs); i++)
		conf_hldefs[conf_theme_idxs[i]] = 0;
	syn_tcreset();
	for (i = 0; i < th->ndefs; i++) {
		int mode = th->defs[i].extra;
		if (th->defs[i].fgrgb >= 0) {
			long rgb = th->defs[i].fgrgb;
			int slot = syn_tcslot((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
			if (slot >= 0)
				mode |= SYN_FGTC | SYN_FGMK(slot);
		}
		if (th->defs[i].bgrgb >= 0) {
			long rgb = th->defs[i].bgrgb;
			int slot = syn_tcslot((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
			if (slot >= 0)
				mode |= SYN_BGTC | SYN_BGMK(slot);
		}
		if (th->defs[i].idx >= 0 && th->defs[i].idx < LEN(conf_hldefs))
			conf_hldefs[th->defs[i].idx] = mode;
	}
	snprintf(conf_theme_name, sizeof(conf_theme_name), "%s", name);
	return 0;
}

/* Persists the active theme name to flash -- deliberately NOT called from
 * conf_theme_load() below. */

/* Called once from syn_init(), before any highlighting runs (and so
 * before ex_init() sources /flash/.virc -- see vimod_main()'s setenv()
 * comment): applies CONF_THEME_DEFAULT as a starting point. That's the
 * *only* persistence story now -- a "theme <name>" line in .virc is
 * the official way to make a theme stick across restarts, sourced after
 * this and so free to override it. No dedicated flash file for this
 * (there used to be one, /flash/.vitheme; redundant once the rc file
 * could do the same job with less bespoke code). */
void conf_theme_load(void)
{
	conf_theme_apply(CONF_THEME_DEFAULT);
}

int conf_hlnum(char *name)
{
	if (name[0] == 'x' && isprint((unsigned char) name[1]))
		return SX(name[1]);
	if (name[0] == 'm' && isprint((unsigned char) name[1]))
		return SM(name[1]);
	return name[0];
}

int conf_hl(int id)
{
	return id >= 0 && id < LEN(conf_hldefs) ? conf_hldefs[id] : 0;
}

void conf_hlset(int id, int att)
{
	if (id >= 0 && id < LEN(conf_hldefs))
		conf_hldefs[id] = att;
}

int conf_highlight(int idx, char **ft, int **att, char **pat, int *end)
{
	if (idx < 0 || idx >= LEN(highlights))
		return 1;
	if (ft)
		*ft = highlights[idx].ft;
	if (att)
		*att = highlights[idx].att;
	if (pat)
		*pat = highlights[idx].pat;
	if (end)
		*end = highlights[idx].end;
	return 0;
}

int conf_filetype(int idx, char **ft, char **pat)
{
	if (idx < 0 || idx >= LEN(filetypes))
		return 1;
	if (ft)
		*ft = filetypes[idx].ft;
	if (pat)
		*pat = filetypes[idx].pat;
	return 0;
}

int conf_mode(void)
{
	return MKFILE_MODE;
}

char *kmap_map(int id, int key)
{
	static char cs[4];
	cs[0] = key;
	return id < LEN(kmaps) && kmaps[id][key] ? kmaps[id][key] : cs;
}

void kmap_def(int id, int key, char *def)
{
	static char defs[512];
	static int defs_pos;
	int len = strlen(def) + 1;
	if (id >= 0 && id < LEN(kmaps) && key >= 0 && key < 256) {
		if (len < sizeof(defs) - defs_pos) {
			memcpy(defs + defs_pos, def, len);
			kmaps[id][key] = defs + defs_pos;
			defs_pos += len;
		}
	}
}

int kmap_find(char *name)
{
	int i;
	for (i = 0; i < LEN(kmaps); i++)
		if (name && kmaps[i][0] && !strcmp(name, kmaps[i][0]))
			return i;
	if (!kmaps[LEN(kmaps) - 1][0]) {
		kmap_def(LEN(kmaps) - 1, 0, name);
		return LEN(kmaps) - 1;
	}
	return -1;
}

char *conf_digraph(int c1, int c2)
{
	int i;
	for (i = 0; i < LEN(digraphs); i++)
		if (digraphs[i][0][0] == c1 && digraphs[i][0][1] == c2)
			return digraphs[i][1];
	return NULL;
}

char *conf_definition(char *ft)
{
	int i;
	for (i = 0; i < LEN(filetypes); i++)
		if (!strcmp(ft, filetypes[i].ft) && filetypes[i].def)
			return filetypes[i].def;
	return "^%s\\>";
}

char *conf_section(char *ft)
{
	int i;
	for (i = 0; i < LEN(filetypes); i++)
		if (!strcmp(ft, filetypes[i].ft) && filetypes[i].sec)
			return filetypes[i].sec;
	return "^\\{";
}

char *conf_ecmd(void)
{
	return ECMD;
}

/* character placeholders */
static struct mapch {
	char s[6];	/* the source character */
	char d[16];	/* the placeholder */
	int wid;	/* the width of the placeholder */
} mapch[64];

static int mapch_cnt;
static EXT_RAM_BSS_ATTR char mapch_map[256];

static void mapch_init(void)
{
	static int called;
	if (!called) {
		called = 1;
		mapch_def("‌", "-", 1);
		mapch_def("‍", "-", 1);
		mapch_def("ً", "ـً", 1);
		mapch_def("ٌ", "ـٌ", 1);
		mapch_def("ٍ", "ـٍ", 1),
		mapch_def("َ", "ـَ", 1);
		mapch_def("ُ", "ـُ", 1);
		mapch_def("ِ", "ـِ", 1);
		mapch_def("ّ", "ـّ", 1);
		mapch_def("ْ", "ـْ", 1);
		mapch_def("ٓ", "ـٓ", 1);
		mapch_def("ٔ", "ـٔ", 1);
		mapch_def("ٕ", "ـٕ", 1);
		mapch_def("ٰ", "ـٰ", 1);
	}
}

void mapch_def(char *s, char *d, int wid)
{
	int i;
	mapch_init();
	for (i = 0; i < mapch_cnt; i++)
		if (!mapch[i].s[0] || uc_code(s) == uc_code(mapch[i].s))
			break;
	if (i < LEN(mapch)) {
		struct mapch *mc = &mapch[i];
		int c = (unsigned char) s[0];
		snprintf(mc->s, sizeof(mc->s), "%s", d[0] ? s : "");
		snprintf(mc->d, sizeof(mc->d), "%s", d);
		mc->wid = wid >= 0 ? wid : ren_wid(d);
		mapch_map[c] = 1;
		mapch_cnt = MAX(mapch_cnt, i + 1);
	}
}

char *mapch_get(char *s, int *wid)
{
	int c = (unsigned char) s[0];
	int i;
	if (!mapch_cnt)
		mapch_init();
	if (mapch_map[c]) {
		for (i = 0; i < mapch_cnt; i++) {
			struct mapch *mc = &mapch[i];
			if (s[0] == mc->s[0] && uc_code(s) == uc_code(mc->s)) {
				if (wid)
					*wid = mc->wid;
				return mc->d;
			}
		}
	}
	if (wid)
		*wid = 1;
	if (uc_isbell(s))
		return "�";
	return NULL;
}
