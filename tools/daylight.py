"""
tools/daylight.py — the ONE definition of "daylight between two glyphs on a
row", shared by the generator (atlas_gen.optical_kern, fit_space_advance),
the solver (optical_solve) and the judge (text_metrics.string_gaps), so the
value solved for is the value judged and both are the value the eye sees.

WHY A DEFINITION OF ITS OWN

The optical solve that shipped equalised daylight measured from each glyph's
OUTERMOST pixel with any alpha at all. Then the artist sub-line on the album
list (regular 9, always drawn muted) still read as "Je remih", "Ste ely",
"Bie ber": every 'e', 's', 'c' followed by a hole. The 9px 'e' explains it:

        row 6   177  38   0 163   2
        row 7   248 192 192 211  17
        row 9    26 189 191 170   7

On four of its five x-height rows the rightmost pixel is alpha 2-17 — pure
antialiasing residue, seven sRGB units of darkening on the panel, invisible —
and the solver counted it as a full column of ink. So it placed the next
letter a pixel further out than the eye wanted, for exactly the glyphs whose
curves end mid-pixel. Across the six atlases 17-29% of lowercase x-height
rows end in a sub-64-alpha pixel on one side or the other; the solve had
equalised a quantity the panel does not draw, to an sd of 0.24px, while the
VISIBLE daylight it left had an sd of 0.69px at 9px (coverage-integrated,
over real artist names). The same fault is present at every size — it is
merely a smaller fraction of a wider gap at 12 and 18.

Thresholding higher (alpha >= 64, as the no-touch floor already does) trades
one cliff for another: a 63 counts as nothing and a 64 as a whole pixel.
Alpha IS coverage — the rasteriser wrote the fraction of the pixel the
outline covers — so the honest measure integrates it.

THE MEASURE

For one row, with A placed at xa and B at xb (whole pixels, the device pen):
the daylight is the WHITE AREA between A's core and B's core — every column
strictly between A's last >= CORE_ALPHA pixel and B's first, each contributing
1 - alpha/255, where alpha is the darker of the two glyphs' pixels there.
Fringe pixels on either side count by what they leave white; a 17 is 93%
daylight, a 200 is 22%. When the cores overlap the row's daylight is the
overlap as a negative whole number (no fringe credit for a collision). Rows
where either glyph has no core are not adjacent at that height and are
skipped, as before.

CORE_ALPHA is 128: the level text_metrics has always called "core, what the
eye reads as the letter". The one discontinuity left is a pixel crossing
127 -> 128, worth at most half a pixel of daylight, against the whole pixel
the any-alpha rule flipped at 0 -> 1. Stems in these atlases sit at 170-250,
so the edge pixel that matters is rarely in that zone.

Everything here is plain Python on the baked alpha bytes: no Pillow, so the
judge can import it in a tree with no venv.
"""

CORE_ALPHA = 128


def glyph_rows(bmp, w, h, oy):
    """{ascender-relative row: alpha list} for one glyph's baked bitmap.

    Keyed by oy + row (offset_y is "px below the ascender"), so glyphs of
    different heights compare row against row at the same height on screen.
    Raw bitmap row 0 of 'L' and of 'a' are NOT the same line — see
    atlas_gen.row_extents_bitmap for the bug that taught us that.
    """
    return {oy + r: list(bmp[r * w:(r + 1) * w]) for r in range(h)}


def core_span(row):
    """(first, last) column at or above CORE_ALPHA, or None if the row has
    only fringe."""
    first = last = None
    for c, a in enumerate(row):
        if a >= CORE_ALPHA:
            if first is None:
                first = c
            last = c
    return None if first is None else (first, last)


def row_daylight(ra, xa, rb, xb, ca=None, cb=None):
    """White area between A's core and B's core on one row (see module doc).
    ra/rb are alpha lists; xa/xb the glyphs' left edges on screen. ca/cb are
    the precomputed core spans if the caller has them. None when either
    glyph has no core on this row; negative when the cores overlap."""
    ca = core_span(ra) if ca is None else ca
    cb = core_span(rb) if cb is None else cb
    if ca is None or cb is None:
        return None
    x0 = xa + ca[1] + 1               # first column past A's core
    x1 = xb + cb[0]                   # B's core column, exclusive
    if x1 <= x0:
        return float(x1 - x0)         # overlap: whole pixels, no fringe credit
    white = 0.0
    for x in range(x0, x1):
        a = 0
        i = x - xa
        if 0 <= i < len(ra):
            a = ra[i]
        j = x - xb
        if 0 <= j < len(rb) and rb[j] > a:
            a = rb[j]
        white += 1.0 - a / 255.0
    return white


def row_daylights(rows_a, xa, rows_b, xb, band=None):
    """Daylight on every row both glyphs have a core on, restricted to `band`
    (top, bottom) rows if given. Empty if they share no such row."""
    ys = set(rows_a) & set(rows_b)
    if band:
        ys = {y for y in ys if band[0] <= y < band[1]}
    vals = []
    for y in ys:
        d = row_daylight(rows_a[y], xa, rows_b[y], xb)
        if d is not None:
            vals.append(d)
    return vals


def band_daylight(rows_a, xa, rows_b, xb, band=None):
    """Mean daylight over the rows both glyphs have a core on, restricted to
    `band` — the x-height band, the texture the eye integrates. None if they
    share no such row.

    This is the TEXTURE measure only. The no-touch floor (atlas_gen) stays a
    threshold — one clear pixel at alpha >= 64 on the tightest row — on
    purpose: whether two letters read as joined is decided by two dark-ish
    pixels being adjacent, a cliff, not an integral. A coverage floor was
    tried ("1px of white on the tightest row"); it let [core][115][115][core]
    through as 1.1px of daylight, which is a grey bridge on the panel, and
    it made bold's rhythm worse, not better.
    """
    vals = row_daylights(rows_a, xa, rows_b, xb, band)
    return sum(vals) / len(vals) if vals else None


def counter_daylight(rows, band):
    """Mean white inside a glyph with two vertical strokes — the counter of
    'n' — over the band rows that have exactly two core runs. The arch rows
    of n have one run and are skipped. Measured with the same integral as
    the gaps it will be compared to, or the comparison means nothing."""
    vals = []
    for y, row in rows.items():
        if not (band[0] <= y < band[1]):
            continue
        runs, inrun = [], False
        for c, a in enumerate(row):
            if a >= CORE_ALPHA and not inrun:
                runs.append([c, c]); inrun = True
            elif a >= CORE_ALPHA:
                runs[-1][1] = c
            else:
                inrun = False
        if len(runs) == 2:
            vals.append(sum(1.0 - row[c] / 255.0
                            for c in range(runs[0][1] + 1, runs[1][0])))
    return sum(vals) / len(vals) if vals else None
