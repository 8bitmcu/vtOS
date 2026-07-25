#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vi.h"

#define NFTS		32

/* mapping filetypes to regular expression sets */
static struct ftmap {
	char ft[32];
	struct rset *rs;
} ftmap[NFTS];

static struct rset *syn_ftrs;
static int syn_ctx1, syn_ctx2;

/* truecolor slot table: SYN_FG/SYN_BG only carry 8 bits, not enough for a
 * 24-bit RGB value -- SYN_FGTC/SYN_BGTC (vi.h) mark that the fg/bg value
 * is an index into this table instead of a literal 256-color index. */
static int tc_slots[SYN_TCSLOTS][3];
static int tc_slot_count;

static struct rset *syn_find(char *ft)
{
	int i;
	for (i = 0; i < LEN(ftmap); i++)
		if (!strcmp(ft, ftmap[i].ft))
			return ftmap[i].rs;
	return NULL;
}

static struct rset *syn_make(char *name)
{
	char *pats[256] = {NULL};
	char *ft, *pat;
	int i;
	int n = 0;
	if (name == NULL || !name[0])
		return NULL;
	for (i = 0; !conf_highlight(i, &ft, NULL, &pat, NULL) && i < LEN(pats); i++)
		if (!strcmp(ft, name))
			pats[i] = pat;
	n = i;
	for (i = 0; i < LEN(ftmap); i++) {
		if (!ftmap[i].ft[0]) {
			strcpy(ftmap[i].ft, name);
			ftmap[i].rs = rset_make(n, pats, 0);
			return ftmap[i].rs;
		}
	}
	return NULL;
}

int syn_merge(int old, int new)
{
	/* Track which of old/new "wins" for fg and for bg (same condition the
	 * plain SYN_FG()/SYN_BG() extraction below already uses) so the
	 * SYN_FGTC/SYN_BGTC marker can travel with whichever value is chosen.
	 * A blanket "(old | new) & mask" (the way the other flag bits below
	 * are merged) would be wrong here: it would tag a plain 256-color
	 * value as truecolor whenever the *other* operand happened to carry
	 * the marker, even though that operand's own fg/bg didn't win. */
	int fgsrc = SYN_FGSET(new) ? new : old;
	int bgsrc = SYN_BGSET(new) ? new : old;
	int fg = SYN_FG(fgsrc);
	int bg = SYN_BG(bgsrc);
	if (SYN_RANK(old) > SYN_RANK(new))
		return syn_merge(new, old);
	return ((old | new) & SYN_FLG) | (fgsrc & SYN_FGTC) | (bgsrc & SYN_BGTC) |
		(bg << 8) | fg;
}

/* Register (or reuse, if already present) a truecolor slot for (r,g,b).
 * Returns the slot index (0..SYN_TCSLOTS-1), or -1 if the table is full --
 * callers must not tag a -1 result with SYN_FGTC/SYN_BGTC. */
int syn_tcslot(int r, int g, int b)
{
	int i;
	for (i = 0; i < tc_slot_count; i++)
		if (tc_slots[i][0] == r && tc_slots[i][1] == g && tc_slots[i][2] == b)
			return i;
	if (tc_slot_count >= SYN_TCSLOTS)
		return -1;
	tc_slots[tc_slot_count][0] = r;
	tc_slots[tc_slot_count][1] = g;
	tc_slots[tc_slot_count][2] = b;
	return tc_slot_count++;
}

void syn_tcget(int slot, int *r, int *g, int *b)
{
	if (slot < 0 || slot >= tc_slot_count) {
		*r = *g = *b = 0;
		return;
	}
	*r = tc_slots[slot][0];
	*g = tc_slots[slot][1];
	*b = tc_slots[slot][2];
}

void syn_context(int ctx1, int ctx2)
{
	syn_ctx1 = conf_hl(ctx1);
	syn_ctx2 = conf_hl(ctx2);
}

int *syn_highlight(char *ft, char *s)
{
	int subs[16 * 2];
	int n = uc_slen(s);
	int *att = malloc(n * sizeof(att[0]));
	int soff = 0;
	struct rset *rs = syn_find(ft);
	int flg = 0;
	int hl, j, i;
	if (!strcmp(ft, "___")) {
		for (i = 0; i < n; i++)
			att[i] = SYN_RV;
		return att;
	}
	for (i = 0; i < n; i++)
		att[i] = syn_ctx1;
	if (conf_hl('&')) {
		char *r = s;
		for (i = 0; i < n; i++) {
			if (mapch_get(r, NULL))
				att[i] = syn_merge(att[i], conf_hl('&'));
			r = uc_next(r);
		}
	}
	if (syn_ctx2) {
		for (i = 0; i < n; i++)
			att[i] = syn_merge(att[i], syn_ctx2);
	}
	if (!rs)
		rs = syn_make(ft);
	if (!rs)
		return att;
	while ((hl = rset_find(rs, s + soff, LEN(subs) / 2, subs, flg)) >= 0) {
		int sidx = uc_off(s, soff);
		int grp = 0;
		int cend = 1;
		int *catt;
		conf_highlight(hl, NULL, &catt, NULL, &grp);
		for (i = 0; i < LEN(subs) / 2; i++) {
			if (subs[i * 2] >= 0) {
				int o1 = subs[i * 2 + 0];
				int o2 = subs[i * 2 + 1];
				int beg = sidx + uc_off(s + soff, o1);
				int end = beg + uc_off(s + soff + o1, o2 - o1);
				for (j = beg; j < end; j++)
					att[j] = syn_merge(att[j], conf_hl(catt[i]));
				if (i == grp)
					cend = MAX(cend, o2);
			}
		}
		soff += cend;
		flg = RE_NOTBOL;
	}
	return att;
}

char *syn_filetype(char *path)
{
	int hl = rset_find(syn_ftrs, path, 0, NULL, 0);
	char *ft;
	if (!conf_filetype(hl, &ft, NULL))
		return ft;
	return "";
}

void syn_init(void)
{
	char *pats[128] = {NULL};
	char *pat;
	int i;
	for (i = 0; !conf_filetype(i, NULL, &pat) && i < LEN(pats); i++)
		pats[i] = pat;
	syn_ftrs = rset_make(i, pats, 0);
	// Resolve conf.c's compile-time truecolor overrides (conf_tcdefs[])
	// into real tc_slots[] entries -- must happen before the first
	// syn_highlight() call, which is why this lives in syn_init() rather
	// than somewhere lazier.
	conf_hltcinit();
}

void syn_done(void)
{
	int i;
	// ftmap[] is a file-scope static, persisting across repeated
	// syn_init()/syn_done() cycles in this port (unlike neatvi's
	// original one-shot-process design, where the OS reclaims
	// everything on exit and this never mattered). Confirmed the hard
	// way: leaving ftmap[i].rs pointing at freed memory and ftmap[i].ft
	// still holding the old filetype name meant a *second* vi session
	// editing a file with the same filetype had syn_find() hand
	// syn_highlight() a dangling rset pointer, crashing inside
	// regexec() with a NULL-pointer write. Must reset both fields, not
	// just free the pointer.
	for (i = 0; i < LEN(ftmap); i++) {
		if (ftmap[i].rs)
			rset_free(ftmap[i].rs);
		ftmap[i].rs = NULL;
		ftmap[i].ft[0] = '\0';
	}
	rset_free(syn_ftrs);
	syn_ftrs = NULL;
	// Same class of fix as ftmap[] above: tc_slot_count is a file-scope
	// static too, and would otherwise keep accumulating :hi-registered
	// truecolor slots across sessions instead of starting fresh, slowly
	// eating into the fixed SYN_TCSLOTS cap over many vi invocations.
	tc_slot_count = 0;
}
