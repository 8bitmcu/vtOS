#!/usr/bin/env python3

# This utility will convert a monospaced font for usage with vtOS
#
# Supports TrueType/OpenType (.ttf/.otf) fonts, rendered and quantized via
# PIL, as well as BDF (.bdf) bitmap fonts, whose glyph bitmaps are read
# directly from the file (no rendering/quantization needed since they're
# already 1bpp).
#
# PCF (.pcf) fonts are not parsed directly (it's a binary format with
# several internal tables). If you have a .pcf font, convert it to .bdf
# first, e.g. with `pcf2bdf` (Debian/Ubuntu: `apt install xfonts-utils` or
# `fonttools`), then point this script at the resulting .bdf file.

import sys
from PIL import Image, ImageFont, ImageDraw
import argparse

BPP = 1
CHARACTERS = ''.join(chr(c) for c in range(32, 127))
UNICODE_SYM = ''.join(chr(c) for c in (0x2500, 0x2502, 0x250c, 0x2510, 0x2514, 0x2518, 0x251C, 0x2524, 0x252C, 0x2534, 0x253C))

UNICODE_ICONS = ''.join(chr(c) for c in range(0xE000, 0xE196))

# The specific Siji icons actually used (status bar: clock, speaker,
# wifi, battery, bluetooth, mem) -- see --statusbar-icons. Non-contiguous
# within UNICODE_ICONS, so unlike that full range this needs the explicit
# WIDE_CHARS list mechanism (same as WIDE_SYM below), not WIDE_FIRST/
# WIDE_COUNT. Deliberately just these 6, not the full ~400-icon set:
# icon glyphs only render through fb.c's WIDE_FONT path (see
# VT.set_icon_font()), which only the vtOS terminal engine implements --
# they don't render at all in a plain MicroPython REPL, so there was no
# reason to ship glyphs for icons nothing outside the status bar uses.
STATUSBAR_ICONS = ''.join(chr(c) for c in (0xE015, 0xE152, 0xE048, 0xE033, 0xE00B, 0xE020))

# Chess Symbols (U+2654-265F) -- rendered double-width (see --wide-unicode),
# matching st.c's st_wcwidth() marking these codepoints ATTR_WIDE.
WIDE_SYM = ''.join(chr(c) for c in range(0x2654, 0x2660))

def font_kind(path):
    lower = path.lower()
    if lower.endswith('.bdf'):
        return 'bdf'
    if lower.endswith('.pcf'):
        raise ValueError(
            f"'{path}' looks like a .pcf font, which isn't supported directly. "
            f"Convert it to .bdf first (e.g. `pcf2bdf {path} > out.bdf`) and pass "
            "the .bdf file instead."
        )
    return 'ttf'


# ---------------------------------------------------------------------------
# TrueType / OpenType handling (rendered via PIL, then quantized to bpp)
# ---------------------------------------------------------------------------

def encode_chars_ttf(characters, font, char_width, char_height, y_offset, bpp, sample_p, remap, palette):
    wide = (char_width * bpp + 7) // 8  # bytes per row (matches decoder's wide)
    row_stride = wide * 8               # bits per row after padding
    bitstring = ''
    for char in characters:
        char_im = Image.new('L', (char_width, char_height), 0)
        ImageDraw.Draw(char_im).text((0, y_offset), char, font=font, fill=255)
        char_im = char_im.quantize(palette=sample_p)
        char_im = char_im.point(remap)
        char_im.putpalette(palette)
        for y in range(char_im.height):
            row_bits = ''
            for x in range(char_im.width):
                color = char_im.getpixel((x, y))
                row_bits += ''.join(
                    '1' if (color & (1 << bit - 1)) else '0'
                    for bit in range(bpp, 0, -1)
                )
            bitstring += row_bits + '0' * (row_stride - len(row_bits))
    return bitstring


# ---------------------------------------------------------------------------
# BDF handling (glyph bitmaps read directly from the file)
# ---------------------------------------------------------------------------

def parse_bdf(path):
    """Parse a BDF file into (font_bbox, glyphs).

    font_bbox is (width, height, xoff, yoff) from FONTBOUNDINGBOX.
    glyphs is a dict: codepoint -> {bbw, bbh, bbxoff, bbyoff, bytes_per_row, rows}
    where 'rows' is a list of bbh integers, each the big-endian value of that
    row's bitmap bytes (as printed in the BDF BITMAP section).
    """
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        lines = f.read().splitlines()

    font_bbx = None
    glyphs = {}
    i = 0
    n = len(lines)
    while i < n:
        line = lines[i].strip()
        if line.startswith('FONTBOUNDINGBOX'):
            parts = line.split()
            font_bbx = tuple(int(x) for x in parts[1:5])
            i += 1
            continue
        if line.startswith('STARTCHAR'):
            i += 1
            encoding = None
            bbx = None
            bitmap_rows = []
            in_bitmap = False
            while i < n and not lines[i].strip().startswith('ENDCHAR'):
                l = lines[i].strip()
                if l.startswith('ENCODING'):
                    encoding = int(l.split()[1])
                elif l.startswith('BBX'):
                    bbx = tuple(int(x) for x in l.split()[1:5])
                elif l == 'BITMAP':
                    in_bitmap = True
                elif in_bitmap and l:
                    bitmap_rows.append(l)
                i += 1
            if encoding is not None and encoding >= 0 and bbx is not None:
                bbw, bbh, bbxoff, bbyoff = bbx
                bytes_per_row = (bbw + 7) // 8
                rows = [int(r, 16) if r else 0 for r in bitmap_rows[:bbh]]
                while len(rows) < bbh:
                    rows.append(0)
                glyphs[encoding] = {
                    'bbw': bbw, 'bbh': bbh,
                    'bbxoff': bbxoff, 'bbyoff': bbyoff,
                    'bytes_per_row': bytes_per_row,
                    'rows': rows,
                }
            i += 1  # skip past ENDCHAR
            continue
        i += 1

    if font_bbx is None:
        raise ValueError(f'No FONTBOUNDINGBOX found in {path}')
    return font_bbx, glyphs


def encode_chars_bdf(characters, glyphs, char_width, char_height, canvas_top, canvas_left, bpp, src_path):
    wide = (char_width * bpp + 7) // 8
    row_stride = wide * 8
    bitstring = ''
    warned = set()
    for char in characters:
        codepoint = ord(char)
        glyph = glyphs.get(codepoint)
        grid = [[0] * char_width for _ in range(char_height)]
        if glyph is None:
            if codepoint not in warned:
                print(f'Warning: {src_path} has no glyph for U+{codepoint:04X} ({char!r}); leaving blank',
                      file=sys.stderr)
                warned.add(codepoint)
        else:
            bbw, bbh = glyph['bbw'], glyph['bbh']
            bbxoff, bbyoff = glyph['bbxoff'], glyph['bbyoff']
            bytes_per_row = glyph['bytes_per_row']
            glyph_top_rel = bbyoff + bbh          # top edge of glyph, relative to baseline
            row_off = canvas_top - glyph_top_rel  # rows from canvas top down to glyph top
            col_off = bbxoff - canvas_left
            for ry, rowval in enumerate(glyph['rows']):
                cy = row_off + ry
                if cy < 0 or cy >= char_height:
                    continue
                for rx in range(bbw):
                    bit = (rowval >> (bytes_per_row * 8 - 1 - rx)) & 1
                    if not bit:
                        continue
                    cx = col_off + rx
                    if 0 <= cx < char_width:
                        grid[cy][cx] = 1
        for y in range(char_height):
            # 0 = ink/foreground, 1 = background -- see matching comment in
            # encode_chars_ttf().
            row_bits = ''.join('0' if grid[y][x] else '1' for x in range(char_width))
            bitstring += row_bits + '0' * (row_stride - len(row_bits))
    return bitstring


# ---------------------------------------------------------------------------
# Shared output helper
# ---------------------------------------------------------------------------

def emit_block(varname, bitstring):
    print(f'_{varname} =\\')
    print("    b'", end='')
    for i in range(0, len(bitstring), 8):
        if i and i % (16 * 8) == 0:
            print("'\\\n    b'", end='')
        print(f'\\x{int(bitstring[i:i+8], 2):02x}', end='')
    print("'\\\n    b''")
    print(f'\n{varname} = memoryview(_{varname})')


def main():
    parser = argparse.ArgumentParser(prog='fontconvert')
    parser.add_argument('font_file', help='Path to the regular font file (.ttf/.otf or .bdf)')
    parser.add_argument('font_size', type=int,
                         help='Font size to render (pixel size; ignored for .bdf fonts, which are already bitmaps)')
    parser.add_argument('-b', '--bold', help='Optional path to the bold font file', default=None)
    parser.add_argument('-i', '--italic', help='Optional path to the italic font file', default=None)
    parser.add_argument('-bi', '--bold_italic', help='Optional path to the bold+italic font file', default=None)
    parser.add_argument('-u', '--unicode', action='store_true', help='Include unicode box drawing characters')
    parser.add_argument('--wide-unicode', action='store_true',
                        help='Include double-width unicode characters (currently Chess Symbols, '
                             'U+2654-265F) as a separate WIDE_FONT block, rendered at 2x the '
                             'normal character width -- see ATTR_WIDE handling in st.c/fb.c')
    parser.add_argument('--icons', action='store_true', help='iconic font only')
    parser.add_argument('--statusbar-icons', action='store_true',
                        help='With --icons --wide-unicode: use the curated 6-icon '
                             'STATUSBAR_ICONS set instead of the full ~400-icon range')
    parser.add_argument('--force-height', type=int, help='Force a specific pixel height boundary', default=None)
    parser.add_argument('--force-width', type=int, help='Force a specific pixel width boundary', default=None)
    args = parser.parse_args()

    bpp = BPP
    try:
        kind = font_kind(args.font_file)

        # Sanity-check that any secondary faces match the regular face's format.
        for opt_path, opt_name in ((args.bold, '--bold'), (args.italic, '--italic'), (args.bold_italic, '--bold_italic')):
            if opt_path is not None and font_kind(opt_path) != kind:
                parser.error(f'{opt_name} font must be the same format as the regular font ({kind})')
    except ValueError as e:
        parser.error(str(e))

    if kind == 'bdf':
        # -------------------------------------------------------------
        # BDF path: glyph bitmaps are read directly, no rendering needed.
        # -------------------------------------------------------------
        font_bbx, reg_glyphs = parse_bdf(args.font_file)
        bbx_w, bbx_h, bbx_xoff, bbx_yoff = font_bbx

        char_height = args.force_height if args.force_height is not None else bbx_h
        char_width = args.force_width if args.force_width is not None else bbx_w
        canvas_top = bbx_yoff + bbx_h
        canvas_left = bbx_xoff

        if args.icons and args.wide_unicode:
            # Combined mode: emit the icon range as a STANDALONE iconic-
            # font module -- WIDE_FONT/WIDE_FIRST/WIDE_COUNT/WIDE_WIDTH
            # only, no FIRST/LAST/HEIGHT/WIDTH/REGULAR/BOLD. This module
            # is never used as a main font (never passed to
            # env.update_font()); it's loaded independently via
            # VT.set_icon_font() (see vt_module.c) as a supplemental
            # double-width glyph source for ATTR_WIDE codepoints,
            # alongside whatever the main text font currently is.
            #
            # For a fixed-size bitmap icon font (e.g. Siji) that can't be
            # rescaled like a TTF -- its glyphs are pre-drawn at one
            # native size -- WIDE_WIDTH/WIDE_HEIGHT tell fb.c the icon's
            # actual glyph dimensions so it can center them within
            # whatever cell size the paired main font uses (2x its width,
            # its own height), padding or clipping evenly on all sides as
            # needed. A close size match still looks best, but an exact
            # one is no longer required. Don't pass --force-width/height
            # here unless you actually want to distort the icon font's
            # native size.
            if args.statusbar_icons:
                # Curated, non-contiguous subset -- needs the explicit
                # WIDE_CHARS list (same mechanism WIDE_SYM/chess uses),
                # not WIDE_FIRST/WIDE_COUNT (contiguous-range only).
                icon_set = STATUSBAR_ICONS
                codepoints = ', '.join(hex(ord(c)) for c in icon_set)
                print(f'WIDE_CHARS = ({codepoints},)')
            else:
                icon_set = UNICODE_ICONS
                print(f'WIDE_FIRST = {hex(ord(icon_set[0]))}')
                print(f'WIDE_COUNT = {len(icon_set)}')
            print(f'WIDE_WIDTH = {char_width}')
            print(f'WIDE_HEIGHT = {char_height}')
            print()
            emit_block('WIDE_FONT', encode_chars_bdf(
                icon_set, reg_glyphs, char_width, char_height, canvas_top, canvas_left, bpp, args.font_file))
            return

        print(f'FIRST = {hex(ord(CHARACTERS[0]))}')
        print(f'LAST = {hex(ord(CHARACTERS[-1]))}')
        print(f'HEIGHT = {char_height}')
        print(f'WIDTH = {char_width}')
        print()

        if args.icons:
            # UNICODE_ICONS is one contiguous range, so a base codepoint +
            # count is enough for fb.c to compute "is this char an icon,
            # and if so which index" directly -- no per-glyph codepoint
            # table needed the way UNICODE_FONT/WIDE_FONT require, since
            # those cover disjoint/non-contiguous codepoints.
            print(f'ICON_FIRST = {hex(ord(UNICODE_ICONS[0]))}')
            print(f'ICON_COUNT = {len(UNICODE_ICONS)}')
            print()
            emit_block('ICONS', encode_chars_bdf(
                UNICODE_ICONS, reg_glyphs, char_width, char_height, canvas_top, canvas_left, bpp, args.font_file))
        else:

            emit_block('REGULAR', encode_chars_bdf(
                CHARACTERS, reg_glyphs, char_width, char_height, canvas_top, canvas_left, bpp, args.font_file))

            if args.bold:
                _, bold_glyphs = parse_bdf(args.bold)
                print()
                emit_block('BOLD', encode_chars_bdf(
                    CHARACTERS, bold_glyphs, char_width, char_height, canvas_top, canvas_left, bpp, args.bold))
            else:
                print('\nBOLD = REGULAR')

            if args.italic:
                _, italic_glyphs = parse_bdf(args.italic)
                print()
                emit_block('ITALIC', encode_chars_bdf(
                    CHARACTERS, italic_glyphs, char_width, char_height, canvas_top, canvas_left, bpp, args.italic))
            else:
                print('\nITALIC = REGULAR')

            if args.bold_italic:
                _, bi_glyphs = parse_bdf(args.bold_italic)
                print()
                emit_block('BOLD_ITALIC', encode_chars_bdf(
                    CHARACTERS, bi_glyphs, char_width, char_height, canvas_top, canvas_left, bpp, args.bold_italic))
            else:
                print('\nBOLD_ITALIC = BOLD')

            if args.unicode:
                codepoints = ', '.join(hex(ord(c)) for c in UNICODE_SYM)
                print()
                print(f'UNICODE_CHARS = ({codepoints},)')
                print()
                emit_block('UNICODE_FONT', encode_chars_bdf(
                    UNICODE_SYM, reg_glyphs, char_width, char_height, canvas_top, canvas_left, bpp, args.font_file))

            if args.wide_unicode:
                wide_width = char_width * 2
                codepoints = ', '.join(hex(ord(c)) for c in WIDE_SYM)
                print()
                print(f'WIDE_CHARS = ({codepoints},)')
                print(f'WIDE_WIDTH = {wide_width}')
                print()
                emit_block('WIDE_FONT', encode_chars_bdf(
                    WIDE_SYM, reg_glyphs, wide_width, char_height, canvas_top, canvas_left, bpp, args.font_file))

        return

    # -------------------------------------------------------------------
    # TrueType / OpenType path (original behavior, unchanged)
    # -------------------------------------------------------------------

    # Always lock grid metrics to the regular font face
    reg_font = ImageFont.truetype(args.font_file, args.font_size)

    _, top, _, bottom = reg_font.getbbox(CHARACTERS)

    char_height = args.force_height if args.force_height is not None else (bottom - top)
    y_offset = -top

    # Use forced width if provided, otherwise fall back to automated calculation
    char_width = args.force_width if args.force_width is not None else max(reg_font.getbbox(c)[2] for c in 'MW@#')

    try:
        _adaptive = Image.Palette.ADAPTIVE
    except AttributeError:
        _adaptive = Image.ADAPTIVE

    sample = Image.new('L', (char_width * len(CHARACTERS), char_height), 0)
    ImageDraw.Draw(sample).text((0, y_offset), CHARACTERS, font=reg_font, fill=255)
    sample_p = sample.convert(mode='P', palette=_adaptive, colors=1 << bpp)
    palette = sample_p.getpalette()

    # Ensure darkest entry = index 0
    n = 1 << bpp
    lum = [sum(palette[i*3:(i+1)*3]) for i in range(n)]
    order = sorted(range(n), key=lambda i: lum[i])
    remap = list(range(256))
    if order != list(range(n)):
        remap = [0] * 256
        for new, old in enumerate(order):
            remap[old] = new
        new_pal = palette[:]
        for new, old in enumerate(order):
            new_pal[new*3:(new+1)*3] = palette[old*3:(old+1)*3]
        palette = new_pal

    # Metadata Global Headers
    print(f'FIRST = {hex(ord(CHARACTERS[0]))}')
    print(f'LAST = {hex(ord(CHARACTERS[-1]))}')
    print(f'HEIGHT = {char_height}')
    print(f'WIDTH = {char_width}')
    print()

    # Emit REGULAR Font Block
    emit_block('REGULAR', encode_chars_ttf(CHARACTERS, reg_font, char_width, char_height, y_offset, bpp, sample_p, remap, palette))

    # Emit BOLD Font Block
    if args.bold:
        bold_font = ImageFont.truetype(args.bold, args.font_size)
        print()
        emit_block('BOLD', encode_chars_ttf(CHARACTERS, bold_font, char_width, char_height, y_offset, bpp, sample_p, remap, palette))
    else:
        print('\nBOLD = REGULAR')

    # Emit ITALIC Font Block
    if args.italic:
        italic_font = ImageFont.truetype(args.italic, args.font_size)
        print()
        emit_block('ITALIC', encode_chars_ttf(CHARACTERS, italic_font, char_width, char_height, y_offset, bpp, sample_p, remap, palette))
    else:
        print('\nITALIC = REGULAR')

    # Emit BOLD_ITALIC Font Block
    if args.bold_italic:
        bi_font = ImageFont.truetype(args.bold_italic, args.font_size)
        print()
        emit_block('BOLD_ITALIC', encode_chars_ttf(CHARACTERS, bi_font, char_width, char_height, y_offset, bpp, sample_p, remap, palette))
    else:
        # Cascades back elegantly to Bold (which itself cascades back to Regular if missing)
        print('\nBOLD_ITALIC = BOLD')

    # Emit UNICODE Block (Using Regular Font Metrics)
    if args.unicode:
        codepoints = ', '.join(hex(ord(c)) for c in UNICODE_SYM)
        print()
        print(f'UNICODE_CHARS = ({codepoints},)')
        print()
        emit_block('UNICODE_FONT', encode_chars_ttf(UNICODE_SYM, reg_font, char_width, char_height, y_offset, bpp, sample_p, remap, palette))

    # Emit WIDE Block (double-width characters, e.g. Chess Symbols)
    if args.wide_unicode:
        wide_width = char_width * 2
        codepoints = ', '.join(hex(ord(c)) for c in WIDE_SYM)
        print()
        print(f'WIDE_CHARS = ({codepoints},)')
        print(f'WIDE_WIDTH = {wide_width}')
        print()
        emit_block('WIDE_FONT', encode_chars_ttf(WIDE_SYM, reg_font, wide_width, char_height, y_offset, bpp, sample_p, remap, palette))


if __name__ == '__main__':
    main()
