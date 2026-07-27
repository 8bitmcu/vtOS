/* See LICENSE for license details. */

#ifndef ST_TERM_H
#define ST_TERM_H

#include <stddef.h>
#include <stdint.h>

#define HISTSIZE 100

/* macros */
#define SMIN(a, b) ((a) < (b) ? (a) : (b))
#define SMAX(a, b) ((a) < (b) ? (b) : (a))
#define LEN(a) (sizeof(a) / sizeof(a)[0])
#define BETWEEN(x, a, b) (((long)(x)) >= (a) && ((long)(x)) <= (b))
#define DIVCEIL(n, d) (((n) + ((d) - 1)) / (d))
#define DEFAULT(a, b) (a) = (a) ? (a) : (b)
#define LIMIT(x, a, b) (x) = (long)(x) < (a) ? (a) : (long)(x) > (b) ? (b) : (x)
#define ATTRCMP(a, b)                                                          \
  ((a).mode != (b).mode || (a).fg != (b).fg || (a).bg != (b).bg)
#define TIMEDIFF(t1, t2)                                                       \
  ((t1.tv_sec - t2.tv_sec) * 1000 + (t1.tv_nsec - t2.tv_nsec) / 1E6)
#define MODBIT(x, set, bit) ((set) ? ((x) |= (bit)) : ((x) &= ~(bit)))

#define TRUECOLOR(r, g, b) (1 << 24 | (r) << 16 | (g) << 8 | (b))
#define IS_TRUECOL(x) (1 << 24 & (x))
#define TLINE(y)                                                               \
  ((y) < term.scr                                                              \
       ? term.hist[((y) + term.histi - term.scr + HISTSIZE + 1) % HISTSIZE]    \
       : term.line[(y) - term.scr])

enum glyph_attribute {
  ATTR_NULL = 0,
  ATTR_BOLD = 1 << 0,
  ATTR_FAINT = 1 << 1,
  ATTR_ITALIC = 1 << 2,
  ATTR_UNDERLINE = 1 << 3,
  ATTR_BLINK = 1 << 4,
  ATTR_REVERSE = 1 << 5,
  ATTR_INVISIBLE = 1 << 6,
  ATTR_STRUCK = 1 << 7,
  ATTR_WRAP = 1 << 8,
  ATTR_WIDE = 1 << 9,
  ATTR_WDUMMY = 1 << 10,
  ATTR_BOLD_FAINT = ATTR_BOLD | ATTR_FAINT,
};

enum cursor_style { CURSOR_BLOCK = 2, CURSOR_UNDERLINE = 4, CURSOR_BAR = 6 };

enum selection_mode { SEL_IDLE = 0, SEL_EMPTY = 1, SEL_READY = 2 };

enum selection_type { SEL_REGULAR = 1, SEL_RECTANGULAR = 2 };

enum selection_snap { SNAP_WORD = 1, SNAP_LINE = 2 };

typedef unsigned char uchar;
typedef unsigned int uint;
typedef unsigned long ulong;
typedef unsigned short ushort;

typedef uint_least32_t Rune;

#define Glyph Glyph_
typedef struct {
  Rune u;      /* character code */
  ushort mode; /* attribute flags */
  uint32_t fg; /* foreground  */
  uint32_t bg; /* background  */
} Glyph;

typedef Glyph *Line;

typedef union {
  int i;
  uint ui;
  float f;
  const void *v;
  const char *s;
} Arg;

void die(const char *, ...);
void redraw(void);
void draw(void);

void printscreen(const Arg *);
void printsel(const Arg *);
void sendbreak(const Arg *);
void toggleprinter(const Arg *);

int tattrset(int);
void tnew(int, int);
void tresize(int, int);
void tsetdirtattr(int);
void ttyhangup(void);
int ttynew(const char *, char *, const char *, char **);
size_t ttyread(void);
void ttyresize(int, int);
void ttywrite(const char *, size_t, int);

void resettitle(void);

void kscrolldown(const Arg *a);
void kscrollup(const Arg *a);

void selclear(void);
void selinit(void);
void selstart(int, int, int);
void selextend(int, int, int, int);
int selected(int, int);
char *getsel(void);

size_t utf8encode(Rune, char *);
size_t utf8decode(const char *, Rune *, size_t);

void *xmalloc(size_t);
void *xrealloc(void *, size_t);
char *xstrdup(const char *);


/* config.h globals */
extern char *utmp;
extern char *scroll;
extern char *stty_args;
extern char *vtiden;
extern wchar_t *worddelimiters;
extern int allowaltscreen;
extern int allowwindowops;
extern char *termname;
extern unsigned int defaultcs;

void tputc(Rune u);

extern unsigned int tabspaces;
extern unsigned int defaultfg; /* Usually white/light gray */
extern unsigned int defaultbg; /* Usually black */

/* Arbitrary sizes */
#define UTF_INVALID 0xFFFD
#define UTF_SIZ 4
#define ESC_BUF_SIZ (128 * UTF_SIZ)
#define ESC_ARG_SIZ 16
#define STR_BUF_SIZ ESC_BUF_SIZ
#define STR_ARG_SIZ ESC_ARG_SIZ

/* macros */
#define IS_SET(flag) ((term.mode & (flag)) != 0)
#define ISCONTROLC0(c) (BETWEEN(c, 0, 0x1f) || (c) == 0x7f)
#define ISCONTROLC1(c) (BETWEEN(c, 0x80, 0x9f))
#define ISCONTROL(c) (ISCONTROLC0(c) || ISCONTROLC1(c))
#define ISDELIM(u) (u && wcschr(worddelimiters, u))

enum term_mode {
  MODE_WRAP = 1 << 0,
  MODE_INSERT = 1 << 1,
  MODE_ALTSCREEN = 1 << 2,
  MODE_CRLF = 1 << 3,
  MODE_ECHO = 1 << 4,
  MODE_PRINT = 1 << 5,
  MODE_UTF8 = 1 << 6,
};

enum cursor_movement { CURSOR_SAVE, CURSOR_LOAD };

enum cursor_state {
  CURSOR_DEFAULT = 0,
  CURSOR_WRAPNEXT = 1,
  CURSOR_ORIGIN = 2
};

enum charset {
  CS_GRAPHIC0,
  CS_GRAPHIC1,
  CS_UK,
  CS_USA,
  CS_MULTI,
  CS_GER,
  CS_FIN
};

enum escape_state {
  ESC_START = 1,
  ESC_CSI = 2,
  ESC_STR = 4, /* DCS, OSC, PM, APC */
  ESC_ALTCHARSET = 8,
  ESC_STR_END = 16, /* a final string was encountered */
  ESC_TEST = 32,    /* Enter in test mode */
  ESC_UTF8 = 64,
};

typedef struct {
  Glyph attr; /* current char attributes */
  int x;
  int y;
  char state;
} TCursor;

typedef struct {
  int mode;
  int type;
  int snap;
  /*
   * Selection variables:
   * nb – normalized coordinates of the beginning of the selection
   * ne – normalized coordinates of the end of the selection
   * ob – original coordinates of the beginning of the selection
   * oe – original coordinates of the end of the selection
   */
  struct {
    int x, y;
  } nb, ne, ob, oe;

  int alt;
} Selection;

/* Internal representation of the screen */
typedef struct {
  int row;             /* nb row */
  int col;             /* nb col */
  Line *line;          /* screen */
  Line *alt;           /* alternate screen */
  Line hist[HISTSIZE]; /* history buffer */
  int histi;           /* history index */
  int scr;             /* scroll back */
  int *dirty;          /* dirtyness of lines */
  TCursor c;           /* cursor */
  int ocx;             /* old cursor col */
  int ocy;             /* old cursor row */
  int top;             /* top    scroll limit */
  int bot;             /* bottom scroll limit */
  int mode;            /* terminal mode flags */
  int esc;             /* escape state flags */
  char trantbl[4];     /* charset table translation */
  int charset;         /* current charset */
  int icharset;        /* selected charset for sequence */
  int *tabs;
  Rune lastc; /* last printed char outside of sequence, 0 if control */
  int cursor_style;
  int top_offset;
} Term;

/* CSI Escape sequence structs */
/* ESC '[' [[ [<priv>] <arg> [;]] <mode> [<mode>]] */
typedef struct {
  char buf[ESC_BUF_SIZ]; /* raw string */
  size_t len;            /* raw string length */
  char priv;
  int arg[ESC_ARG_SIZ];
  int narg; /* nb of args */
  char mode[2];
} CSIEscape;

/* STR Escape sequence structs */
/* ESC type [[ [<priv>] <arg> [;]] <mode>] ESC '\' */
typedef struct {
  char type;  /* ESC type ... */
  char *buf;  /* allocated raw string */
  size_t siz; /* allocation size */
  size_t len; /* raw string length */
  char *args[STR_ARG_SIZ];
  int narg; /* nb of args */
} STREscape;

extern Term term;

// Not static -- draw_bar_ansi() (fb.c) needs to save/restore these around
// its temporary takeover of the shared parser state, same reason
// utf8decode() above lost its static. Both are accumulation buffers for
// an in-progress escape sequence (CSIEscape for CSI, STREscape for OSC/
// DCS/etc.); processing the status bar's own text through tputc() while
// the main terminal is mid-sequence -- e.g. mid-way through one of
// modtui's own SGR color codes -- silently clobbers whatever had been
// accumulated so far, since both paths share the same buffer.
extern CSIEscape csiescseq;
extern STREscape strescseq;

#endif
