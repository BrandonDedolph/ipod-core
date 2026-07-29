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

  STEMVAR  Distinct widths measured across the vertical stems of i l h n u.
           Those stems are all the same weight by design, so >1 means the
           rasteriser is rendering identical shapes at different weights —
           the classic unhinted-at-small-size artefact.

Lower is better for every metric. Usage:

    tools/.venv/bin/python3 tools/glyph_quality.py FACE=path/to/Regular.ttf ...
    tools/.venv/bin/python3 tools/glyph_quality.py            # repo default
"""
import sys
from PIL import Image, ImageDraw, ImageFont

SIZES = (9, 11, 13)
XLETTERS = "acemnorsuvwxz"       # flat-topped + round; all share an x-height
STEMLETTERS = "ilhnu"


def render(font, ch):
    bb = font.getbbox(ch)
    if not bb or bb[2] - bb[0] <= 0 or bb[3] - bb[1] <= 0:
        return None, None
    w, h = bb[2] - bb[0], bb[3] - bb[1]
    img = Image.new("L", (w, h), 0)
    ImageDraw.Draw(img).text((-bb[0], -bb[1]), ch, font=font, fill=255)
    return img, bb


def measure(path, px):
    font = ImageFont.truetype(path, px, layout_engine=ImageFont.Layout.RAQM)
    fringe = solid = total = 0
    tops, bots, stems = set(), set(), set()
    for ch in (XLETTERS + STEMLETTERS + "ABCDEFGHIJKLMNOPQRSTUVWXYZ"):
        img, bb = render(font, ch)
        if img is None:
            continue
        px_vals = list(img.getdata())
        for v in px_vals:
            if v == 0:
                continue
            total += 1
            if v < 64:
                fringe += 1
            elif v >= 192:
                solid += 1
        if ch in XLETTERS:
            # bbox top/bottom ARE the ink extents, in baseline-relative coords
            tops.add(bb[1])
            bots.add(bb[3])
        if ch in STEMLETTERS:
            # widest run of >=50% alpha on the glyph's densest row = stem width
            best = 0
            for row in range(img.height):
                run = mx = 0
                for col in range(img.width):
                    if img.getpixel((col, row)) >= 128:
                        run += 1
                        mx = max(mx, run)
                    else:
                        run = 0
                best = max(best, mx)
            if best:
                stems.add(best)
    if not total:
        return None
    return dict(fringe=100.0 * fringe / total, solid=100.0 * solid / total,
                xvar=len(tops), bvar=len(bots), stemvar=len(stems))


def main():
    args = sys.argv[1:]
    if args:
        faces = [tuple(a.split("=", 1)) for a in args]
    else:
        faces = [("Nunito", "tools/fonts-src/Nunito-Regular.ttf")]

    print(f"{'face':<14}{'px':>4}{'FRINGE':>9}{'SOLID':>8}"
          f"{'XVAR':>6}{'BVAR':>6}{'STEMVAR':>9}")
    print("-" * 56)
    totals = {}
    for name, path in faces:
        agg = []
        for px in SIZES:
            m = measure(path, px)
            if not m:
                continue
            agg.append(m)
            print(f"{name:<14}{px:>4}{m['fringe']:>8.1f}%{m['solid']:>7.1f}%"
                  f"{m['xvar']:>6}{m['bvar']:>6}{m['stemvar']:>9}")
        if agg:
            totals[name] = (
                sum(a["fringe"] for a in agg) / len(agg),
                sum(a["xvar"] + a["bvar"] + a["stemvar"] for a in agg) / len(agg))
        print()

    if len(totals) > 1:
        print("ranking (lower is better)")
        print(f"  {'face':<14}{'mean FRINGE':>13}{'mean raggedness':>18}")
        for name, (f, r) in sorted(totals.items(), key=lambda kv: kv[1][0]):
            print(f"  {name:<14}{f:>12.1f}%{r:>18.1f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
