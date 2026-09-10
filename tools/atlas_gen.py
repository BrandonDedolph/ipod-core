#!/usr/bin/env python3
"""
Pre-rasterize a TTF font at a fixed pixel size into a C header
suitable for static linking. Output is two arrays + a struct
literal; see core/apps/ui/atlas.h for the C-side type definitions.

Run via tools/atlas_gen.sh which sets up the venv with Pillow.

REQUIRES PYTHON 3.12+. The glyph-label f-string below embeds a backslash inside
its expression part, which only became legal in 3.12 (PEP 701); on 3.11 this
file is a SyntaxError at import, before any argument is parsed, so the failure
looks like the tool is broken rather than the interpreter being too old. There
is an explicit version check below so the error says which it is.

All file writes here are explicitly UTF-8. The generated header carries the
literal glyph characters in its comments (smart quotes, chevrons, the middle
dot), so writing with the platform default encoding meant this tool died with a
UnicodeEncodeError under LC_ALL=C — which is exactly the environment a CI
container or a cron job runs in.

Usage:
    atlas_gen.py <ttf-path> <pixel-size> <c-symbol-name> <output.h>

Example:
    atlas_gen.py tools/fonts-src/Nunito-Regular.ttf 13 NUNITO_REGULAR_13 \\
        core/apps/ui/atlas/nunito_regular_13.h
"""
import argparse
import os
import sys

if sys.version_info < (3, 12):
    raise SystemExit(
        f"atlas_gen.py needs Python 3.12+ (found "
        f"{sys.version_info.major}.{sys.version_info.minor}): it uses a "
        f"backslash inside an f-string expression, legal only since PEP 701.")

from PIL import Image, ImageDraw, ImageFont

# The one definition of daylight between two glyphs, shared with the solver
# and the judge. Read its docstring before touching any spacing number here.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from daylight import glyph_rows, core_span, band_daylight   # noqa: E402

# The generated headers declare `uint16_t data_offset`, so the concatenated
# glyph bitmap cannot exceed 64 KiB — past that the offsets wrap and every
# glyph after the wrap point renders as a slice of some other glyph's bitmap.
# Nothing checked this; a large face at a large pixel size would have produced
# a header that compiles cleanly and draws garbage.
DATA_OFFSET_MAX = 0xFFFF

# Advance fixed-point scale. MUST match ATLAS_ADV_SHIFT/ATLAS_ADV_ONE in
# core/ui/atlas.h — the generated headers are consumed directly by the device
# renderer, so a mismatch here silently rescales all text spacing.
ADV_SHIFT = 6
ADV_ONE = 1 << ADV_SHIFT
ADVANCE_MAX = 0xFFFF                 # the uint16_t field it is emitted into

# Kerning. Stored per PAIR in 1/32 px in an int8_t, so the representable range
# is +/-3.97px (worst real pair is bold-17 "LT" at -2.21px) at 0.03px
# resolution. A pair is only emitted when |kern| reaches KERN_MIN_PX — below
# that it cannot change a rounded pixel position often enough to be worth the
# ROM.
KERN_SHIFT = 5
KERN_ONE = 1 << KERN_SHIFT
# 0.25px. Measured trade at regular-13 across real album/artist strings:
#   threshold   ASCII pairs   worst string-width error
#     0.2500        640            0.66px
#     0.1250       1116            0.66px
#     0.0625       1394            0.41px
# Every pair big enough to change a visible gap is >= 0.25px, so the tighter
# thresholds buy only sub-pixel centring accuracy for roughly double the ROM
# (~+50KB across the seven atlases). The residual shows up as string width,
# never as the uneven letter gaps this was fixing.
KERN_MIN_PX = 0.25
KERN_ADJ_MIN, KERN_ADJ_MAX = -128, 127

# Letter-spacing added between glyphs, in PIXELS, per pixel size.
#
# Nunito's antialiased ink is wider than its advances at these sizes — measured
# mean side bearing is negative everywhere we ship (9px -0.36, 11px -0.13,
# 13px -0.21), i.e. glyphs touch before any spacing logic is involved. These
# values restore roughly half a pixel of daylight between letters. Tune here;
# the device just adds the number.
# Keyed by (is_bold, px) — NOT by size alone. Bold is fitted tighter than
# regular at the same size (design RSB 0.040em vs 0.046em) on top of carrying
# heavier stems, so keying on size under-tracks bold 11/13.
#
# Values chosen against a measured criterion rather than by eye: over ~600
# adjacent letter pairs from 53 real title/artist/menu strings, put fewer than
# ~3% of pairs' >=50%-alpha glyph CORES in contact, which lands the soft
# (any-alpha) mean gap around +0.4..+0.7px. Targeting a +1.0px mean instead
# would need 1.2-1.7px of tracking and reads as deliberately letterspaced.
TRACKING_PX = {
    # SOLVED, not guessed: tools/text_metrics.py --target 1.45 picks the value
    # that puts each atlas's mean visual gap at 1.45px, measuring real ink out
    # of the baked bitmaps with the device's own pen arithmetic.
    #
    # They differ wildly because the FACES differ: at 0.91px of tracking
    # everywhere, the measured mean gap ran from 0.78px (bold 11 — cramped) to
    # 1.82px (bold 17 — loose). That spread is why the text read as tight in
    # some places and airy in others at the same time. Equalising the gap
    # matters more than the absolute number.
    (False,  9): 0.92, (False, 11): 0.59, (False, 12): 0.48,
    (True,  12): 0.73, (True,  13): 0.64, (True,  18): 0.09,
}
TRACKING_DEFAULT = 0.7

# Optical kerning: re-solve every LETTER pair from the baked bitmaps so they
# all render at the same ink gap. See optical_kern(). The target is in whole
# pixels because the device pen steps in whole pixels.
OPTICAL_KERN = os.environ.get("CORE_OPTICAL_KERN", "1") != "0"
OPTICAL_TARGET_PX = int(os.environ.get("CORE_OPTICAL_TARGET", "2"))
# Word gap, in px of ink-to-ink daylight across a space. Once every LETTER gap
# is pinned to OPTICAL_TARGET_PX, the space stops being self-correcting: the
# faces' designed space advances gave word/letter ratios from 1.5 (regular 9 —
# "Taylor Swift" read as one word) to 3.0 (bold 13). Pinning this too is what
# makes word spacing consistent between faces rather than an accident of each
# one's design width.
# float, not int: the same variable overrides the solved word gap in area
# mode (solved_for), whose values are quarter pixels.
OPTICAL_WORD_PX = float(os.environ.get("CORE_OPTICAL_WORD", "5"))
# Spacing criterion. "min" (the e190093 rule) puts every letter pair at the
# same clearance on its single tightest row; "area" puts every pair at the same
# mean DAYLIGHT over the x-height band, with a floor on the tightest row so
# cores never touch.
#
# Why area: a tail or hook (Nunito l, f, t) is one row. Under the min rule that
# row sets the whole pair's gap, so 'ff', 'ld', 'ht', 'ft' rendered with a
# visible hole ("Shuf fle", "Chil dren") while 'oo' sat tight — measured over
# 13 real UI strings (tools/ui_strings.txt, tools/text_metrics.py --strings)
# the per-pair daylight had a coefficient of variation of 0.28-0.35, and the
# same solver on Tahoma gave 0.25-0.32, so it was the criterion, not the face.
OPTICAL_MODE = os.environ.get("CORE_OPTICAL_MODE", "area")    # "min" | "area"
# Floor: the tightest-row clearance that may never be violated, in px, and
# the alpha at which a pixel counts as ink for that test (1 = any ink,
# 128 = core). ~37% of the pixels in these atlases are sub-25%-alpha fringe;
# floors measured on any-ink keep the fringe apart (a 2%-alpha pixel blocked
# every kern touching an f), which is where the unavoidable 'ld' looseness
# came from. This is deliberately a THRESHOLD while the texture measure
# (daylight.py) integrates: touching is decided by two dark-ish pixels
# sitting side by side, and a coverage floor waved that through — see
# daylight.band_daylight.
OPTICAL_FLOOR_PX = int(os.environ.get("CORE_OPTICAL_FLOOR", "1"))
OPTICAL_FLOOR_ALPHA = int(os.environ.get("CORE_OPTICAL_FLOOR_ALPHA", "64"))
# SOLVED per atlas by tools/optical_solve.py, not chosen by eye: (area px,
# word px). Area is the daylight target — coverage-integrated white between
# the cores, tools/daylight.py — within a quarter pixel of the face's own
# 'n' counter at that size (the classic even-texture rule) that minimises
# the rhythm sd over the real strings; word is the midpoint of the range
# where every word gap clears every intra-word gap by a whole pixel and the
# space advance stays under half an em. See the solver's docstring for the
# argument and the table it printed.
# Output of: tools/optical_solve.py  (floor: alpha >= 64 clears 1px; strings:
# tools/ui_strings.txt; 2026-09-10, coverage measure). Re-run it after
# changing the face, the sizes, the tracking table, the floor, the measure,
# or the strings — never edit by hand. The numbers are NOT comparable with
# the any-alpha solve's (2.10/5.25 ... 2.65/9.00): a 2.1 there was ~2.8 of
# white; text_metrics.py --edge any judges any atlas in the old units.
OPTICAL_SOLVED = {
    (False,  9): (2.20, 6.25),   # NUNITO_REGULAR_9: n counter 1.97, sd 0.35 CV 0.154, word range 5.25-7.0
    (False, 11): (2.75, 8.25),   # NUNITO_REGULAR_11: n counter 2.51, sd 0.35 CV 0.125, word range 7.75-8.5
    (False, 12): (2.70, 8.25),   # NUNITO_REGULAR_12: n counter 2.50, sd 0.42 CV 0.151, word range 7.5-8.75
    (True , 12): (1.95, 7.50),   # NUNITO_BOLD_12: n counter 1.75, sd 0.43 CV 0.200, word range 6.25-8.5
    (True , 13): (1.95, 8.75),   # NUNITO_BOLD_13: n counter 1.74, sd 0.45 CV 0.203, word range 7.25-10.0
    (True , 18): (3.50, 10.00),  # NUNITO_BOLD_18: n counter 3.25, sd 0.44 CV 0.120, word range 7.25-12.5
}
OPTICAL_AREA_DEFAULT = 2.5
OPTICAL_WORD_DEFAULT = 7.0


def fit_space_advance(glyphs, glyph_data, kerns, tracking, target_px,
                      band=None):
    """Space advance (26.6) that puts the median word gap at target_px.

    Measured the way the device draws it: A's step into the space (tracking
    applies), then the space's step out (it does not), then the ink-to-ink
    clearance between A and B over the rows they share.

    With `band` (x-height rows, ascender-relative) the gap is the mean
    coverage-integrated daylight over the band rows both glyphs have a core
    on (daylight.band_daylight) — the same measure the area criterion pins
    letter pairs to, so the word/letter ratio means what it says — sampled
    over Upper->lower, lower->Upper and lower->lower pairs. The first
    version sampled only Upper->lower on the tightest row, which left 'd M'
    ("Fleetwood Mac") and 'e S' unconstrained. Without `band` it is the
    tightest-row any-alpha clearance over Upper->lower, as before.
    """
    kmap = {(l, r): v for l, r, v in kerns}
    idx = {chr(0x20 + i): i for i in range(95)}
    ups = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    los = "abcdefghijklmnopqrstuvwxyz"
    ext = {}
    for ch in ups + los:
        gi = idx[ch]
        ox, oy, w, h, adv, off = glyphs[gi]
        if w and h:
            bmp = glyph_data[off:off + w * h]
            ext[ch] = (glyph_rows(bmp, w, h, oy) if band is not None
                       else row_extents_bitmap(bmp, w, h, oy), ox, adv)
    sp_adv = glyphs[idx[" "]][4]
    combos = [(ups, los)] if band is None else [(ups, los), (los, ups), (los, los)]
    gaps = []
    for A, B in combos:
        for a in A:
            if a not in ext:
                continue
            ra, oxa, adva = ext[a]
            for b in B:
                if b not in ext:
                    continue
                rb, oxb, _ = ext[b]
                s1 = (adva + tracking + kmap.get((idx[a], idx[" "]), 0) * 2
                      + ADV_ONE // 2) >> ADV_SHIFT
                s2 = (sp_adv + kmap.get((idx[" "], idx[b]), 0) * 2
                      + ADV_ONE // 2) >> ADV_SHIFT
                if band is not None:
                    d = band_daylight(ra, oxa, rb, s1 + s2 + oxb, band)
                    if d is not None:
                        gaps.append(d)
                    continue
                sh = set(ra) & set(rb)
                if not sh:
                    continue
                gaps.append(min((s1 + s2 + oxb + rb[y][0]) - (oxa + ra[y][1]) - 1
                                for y in sh))
    if not gaps:
        return sp_adv
    gaps.sort()
    median = gaps[len(gaps) // 2]
    return max(ADV_ONE, sp_adv + int(round((target_px - median) * ADV_ONE)))

PRINTABLE = range(0x20, 0x7F)  # 0x20..0x7E inclusive — 95 glyphs

# Non-ASCII glyphs appended AFTER the 95 ASCII glyphs, at fixed indices 95, 96,
# 97 … . The firmware maps a Unicode codepoint to its index via the generated
# core/ui/atlas/glyphmap.h (built from this same list, so the two never drift),
# plus three legacy private single-byte codes 0x01/0x02/0x03 for ‹ › · that
# predate UTF-8 decoding (see core/ui/text.c glyph_index()). Order is
# load-bearing: it fixes each glyph's index, which glyphmap.h then records.
#
# Coverage: the three Linen chevron/dot glyphs, the smart-punctuation that shows
# up in real music metadata (curly quotes, en/em dash, ellipsis), and the whole
# printable Latin-1 Supplement (À-ÿ and friends) so Western artist/album names
# with accents render true instead of being stripped.
EXTRAS = [
    (0x2039, "‹"),   # index 95: ‹  single left  angle quote  (also code 0x01)
    (0x203A, "›"),   # index 96: ›  single right angle quote  (also code 0x02)
    (0x00B7, "·"),   # index 97: ·  middle dot                (also code 0x03)
    # smart punctuation common in tags/filenames
    (0x2018, "‘"), (0x2019, "’"),          # ‘ ’  single curly quotes
    (0x201C, "“"), (0x201D, "”"),          # “ ”  double curly quotes
    (0x2013, "–"), (0x2014, "—"),          # – —  en / em dash
    (0x2026, "…"),                          # …    horizontal ellipsis
]
# Printable Latin-1 Supplement (0xA1..0xFF), minus 0xAD SOFT HYPHEN (non-printing)
# and 0xB7 MIDDLE DOT (already an extra above — avoid a duplicate glyph/index).
EXTRAS += [(cp, chr(cp)) for cp in range(0xA1, 0x100) if cp not in (0xAD, 0x00B7)]


def write_glyphmap(out_dir: str) -> None:
    """Emit core/ui/atlas/glyphmap.h: the codepoint -> glyph-index table the
    firmware binary-searches for any non-ASCII char. Font-independent (every
    atlas shares the same glyph order), so it is rewritten identically on each
    run. Sorted by codepoint for bsearch."""
    base = 95  # first EXTRAS index (ASCII 0x20..0x7E occupy 0..94)
    rows = sorted((cp, base + i) for i, (cp, _ch) in enumerate(EXTRAS))
    out = []
    out.append("// glyphmap.h — auto-generated by tools/atlas_gen.py.")
    out.append("// Codepoint -> atlas glyph index for non-ASCII glyphs, sorted")
    out.append("// by codepoint. Edit the generator's EXTRAS, not this file.")
    out.append("")
    out.append("#include <stdint.h>")
    out.append("")
    out.append("typedef struct { uint16_t cp; uint8_t idx; } atlas_cpmap_t;")
    out.append("")
    out.append(f"static const atlas_cpmap_t ATLAS_CPMAP[{len(rows)}] = {{")
    for cp, idx in rows:
        try:
            ch = chr(cp)
        except ValueError:
            ch = "?"
        out.append(f"    {{ 0x{cp:04X}, {idx:3d} }},   // '{ch}'")
    out.append("};")
    out.append(f"#define ATLAS_CPMAP_N {len(rows)}")
    out.append("")
    path = os.path.join(out_dir, "glyphmap.h")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(out))
    sys.stderr.write(f"wrote {path}\n")


def row_extents_bitmap(bmp, w, h, oy, thresh=1):
    """Per-row (first,last) ink columns, keyed by ASCENDER-RELATIVE row.

    Keying by the raw bitmap row is wrong and was the first version's bug: two
    glyphs of different heights start at different distances below the
    ascender, so raw row 0 of 'L' and raw row 0 of 'a' are not the same line on
    screen. Comparing them made the solver measure 'L' against the wrong part
    of its neighbour and pull lowercase letters straight into L's foot.
    """
    out = {}
    for row in range(h):
        base = row * w
        first = last = None
        for col in range(w):
            if bmp[base + col] >= thresh:
                if first is None:
                    first = col
                last = col
        if first is not None:
            out[oy + row] = (first, last)
    return out


LETTERS = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
# Steps sampled per pair past the floor. Daylight gains exactly one pixel per
# step once the fringes clear, so anything past the table is extrapolated.
PAIR_TABLE_STEPS = 8


def pair_table(glyphs, glyph_data, band, floor_px=None, floor_alpha=None):
    """Everything the area rule needs per ordered letter pair that does NOT
    depend on the target, built once per atlas: the smallest whole-pixel
    step (A's origin to B's) the no-touch floor allows, and the mean band
    daylight (daylight.band_daylight) at that step and the next
    PAIR_TABLE_STEPS-1. The solver sweeps dozens of targets over one atlas;
    the bitmap work is the same for all of them, so it is done here once.

    Returns {(a, b): (floor_step, [daylight at floor_step, +1, +2, ...])}
    keyed by the characters. Pairs that share no core row at any height are
    absent: they are never adjacent and keep the font's own kern.

    The floor is the tightest row at floor_alpha ink clearing floor_px — the
    same test as before — and the table starts no lower than the step at
    which the >=CORE_ALPHA cores stop overlapping, so its first entry is a
    real daylight and not a collision count.
    """
    floor_px = OPTICAL_FLOOR_PX if floor_px is None else floor_px
    floor_alpha = OPTICAL_FLOOR_ALPHA if floor_alpha is None else floor_alpha
    idx = {chr(0x20 + i): i for i in range(95)}
    rows, extf, core, meta = {}, {}, {}, {}
    for ch in LETTERS:
        ox, oy, w, h, adv, off = glyphs[idx[ch]]
        if w == 0 or h == 0:
            continue
        bmp = glyph_data[off:off + w * h]
        rows[ch] = glyph_rows(bmp, w, h, oy)
        extf[ch] = row_extents_bitmap(bmp, w, h, oy, floor_alpha)
        core[ch] = {y: c for y, c in
                    ((y, core_span(r)) for y, r in rows[ch].items())
                    if c is not None}
        meta[ch] = ox
    table = {}
    for a in rows:
        ra, oxa = rows[a], meta[a]
        for b in rows:
            rb, oxb = rows[b], meta[b]
            cshared = set(core[a]) & set(core[b])
            if not cshared:
                continue                      # no core row in common
            # cores just apart: the table's first entry is a real daylight
            mc = (min(core[b][y][0] - core[a][y][1] for y in cshared)
                  + oxb - oxa)
            s0 = 1 - mc
            fshared = set(extf[a]) & set(extf[b])
            if fshared:
                # closest approach at step s is s + mf - 1; keep it >= floor
                mf = (min(extf[b][y][0] - extf[a][y][1] for y in fshared)
                      + oxb - oxa)
                s0 = max(s0, floor_px + 1 - mf)
            dl = []
            for s in range(s0, s0 + PAIR_TABLE_STEPS):
                d = band_daylight(ra, oxa, rb, s + oxb, band)
                if d is None:                 # no core rows in the band:
                    d = band_daylight(ra, oxa, rb, s + oxb, None)   # all rows
                dl.append(d)
            table[(a, b)] = (s0, dl)
    return table


def optical_kern(glyphs, glyph_data, tracking, target_px, band=None,
                 mode=None, area_px=None, floor_px=None, floor_alpha=None,
                 table=None):
    """Per-pair corrections that put every letter pair at the SAME ink gap.

    The font's own kerning is a design for print at large sizes; at 9-12px on
    this panel it is zero for most pairs and the RASTERISED gaps end up all
    over the place. Measured on the shipped regular-9 atlas: 'or', 'ol', 'ou'
    and 'ON' sat at 3px while 'rd', 'lo', 'ta' and 'wo' sat at 0px — a 3px
    spread at a 9px face, which is what reads as "some letters cramped, some
    loose" no matter how carefully the mean is tuned.

    So we measure the actual baked ink and solve for the gap instead. The
    device pen steps in whole pixels (core/ui/text.c pen_step), so for glyphs A
    then B the tightest-row gap is:

        gap = step + min_over_shared_rows(left_B(y) - right_A(y)) + oxB - oxA - 1

    which inverts to an exact integer step for a target gap, and from there to
    the kern value that produces it. Pairs already on target get no entry.

    mode "min" pins that tightest-row gap to target_px. mode "area" pins the
    MEAN daylight over the x-height `band` rows to area_px — daylight as
    tools/daylight.py defines it: the white area between the two glyphs'
    cores, antialiasing fringe counted by what it leaves white, not by its
    presence. The first area solve measured from any pixel with any alpha,
    and a 9px 'e' whose curve ends in a 17-alpha pixel got a letter placed a
    full pixel further out than the eye could justify; see that module. The
    step chosen is the one whose daylight is nearest area_px, never below
    the no-touch floor (tightest row at floor_alpha ink clears floor_px).
    Row extents are compared row against row throughout — the extreme
    column of a glyph over all its rows is not its edge at any one height.
    `table` is pair_table()'s output if the caller already has it (the
    solver builds it once and sweeps); otherwise it is built here.
    Parameters default to the module settings so atlas_gen.sh and the
    solver call the same function.

    ASCII letters only: digits and punctuation have deliberate design widths
    (a comma should not be spaced like an 'o'), and forcing them to a uniform
    optical gap looks mechanical.
    """
    mode = mode or OPTICAL_MODE
    area_px = OPTICAL_AREA_DEFAULT if area_px is None else area_px
    idx = {chr(0x20 + i): i for i in range(95)}
    adv_of = {ch: glyphs[idx[ch]][4] for ch in LETTERS}

    def emit(out, a, b, step):
        adj64 = step * ADV_ONE - adv_of[a] - tracking
        adj = int(round(adj64 / 2.0))          # 1/64 -> 1/32
        if adj != 0:
            out.append((idx[a], idx[b],
                        max(KERN_ADJ_MIN, min(KERN_ADJ_MAX, adj))))

    out = []
    if mode == "area":
        if table is None:
            table = pair_table(glyphs, glyph_data, band, floor_px, floor_alpha)
        for (a, b), (s0, dl) in table.items():
            if area_px > dl[-1]:
                # past the table: one pixel of daylight per pixel of step
                step = s0 + len(dl) - 1 + int(round(area_px - dl[-1]))
            else:
                # nearest daylight to the target; a tie goes to the tighter
                step = s0 + min(range(len(dl)),
                                key=lambda k: (abs(dl[k] - area_px), k))
            emit(out, a, b, step)
        out.sort()
        return out

    # mode "min": the e190093 rule, kept so the history regenerates.
    ext = {}
    for ch in LETTERS:
        ox, oy, w, h, adv, off = glyphs[idx[ch]]
        if w == 0 or h == 0:
            continue
        ext[ch] = (row_extents_bitmap(glyph_data[off:off + w * h], w, h, oy), ox)
    for a in ext:
        ra, oxa = ext[a]
        for b in ext:
            rb, oxb = ext[b]
            shared = set(ra) & set(rb)
            if not shared:
                continue
            m = min(rb[y][0] - ra[y][1] for y in shared) + oxb - oxa
            emit(out, a, b, target_px + 1 - m)
    return out


def kern_pairs(font, chars):
    """Kerning for every ordered pair of the glyphs we ship, in 1/32 px.

    Measured as len(ab) - len(a) - len(b) through the SAME getlength() that
    produces the advances, with Raqm layout doing the font's own shaping. That
    is deliberate: reading GPOS separately would be a second source of truth
    that could disagree with the advances we baked.

    Returns [(left_idx, right_idx, adj32)] sorted by (left, right) so the
    device can binary-search it.
    """
    single = {c: font.getlength(c) for c in chars}
    out = []
    for li, a in enumerate(chars):
        for ri, b in enumerate(chars):
            k = font.getlength(a + b) - single[a] - single[b]
            if abs(k) < KERN_MIN_PX:
                continue
            adj = int(round(k * KERN_ONE))
            if adj == 0:
                continue
            if adj < KERN_ADJ_MIN or adj > KERN_ADJ_MAX:
                # CLAMP, loudly. Refusing outright was the first behaviour and
                # it blocks evaluating any face with wider kerning than Nunito
                # (Inter has a '<'+em-dash pair at -4.16px). Clamping costs
                # 0.2px on the handful of pairs past the field's +/-3.97px, and
                # the warning keeps it from being a silent truncation — which
                # is the thing actually worth preventing. Widen the field to
                # int16 if a face ever needs it in bulk.
                sys.stderr.write(
                    f"warning: kern {a!r}{b!r} = {k:+.3f}px exceeds the int8 "
                    f"1/32px field; clamped\n")
                adj = max(KERN_ADJ_MIN, min(KERN_ADJ_MAX, adj))
            out.append((li, ri, adj))
    out.sort(key=lambda e: (e[0], e[1]))
    return out


def raster(ttf_path: str, px_size: int):
    """Rasterise every glyph we ship at px_size, exactly as the header will
    carry it. Returns a dict the emitter and tools/optical_solve.py share, so
    the solver evaluates the same bitmaps the device draws."""
    # Raqm applies the font's kerning in getlength(); the basic layout engine
    # does not, and would silently emit an empty kern table.
    font = ImageFont.truetype(ttf_path, px_size,
                              layout_engine=ImageFont.Layout.RAQM)
    ascent, descent = font.getmetrics()

    glyph_data = bytearray()
    glyphs = []   # list of (offset_x, offset_y, w, h, advance, data_offset)

    def add_glyph(ch):
        bbox = font.getbbox(ch)  # (left, top, right, bottom) — top<0 = above baseline
        # 26.6 fixed point (1/64 px), NOT whole pixels. Rounding each glyph's
        # advance to an integer here is what made letter spacing look uneven:
        # at 9-13px a true advance is routinely a half pixel, so neighbouring
        # gaps in one word disagreed by ~1px and the error accumulated along
        # the string. The device pen carries the fraction (core/ui/atlas.h,
        # ATLAS_ADV_*). Costs nothing: the glyph struct was 7 bytes padded to
        # 8 and is still 8.
        advance = int(round(font.getlength(ch) * ADV_ONE))

        if not bbox or (bbox[2] - bbox[0]) <= 0 or (bbox[3] - bbox[1]) <= 0:
            # Whitespace / no ink. Record an empty glyph with just an advance.
            glyphs.append((0, 0, 0, 0, advance, len(glyph_data)))
            return

        offset_x = int(bbox[0])
        offset_y = int(bbox[1])  # negative for ascenders
        w = int(bbox[2] - bbox[0])
        h = int(bbox[3] - bbox[1])

        # Render onto a fresh L-mode image at glyph-bbox size. Origin
        # for ImageDraw.text is the pen baseline; we shift by -bbox to
        # land the visible ink at (0, 0) of the bitmap.
        img = Image.new("L", (w, h), 0)
        draw = ImageDraw.Draw(img)
        draw.text((-offset_x, -offset_y), ch, font=font, fill=255)
        bytes_ = img.tobytes()
        assert len(bytes_) == w * h, \
            f"unexpected byte count: {len(bytes_)} vs {w}*{h}"

        data_offset = len(glyph_data)
        glyph_data.extend(bytes_)
        glyphs.append((offset_x, offset_y, w, h, advance, data_offset))

    for codepoint in PRINTABLE:
        add_glyph(chr(codepoint))
    for _cp, ch in EXTRAS:
        add_glyph(ch)

    chars = [chr(cp) for cp in PRINTABLE] + [ch for _cp, ch in EXTRAS]
    xb = font.getbbox("x")
    band = (ascent - (xb[3] - xb[1]), ascent) if xb else None
    return dict(font=font, ascent=ascent, descent=descent, glyphs=glyphs,
                glyph_data=glyph_data, chars=chars, band=band,
                kerns=kern_pairs(font, chars))


def tracking_for(symbol: str, px_size: int) -> int:
    """Tracking in 26.6 for this atlas: the env override, else the table."""
    _ovr = os.environ.get("CORE_TRACKING_PX")
    _bold = "BOLD" in symbol.upper()
    _tpx = (float(_ovr) if _ovr
            else TRACKING_PX.get((_bold, px_size), TRACKING_DEFAULT))
    return int(round(_tpx * ADV_ONE))


def solved_for(symbol: str, px_size: int):
    """(area px, word px) for this atlas: env overrides, else the solved
    table, else the defaults (and a warning, because a default here is a
    guess the solver has not blessed)."""
    _bold = "BOLD" in symbol.upper()
    area, word = OPTICAL_SOLVED.get((_bold, px_size), (None, None))
    if area is None and OPTICAL_MODE == "area":
        sys.stderr.write(f"warning: {symbol}: no solved optical constants "
                         f"for ({_bold}, {px_size}); using defaults "
                         f"{OPTICAL_AREA_DEFAULT}/{OPTICAL_WORD_DEFAULT} — "
                         f"run tools/optical_solve.py\n")
        area, word = OPTICAL_AREA_DEFAULT, OPTICAL_WORD_DEFAULT
    if os.environ.get("CORE_OPTICAL_AREA"):
        area = float(os.environ["CORE_OPTICAL_AREA"])
    if os.environ.get("CORE_OPTICAL_WORD"):
        word = float(os.environ["CORE_OPTICAL_WORD"])
    return area, word


def render_atlas(ttf_path: str, px_size: int, symbol: str) -> str:
    r = raster(ttf_path, px_size)
    glyphs, glyph_data, chars = r["glyphs"], r["glyph_data"], r["chars"]
    ascent, descent, band = r["ascent"], r["descent"], r["band"]

    # Every data_offset must fit the uint16_t field it is generated into.
    if len(glyph_data) > DATA_OFFSET_MAX:
        raise SystemExit(
            f"{symbol}: glyph bitmap data is {len(glyph_data)} bytes, past the "
            f"{DATA_OFFSET_MAX} the generated `uint16_t data_offset` field can "
            f"address. Offsets past that point would wrap and every later "
            f"glyph would draw a slice of the wrong bitmap — silently, at "
            f"runtime. Use a smaller pixel size, trim EXTRAS, or widen "
            f"data_offset in core/ui/atlas.h (and the generator).")

    # Output as a C header.
    out = []
    out.append(f"// {symbol} — auto-generated by tools/atlas_gen.py.")
    out.append(f"// Source: {os.path.basename(ttf_path)} @ {px_size}px")
    out.append("// Edit the generator, not this file.")
    out.append("")
    out.append("#include \"../atlas.h\"")
    out.append("")
    out.append(f"static const uint8_t {symbol}_DATA[] = {{")
    # Pack 16 bytes per line for diff-friendliness.
    for i in range(0, len(glyph_data), 16):
        line = ", ".join(f"0x{b:02x}" for b in glyph_data[i:i + 16])
        out.append(f"    {line},")
    out.append("};")
    out.append("")
    total = 95 + len(EXTRAS)
    # Tracking first: the optical solver needs it to compute a pair's step.
    track = tracking_for(symbol, px_size)

    kerns = r["kerns"]
    # Optical pass: every LETTER pair is re-solved from the baked ink so they
    # all land on the same gap. Font kerning still governs everything else
    # (digits, punctuation, the Latin-1 extras), where design widths matter
    # more than a uniform optical rhythm.
    if OPTICAL_KERN:
        area_px, word_px = solved_for(symbol, px_size)
        if OPTICAL_MODE != "area":
            word_px = OPTICAL_WORD_PX
        opt = optical_kern(glyphs, glyph_data, track, OPTICAL_TARGET_PX, band,
                           area_px=area_px)
        merged = {(l, r): v for l, r, v in kerns}
        merged.update({(l, r): v for l, r, v in opt})   # optical wins
        kerns = [(l, r, v) for (l, r), v in merged.items() if v != 0]
        kerns.sort(key=lambda e: (e[0], e[1]))
        # ...and pin the word gap the same way.
        sp = glyphs[0]
        glyphs[0] = (sp[0], sp[1], sp[2], sp[3],
                     fit_space_advance(glyphs, glyph_data, kerns, track,
                                       word_px,
                                       band if OPTICAL_MODE == "area" else None),
                     sp[5])

    out.append(f"static const atlas_glyph_t {symbol}_GLYPHS[{total}] = {{")
    for i, (ox, oy, w, h, adv, off) in enumerate(glyphs):
        if i < 95:
            cp = 0x20 + i
            label = f"0x{cp:02X} '{chr(cp) if cp != 0x5c else '\\'}'"
            idx = cp - 0x20
        else:
            uni, ch = EXTRAS[i - 95]
            label = f"U+{uni:04X} '{ch}' (extra)"
            idx = i
        out.append(
            f"    [{idx:2d}] = {{ .data_offset = {off:5d}, "
            f".w = {w:3d}, .h = {h:3d}, "
            f".offset_x = {ox:3d}, .offset_y = {oy:3d}, "
            f".advance = {adv:5d} }},   // {label}"
        )
    out.append("};")
    out.append("")

    # Kern table: sorted (left, right) so the device binary-searches it.
    out.append(f"/* {len(kerns)} kern pairs, adj in 1/32 px, sorted by")
    out.append(" * (left<<8)|right for binary search. */")
    if kerns:
        out.append(f"static const atlas_kern_t {symbol}_KERN[{len(kerns)}] = {{")
        for li, ri, adj in kerns:
            lc = chars[li]
            rc = chars[ri]
            out.append(
                f"    {{ {li:3d}, {ri:3d}, {adj:4d} }},"
                f"   // '{lc}''{rc}'  {adj / KERN_ONE:+.2f}px")
        out.append("};")
    else:
        out.append(f"static const atlas_kern_t {symbol}_KERN[1] = {{ {{0,0,0}} }};")
    out.append("")

    # Line height: ascent + descent + a hair of leading.
    line_h = ascent + descent
    out.append(f"const atlas_t {symbol} = {{")
    out.append(f"    .glyphs      = {symbol}_GLYPHS,")
    out.append(f"    .data        = {symbol}_DATA,")
    out.append(f"    .kern        = {symbol}_KERN,")
    out.append(f"    .kern_n      = {len(kerns)},")
    out.append(f"    .tracking    = {track},"
               f"   // {track / ADV_ONE:+.2f}px letter-spacing")
    out.append(f"    .ascent      = {ascent},")
    out.append(f"    .descent     = {descent},")
    out.append(f"    .line_height = {line_h},")
    out.append("};")
    out.append("")
    return "\n".join(out)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("ttf")
    ap.add_argument("px_size", type=int)
    ap.add_argument("symbol")
    ap.add_argument("output")
    args = ap.parse_args()

    contents = render_atlas(args.ttf, args.px_size, args.symbol)
    out_dir = os.path.dirname(args.output) or "."
    os.makedirs(out_dir, exist_ok=True)
    with open(args.output, "w", encoding="utf-8") as f:
        f.write(contents)
    sys.stderr.write(f"wrote {args.output}\n")
    write_glyphmap(out_dir)                 # font-independent; rewritten each run
    return 0


if __name__ == "__main__":
    sys.exit(main())
