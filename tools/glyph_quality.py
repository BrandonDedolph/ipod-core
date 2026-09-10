#!/usr/bin/env python3
"""
tools/glyph_quality.py — measure how CRISPLY a typeface rasterises at the
sizes we ship, so choosing a face is an experiment rather than a matter of
taste.

tools/text_metrics.py answers "are the gaps right". This answers the other
half: "do the glyphs themselves come out sharp, and do they line up". At
9-13px those two properties are most of what makes small text read as clean or
as mush, and neither is visible in a spacing number.

Metrics, all computed from the SAME rasterisation the atlas generator uses
(PIL/FreeType, Raqm layout), so a face that scores well here will score well
baked:

  FRINGE   % of inked pixels at <25% alpha. Antialiasing that never resolves
           into ink — pure blur. A hinted face snaps stems onto the pixel grid
           and produces less of it. This is the single best proxy for "looks
           smeary at small sizes".

  SOLID    % of inked pixels at >=75% alpha. The complement worth naming
           separately: high FRINGE with low SOLID is a face dissolving into
           grey, which no amount of spacing work can fix.

  XVAR     How many DIFFERENT pixel rows the x-height tops of a c e m n o r s
           u v w x z land on. A hinted face snaps them all to one row; an
           unhinted one scatters them, and the eye reads the resulting ragged
           top edge as "wobbly" even when every glyph is individually fine.

  BVAR     Same for baselines (bottom row of those glyphs). Round letters
           legitimately overshoot by a hair, so 2 is normal; more is ragged.

  XH       x-height in whole pixels (ink height of 'x'), and XTOP the peak
           alpha of z's flat top bar: 255 means the x-height sits on a whole
           pixel, ~128 means every lowercase letter carries a grey half-row.

  STEM     width in px of the vertical stem of i and l, read on the row
           halfway down the x-height — the one row where those glyphs are
           exactly one stroke. (The first version took the widest >=50% run
           on the DENSEST row of i l h n u, which for n/u/h is the arch, not
           the stem; it reported bold-18 Nunito as an 8px stem.)

  SPREAD   on that same row, ink mass / number of columns carrying any ink.
           1.00 = the stem sits in whole pixels; 0.50 = the same ink smeared
           evenly over two columns, which is what a 1.3px stem does when
           nothing snaps it.

  MASS     ink mass vs the true outline: coverage sum at this ppem over the
           coverage sum of a 16x render box-filtered down. >1 the hinter
           darkened the face, <1 it thinned it.

  CNTR     counter openness of a e o g: for each, the darkest enclosed
           background pixel (flood-fill from outside at <50% alpha; what is
           light but unreached is the counter). Mean of (255-min)/255, so
           1.00 = every counter has a fully clear pixel, 0.87 = the eye of
           the e is filling in. C0 counts letters with NO enclosed pixel at
           all — a counter that has closed.

Lower is better for FRINGE / XVAR / BVAR / C0; higher for SOLID / XTOP /
SPREAD / CNTR.

TRAP — the interpreter version. FreeType's default TrueType interpreter (v40)
applies only the VERTICAL part of a font's bytecode: through it Tahoma's stems
at 11px come out as two grey columns (SPREAD 0.5), worse than autohinted
Nunito (0.9). Set FREETYPE_PROPERTIES=truetype:interpreter-version=35 to get
the full x+y hinting the face was designed with; SPREAD goes to 1.00. Faces
with no bytecode (Nunito) go through the autohinter, which snaps both axes
regardless. atlas_gen.py inherits the same default, so a hinted face baked
without that variable set is baked smeared.

Usage:
    tools/.venv/bin/python3 tools/glyph_quality.py FACE=path/to/Regular.ttf ...
    SIZES=9,11,12 tools/.venv/bin/python3 tools/glyph_quality.py   # sizes
    tools/.venv/bin/python3 tools/glyph_quality.py            # repo default
"""
import os
import sys
from collections import Counter
from PIL import Image, ImageDraw, ImageFont

SIZES = tuple(int(s) for s in os.environ.get("SIZES", "9,11,13").split(","))
XLETTERS = "acemnorsuvwxz"       # flat-topped + round; all share an x-height
STEMLETTERS = "il"               # one stroke wide at mid x-height
COUNTERS = "aeog"
UPPER = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"


def render(font, ch):
    bb = font.getbbox(ch)
    if not bb or bb[2] - bb[0] <= 0 or bb[3] - bb[1] <= 0:
        return None, None
    w, h = bb[2] - bb[0], bb[3] - bb[1]
    img = Image.new("L", (w, h), 0)
    ImageDraw.Draw(img).text((-bb[0], -bb[1]), ch, font=font, fill=255)
    return img, bb


def ideal_mass(path, px, ch, scale=16):
    """Coverage of the outline itself: a 16x render, box-filtered down.
    Hinting at 16x the ppem moves nothing the eye could see."""
    f = ImageFont.truetype(path, px * scale,
                           layout_engine=ImageFont.Layout.RAQM)
    img, _ = render(f, ch)
    if img is None:
        return 0.0
    return sum(img.getdata()) / 255.0 / (scale * scale)


def counter_min(img):
    """Darkest alpha among enclosed light pixels; None if nothing is enclosed."""
    w, h = img.size
    px = img.load()
    light = [[px[x, y] < 128 for x in range(w)] for y in range(h)]
    seen = [[False] * w for _ in range(h)]
    stack = ([(x, y) for x in range(w) for y in (0, h - 1)] +
             [(x, y) for y in range(h) for x in (0, w - 1)])
    while stack:
        x, y = stack.pop()
        if (x < 0 or y < 0 or x >= w or y >= h or seen[y][x]
                or not light[y][x]):
            continue
        seen[y][x] = True
        stack += [(x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)]
    enclosed = [px[x, y] for y in range(h) for x in range(w)
                if light[y][x] and not seen[y][x]]
    return min(enclosed) if enclosed else None


def measure(path, px):
    font = ImageFont.truetype(path, px, layout_engine=ImageFont.Layout.RAQM)
    fringe = solid = total = 0
    tops, bots = set(), set()
    stems, spreads = [], []
    mass_h = mass_i = 0.0
    xtop = 0
    ctr, c0 = [], 0

    ximg, xbb = render(font, "x")
    xh = (xbb[3] - xbb[1]) if xbb else 0

    for ch in XLETTERS + STEMLETTERS + UPPER:
        img, bb = render(font, ch)
        if img is None:
            continue
        data = list(img.getdata())
        for v in data:
            if v == 0:
                continue
            total += 1
            if v < 64:
                fringe += 1
            elif v >= 192:
                solid += 1
        mass_h += sum(data) / 255.0
        mass_i += ideal_mass(path, px, ch)
        if ch in XLETTERS:
            # bbox top/bottom ARE the ink extents, in baseline-relative coords
            tops.add(bb[1])
            bots.add(bb[3])
        if ch == "z":
            xtop = max(data[:img.width])
        if ch in STEMLETTERS and xh:
            row = (bb[3] - xh + bb[3]) // 2 - bb[1]
            row = max(0, min(img.height - 1, row))
            vals = [img.getpixel((c, row)) for c in range(img.width)]
            core = sum(1 for v in vals if v >= 128)
            cols = sum(1 for v in vals if v > 0)
            if cols:
                stems.append(core)
                spreads.append((sum(vals) / 255.0) / cols)
    for ch in COUNTERS:
        img, _ = render(font, ch)
        if img is None:
            continue
        m = counter_min(img)
        if m is None:
            c0 += 1
        else:
            ctr.append((255 - m) / 255.0)
    if not total:
        return None
    return dict(fringe=100.0 * fringe / total, solid=100.0 * solid / total,
                xvar=len(tops), bvar=len(bots), xh=xh, xtop=xtop,
                stem=Counter(stems).most_common(1)[0][0] if stems else 0,
                spread=sum(spreads) / len(spreads) if spreads else 0.0,
                mass=mass_h / mass_i if mass_i else 0.0,
                cntr=sum(ctr) / len(ctr) if ctr else 0.0, c0=c0)


def main():
    args = sys.argv[1:]
    if args:
        faces = [tuple(a.split("=", 1)) for a in args]
    else:
        faces = [("Nunito", "tools/fonts-src/Nunito-Regular.ttf")]

    print(f"{'face':<16}{'px':>3}{'FRINGE':>8}{'SOLID':>7}{'XVAR':>5}"
          f"{'BVAR':>5}{'XH':>3}{'XTOP':>5}{'STEM':>5}{'SPREAD':>7}"
          f"{'MASS':>6}{'CNTR':>6}{'C0':>3}")
    print("-" * 79)
    totals = {}
    for name, path in faces:
        agg = []
        for px in SIZES:
            m = measure(path, px)
            if not m:
                continue
            agg.append(m)
            print(f"{name:<16}{px:>3}{m['fringe']:>7.1f}%{m['solid']:>6.1f}%"
                  f"{m['xvar']:>5}{m['bvar']:>5}{m['xh']:>3}{m['xtop']:>5}"
                  f"{m['stem']:>5}{m['spread']:>7.2f}{m['mass']:>6.2f}"
                  f"{m['cntr']:>6.2f}{m['c0']:>3}")
        if agg:
            totals[name] = (
                sum(a["fringe"] for a in agg) / len(agg),
                sum(a["spread"] for a in agg) / len(agg),
                sum(a["xvar"] + a["bvar"] for a in agg) / len(agg))
        print()

    if len(totals) > 1:
        print("ranking (by fringe; lower is better)")
        print(f"  {'face':<16}{'mean FRINGE':>13}{'mean SPREAD':>13}"
              f"{'mean raggedness':>17}")
        for name, (f, s, r) in sorted(totals.items(), key=lambda kv: kv[1][0]):
            print(f"  {name:<16}{f:>12.1f}%{s:>13.2f}{r:>17.1f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
