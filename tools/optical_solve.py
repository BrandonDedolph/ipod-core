#!/usr/bin/env python3
"""
tools/optical_solve.py — SOLVE the optical spacing constants per atlas, so the
numbers in atlas_gen.OPTICAL_SOLVED are the output of a stated objective and
not the last value someone swept that looked fine on a monitor.

That is how e190093 went wrong: a plausible criterion (equal minimum
clearance) applied confidently and shipped; it split words. This tool exists
so the next criterion has to argue for itself in numbers before it ships.

WHAT IS SOLVED
  For each shipped atlas (face, px), two numbers: AREA — the x-height-band
  daylight every letter pair is pinned to — and WORD — the daylight across a
  space. Everything is evaluated with the device's own pen on the rasterised
  bitmaps the header will carry (atlas_gen.raster), over the real UI strings
  in tools/ui_strings.txt, through text_metrics.string_gaps — the same
  function that reports the result, so what is solved is what is judged.

THE OBJECTIVE, AND WHY
  Rhythm: the standard deviation of per-pair daylight across the adjacent
  LOWERCASE pairs in the strings (CV is reported alongside). Even rhythm is
  what the eye reads as "well spaced"; a hole is a pair far from the mean.

  Lowercase, because that is the texture of running text and every hole the
  device showed was one (ff, ld, ht, ft, si). Capital pairs are solved with
  the same rule and the floor, but kept out of the objective: an F's arm or
  a C's mouth sits inside the x-height band, so the band daylight from F's
  STEM to l is large while l visibly tucks under the arm. That is the
  letterform, not a gap. When capitals were left in, 'Ca'/'Fl' were the max
  pair on every atlas and dragged the word gap past a third of an em.

  Variance cannot be minimised on its own. The pen steps in whole pixels, so
  every pair carries ±0.5px of quantisation error whatever the target, and
  the pairs the floor binds (ff, ft, ld — a tail meets a stem) keep a fixed
  excess; both shrink RELATIVE to the mean as it grows, so the unconstrained
  minimum of CV is letterspacing, which is precisely what "area 3" looked
  like. sd is used rather than CV because it does not reward looseness
  through the 1/mean term, but it still falls as the floor-bound excess is
  absorbed. So the density has to be pinned by something the face owns, and
  variance is minimised over what is left.

  The pin is the oldest rule in fitting: the space BETWEEN straight-sided
  letters should equal the space INSIDE them — the counter of n. (Tracy,
  Letters of Credit; every hand-spaced text face is fitted to it.) It is
  measured here from the baked n at this ppem, row by row over the band, so it
  is a property of the face at the size, not a constant.

  So: choose AREA within TOL of the n counter, and among those, minimum
  sd. AREA is what a straight-sided pair actually receives — 'nn' is a
  rectangle of daylight, so its gap IS the target — which is Tracy's rule
  stated literally. TOL is 0.25px, half the pen quantum: nothing finer is
  achievable, and the gaps move in a staircase — several AREA values round
  the same pairs the same way and differ only in which borderline pairs go
  up or down; that residual choice is the only free variance left, and it
  is the one minimised. The whole curve is printed so the staircase is
  visible, and so is the tighter/looser trade-off if the owner wants to
  pin density differently.

  Why the TARGET and not the achieved mean: the first solve pinned the
  mean over the strings to the counter. Under the any-alpha measure that
  was reachable by accident — round letters' fringe counted as ink, so
  their gaps under-read. Measured honestly, a round pair at the no-touch
  floor already averages more white over the band than the counter (bowls
  curve away above and below the tightest row), so the mean sits above
  AREA by a residual the letterforms own, and for bold — n counter 1.74px,
  floor-bound mean 2.02px — no AREA reaches it. Pinning the mean there
  put every pair on the floor. The achieved mean is still reported.

  Floor: the tightest row (at alpha >= 64) must clear 1px. Pairs where the
  floor binds (an open C against a, an F over l, r's arm over y) keep more
  daylight than AREA; they are the irreducible residual of the CV and are
  listed. The floor stays a threshold while the measure integrates — see
  daylight.band_daylight for why.

  THE MEASURE ITSELF is tools/daylight.py — coverage-integrated white
  between the cores. The first solve measured from any-alpha edges and
  equalised, to an sd of 0.24px, a quantity a 17-alpha pixel could move by
  a whole column; the visible daylight it left had an sd of 0.6-0.7px, and
  the artist sub-line showed it ("Je remih", "Ste ely"). Same objective,
  honest instrument.

THE WORD GAP, AND ITS BOUNDS
  Lower bound — the guard against the failure e190093 shipped: for the real
  strings, the SMALLEST gap across a space must exceed the LARGEST LOWERCASE
  gap inside a word by a whole pixel. Below that, some word gap is a letter
  gap ("FleetwoodMac", bold-13 "Wants to" at 1.9px). The same test against
  every pair including capitals is also printed; it fails on every atlas at
  any word gap under a third of an em, because 'Ca' and 'Fl' measure 5-6px
  of band daylight for the reason above, and it is not used to bound.
  Upper bound — the space's ADVANCE may not exceed half an em: the widest a
  justified line is allowed to stretch a word space before it is flagged
  loose (TeX: 1/3 em plus 1/6 em of stretch; Bringhurst 2.1.4: "never more
  than half an em"). Past it lines of short words read as fragments. Print's
  1/3-em TARGET was tried first and is not attainable here, and the reason
  is worth stating: at 9-13 ppem the whole-pixel pen makes a letter gap
  0.17-0.23 em, three times print's, and a word gap has to scale with the
  letter gap it must be told apart from, not with the em. Converted through
  the atlas so it lands in the same daylight units.
  Between them the midpoint is taken: it is the value with the most room
  against the ±0.5px quantisation on BOTH sides, and it is a range midpoint,
  not a taste. If the range is empty the tool says so and picks the guard.

Usage:
    tools/.venv/bin/python3 tools/optical_solve.py [--strings F] [--tol PX]
    Prints the table and an OPTICAL_SOLVED block to paste into atlas_gen.py.
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import atlas_gen                                              # noqa: E402
from daylight import counter_daylight                         # noqa: E402
from text_metrics import (string_gaps, rhythm, read_strings,  # noqa: E402
                          glyph_alpha_rows)

REPO = os.path.dirname(HERE)
SRC = os.path.join(REPO, "tools", "fonts-src")
# The six atlases text.c links (regular 9/11/12, bold 12/13/18).
ATLASES = [
    ("Nunito-Regular.ttf", 9,  "NUNITO_REGULAR_9",  False),
    ("Nunito-Regular.ttf", 11, "NUNITO_REGULAR_11", False),
    ("Nunito-Regular.ttf", 12, "NUNITO_REGULAR_12", False),
    ("Nunito-Bold.ttf",    12, "NUNITO_BOLD_12",    True),
    ("Nunito-Bold.ttf",    13, "NUNITO_BOLD_13",    True),
    ("Nunito-Bold.ttf",    18, "NUNITO_BOLD_18",    True),
]


def as_dict(glyphs):
    return {i: dict(ox=ox, oy=oy, w=w, h=h, adv=adv, off=off)
            for i, (ox, oy, w, h, adv, off) in enumerate(glyphs)}


def n_counter(gd, data, band):
    """Mean daylight inside 'n' over the band rows with two stems (the arch
    rows have one and are skipped), measured with the SAME integral the
    pair gaps use (daylight.counter_daylight) — a pin in different units
    from the thing it pins would be a number, not a rule."""
    g = gd[ord("n") - 0x20]
    return counter_daylight(glyph_alpha_rows(g, data), band)


def merged_kern(font_kerns, opt):
    k = {(l, r): v for l, r, v in font_kerns}
    k.update({(l, r): v for l, r, v in opt})
    return {p: v for p, v in k.items() if v != 0}


def frange(a, b, step):
    n = int(round((b - a) / step))
    return [round(a + i * step, 4) for i in range(n + 1)]


def solve(ttf, px, symbol, bold, lines, tol, verbose, floor_alpha):
    r = atlas_gen.raster(os.path.join(SRC, ttf), px)
    glyphs, data, band = r["glyphs"], r["glyph_data"], r["band"]
    track = atlas_gen.tracking_for(symbol, px)
    gd = as_dict(glyphs)
    target = n_counter(gd, data, band)
    # The bitmap work is target-independent: one table, many targets.
    table = atlas_gen.pair_table(glyphs, data, band, floor_alpha=floor_alpha)

    # ---- letters: sweep AREA, pin density to the n counter, minimise CV
    curve = []
    for A in frange(1.0, 5.0, 0.05):
        opt = atlas_gen.optical_kern(glyphs, data, track, 2, band,
                                     mode="area", area_px=A, table=table)
        kern = merged_kern(r["kerns"], opt)
        letters, _ = string_gaps(gd, data, kern, track, band, lines)
        rr = rhythm(letters, [])
        curve.append((A, rr["mean"], rr["sd"], rr["max_pair"], kern, letters,
                      rr["cv"], rr["max_min"]))
    # AREA (the target straight pairs receive) within TOL of the counter;
    # the sweep spans 1-5px so this is never empty for a counter in range.
    feas = [c for c in curve if abs(c[0] - target) <= tol]
    best = min(feas, key=lambda c: (c[2], c[0]))
    A, mean, sd, max_pair, kern, letters, cv, max_min = best

    # ---- words: sweep WORD at the chosen kern, bound below by the guard and
    # above by a third of an em of advance, take the midpoint
    kern_list = sorted((l, r, v) for (l, r), v in kern.items())
    em_third = px / 2.0          # half an em; see the docstring
    rows = []
    for W in frange(2.0, 14.0, 0.25):
        sp_adv = atlas_gen.fit_space_advance(glyphs, data, kern_list, track,
                                             W, band)
        gd2 = dict(gd)
        gd2[0] = dict(gd[0], adv=sp_adv)
        _, words = string_gaps(gd2, data, kern, track, band, lines)
        rw = rhythm(letters, words)
        rows.append((W, sp_adv / 64.0, rw["min_word"], rw["word_mean"],
                     rw["ratio"], rw["separable"], rw["separable_all"]))
    lo = next((row for row in rows if row[5]), None)
    hi_rows = [row for row in rows if row[1] <= em_third]
    hi = hi_rows[-1] if hi_rows else None
    if lo and hi and hi[0] >= lo[0]:
        cand = [row for row in rows if lo[0] <= row[0] <= hi[0]]
        chosen = cand[len(cand) // 2]
        note = ""
    else:
        chosen = lo or rows[-1]
        note = "  RANGE EMPTY: guard needs more than half an em; guard wins"

    if verbose:
        print(f"\n{symbol}: n counter {target:.2f}px, band rows {band}, "
              f"tracking {track/64:.2f}px, 1/2 em = {em_third:.2f}px advance")
        print("  AREA   mean     sd     CV   loosest(mean)  widest closest   (lowercase pairs)")
        last = None
        for c in curve:
            key = (round(c[1], 2), round(c[2], 3))
            if key == last:
                continue
            last = key
            flag = " <-- chosen" if c[0] == A else (
                "  feasible" if abs(c[0] - target) <= tol else "")
            print(f"  {c[0]:4.2f}  {c[1]:5.2f}  {c[2]:5.3f}  {c[6]:5.3f}  "
                  f"{c[3][0]:4.1f} {c[3][2]}        {c[7][1]:4.1f} {c[7][2]}{flag}")
        print("  WORD  space adv  min word(closest)  mean word  ratio  guard(lower/all)")
        for row in rows:
            if row[0] % 1.0:
                continue
            mark = " <-- chosen" if row[0] == chosen[0] else ""
            print(f"  {row[0]:4.1f}  {row[1]:7.2f}px  {row[2][1]:4.1f} "
                  f"'{row[2][2]}'  {row[3]:5.2f}  {row[4]:5.2f}  "
                  f"{'ok' if row[5] else 'SPLIT'}/"
                  f"{'ok' if row[6] else 'split'}{mark}")
    loosest = " ".join(f"{t[2]}({t[0]:.1f}/{t[1]:.0f})" for t in
                       sorted((l for l in letters if l[2].islower()),
                              reverse=True)[:4])
    return dict(symbol=symbol, bold=bold, px=px, target=target, A=A,
                mean=mean, sd=sd, cv=cv, max_pair=max_pair,
                max_min=max_min, strict_ok=chosen[6],
                W=chosen[0], W_lo=lo[0] if lo else None,
                W_hi=hi[0] if hi else None, space_adv=chosen[1],
                min_word=chosen[2], ratio=chosen[4], note=note,
                loosest=loosest)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--strings", default=os.path.join(HERE, "ui_strings.txt"))
    ap.add_argument("--tol", type=float, default=0.25,
                    help="px the achieved mean may sit from the n counter")
    ap.add_argument("--floor-alpha", type=int,
                    default=atlas_gen.OPTICAL_FLOOR_ALPHA,
                    help="alpha at which a pixel counts as ink for the "
                         "no-touch floor (1 any, 64 the fringe cutoff, 128 core)")
    ap.add_argument("--verbose", action="store_true",
                    help="print the AREA and WORD sweep curves per atlas")
    args = ap.parse_args()
    lines = read_strings(args.strings)

    results = [solve(t, px, sym, b, lines, args.tol, args.verbose,
                     args.floor_alpha)
               for t, px, sym, b in ATLASES]
    print(f"\nfloor: tightest row at alpha >= {args.floor_alpha} clears "
          f"{atlas_gen.OPTICAL_FLOOR_PX}px; strings: {args.strings}")

    print(f"\n{'atlas':<18}{'n ctr':>6}{'AREA':>6}{'mean':>6}{'sd':>6}{'CV':>6}"
          f"{'max lower':>11}{'closest':>9}{'WORD':>6}{'range':>12}{'sp adv':>7}"
          f"{'min word':>10}{'ratio':>6}{'strict':>7}  loosest lowercase pairs (mean/closest)")
    print("-" * 132)
    for x in results:
        rng = (f"{x['W_lo']:.2f}-{x['W_hi']:.2f}" if x["W_lo"] is not None
               and x["W_hi"] is not None else "n/a")
        print(f"{x['symbol']:<18}{x['target']:>6.2f}{x['A']:>6.2f}"
              f"{x['mean']:>6.2f}{x['sd']:>6.3f}{x['cv']:>6.3f}"
              f"{x['max_pair'][0]:>7.1f} {x['max_pair'][2]:<3}"
              f"{x['max_min'][1]:>5.1f} {x['max_min'][2]:<3}"
              f"{x['W']:>6.2f}{rng:>12}{x['space_adv']:>7.2f}"
              f"{x['min_word'][1]:>6.1f} {x['min_word'][2]:<3}"
              f"{x['ratio']:>6.2f}{('ok' if x['strict_ok'] else 'fail'):>7}"
              f"  {x['loosest']}"
              f"{x['note']}")

    print("\nOPTICAL_SOLVED = {")
    for x in results:
        print(f"    ({x['bold']!s:<5}, {x['px']:2d}): ({x['A']:.2f}, "
              f"{x['W']:.2f}),   # {x['symbol']}: n counter {x['target']:.2f}, "
              f"sd {x['sd']:.2f} CV {x['cv']:.3f}, word range "
              f"{x['W_lo']}-{x['W_hi']}")
    print("}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
