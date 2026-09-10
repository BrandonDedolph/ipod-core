#!/usr/bin/env python3
"""
tools/text_metrics.py — measure letter spacing objectively, from the SHIPPED
atlases, using the device's own pen arithmetic.

Why this exists: "looks cramped" and "looks airy" are not actionable, and a
zoomed PNG cannot tell you WHICH pairs are wrong. Uniform tracking cannot fix a
per-pair problem, so before changing a global number it is worth knowing
whether the complaint is the distribution or a handful of outliers.

What it measures: for every ordered letter pair, the horizontal daylight
between the two glyphs' ACTUAL INK — read out of the baked alpha bitmaps at a
coverage threshold — after placing them exactly as core/ui/text.c does. The
device pen is WHOLE pixels; each pair's step is rounded once (pen_step):

    x_a = pen + offset_x[a]
    step = advance[a] + tracking + kern[a][b] * 2      (26.6; 1/32px kern)
           — tracking applies on entry to a space, not on exit
    pen += (step + 32) >> 6
    x_b = pen + offset_x[b]

so an identical pair sits at an identical distance wherever it occurs, and a
string's width is the sum of these rounded steps (text_width agrees).

Reading the bitmaps rather than the bboxes matters: ~37% of nonzero pixels in
these atlases are sub-25%-alpha fringe, so a bbox-based gap says "touching"
while the visible stems are still a pixel apart. Two thresholds are reported:
CORE (>=50% alpha, what the eye reads as the letter) and SOFT (>0, any ink).

Usage:
    tools/.venv/bin/python3 tools/text_metrics.py [--worst N] [--atlas NAME]
"""
import argparse
import glob
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ATLAS_DIR = os.path.join(REPO, "core", "ui", "atlas")

GLYPH_RE = re.compile(
    r"\[\s*(\d+)\] = \{ \.data_offset =\s*(\d+), \.w =\s*(\d+), \.h =\s*(\d+), "
    r"\.offset_x =\s*(-?\d+), \.offset_y =\s*(-?\d+), \.advance =\s*(\d+) \}")
KERN_RE = re.compile(r"\{\s*(\d+),\s*(\d+),\s*(-?\d+) \},")
TRACK_RE = re.compile(r"\.tracking\s*=\s*(-?\d+)")
ASCENT_RE = re.compile(r"\.ascent\s*=\s*(-?\d+)")


def load_atlas(path):
    src = open(path, encoding="utf-8").read()
    glyphs = {}
    for m in GLYPH_RE.finditer(src):
        glyphs[int(m.group(1))] = dict(
            off=int(m.group(2)), w=int(m.group(3)), h=int(m.group(4)),
            ox=int(m.group(5)), oy=int(m.group(6)), adv=int(m.group(7)))
    # The DATA array: every 0xNN byte between "_DATA[" and the closing "};".
    ds = src.index("_DATA[")
    de = src.index("};", ds)
    data = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", src[ds:de])]
    kern = {(int(a), int(b)): int(v) for a, b, v in KERN_RE.findall(src)}
    tm = TRACK_RE.search(src)
    tracking = int(tm.group(1)) if tm else 0
    return glyphs, data, kern, tracking


def atlas_band(path):
    """(top, bottom) of the x-height in ascender-relative rows: the rows the
    area criterion and the rhythm metric integrate over. From the header's
    ascent and the baked height of 'x'."""
    src = open(path, encoding="utf-8").read()
    am = ASCENT_RE.search(src)
    glyphs, _, _, _ = load_atlas(path)
    x = glyphs.get(ord("x") - 0x20)
    if not am or not x:
        return None
    ascent = int(am.group(1))
    return (ascent - x["h"], ascent)


def string_gaps(glyphs, data, kern, tracking, band, lines):
    """The rhythm measure, from the device's own pen.

    For every adjacent LETTER pair inside a word, over the x-height-band rows
    where BOTH glyphs have ink (row against row — a glyph's extreme column
    over all rows is not its edge at any one height), any alpha counting as
    ink: the MEAN daylight (what the eye integrates as texture) and the MIN
    daylight (the closest approach — what decides whether two letters read
    as joined or as split). For every letter-space-letter: the same two
    across the space. Returns (letter_pairs, word_gaps), each a list of
    (mean, min, label). Shared by the solver and the report so the value
    solved for is the value judged.
    """
    letters, words = [], []
    for line in lines:
        pen = 0                        # whole pixels, as text.c text_draw
        xs, ext = [], {}
        for i, ch in enumerate(line):
            gi = ord(ch) - 0x20
            if gi not in glyphs:
                xs.append(None)
                continue
            xs.append(pen + glyphs[gi]["ox"])
            ext[i] = row_extents(glyphs[gi], data, 1)
            nxt = ord(line[i + 1]) - 0x20 if i + 1 < len(line) else None
            step = glyphs[gi]["adv"]
            if nxt is not None and nxt in glyphs:
                if gi != 0:            # tracking on entry to a space, not exit
                    step += tracking
                step += kern.get((gi, nxt), 0) * 2
            pen += (step + 32) >> 6    # pen_step: rounded ONCE per pair

        def daylight(i, j):
            ra, rb = ext[i], ext[j]
            sh = set(ra) & set(rb)
            if band:
                sh = {y for y in sh if band[0] <= y < band[1]}
            if not sh:
                return None
            g = [(xs[j] + rb[y][0]) - (xs[i] + ra[y][1]) - 1 for y in sh]
            return sum(g) / len(g), min(g)

        for i in range(len(line) - 1):
            a, b = line[i], line[i + 1]
            if a.isalpha() and b.isalpha() and i in ext and i + 1 in ext:
                d = daylight(i, i + 1)
                if d is not None:
                    letters.append((d[0], d[1], a + b))
        for i in range(1, len(line) - 1):
            if (line[i] == " " and line[i - 1].isalpha()
                    and line[i + 1].isalpha() and (i - 1) in ext
                    and (i + 1) in ext):
                d = daylight(i - 1, i + 1)
                if d is not None:
                    words.append((d[0], d[1], line[i - 1] + " " + line[i + 1]))
    return letters, words


def rhythm(letters, words):
    """Summary of string_gaps().

    Rhythm is measured on LOWERCASE pairs' mean daylight: that is the texture
    of running text, and every hole the device showed was one (ff, ld, ht,
    ft, si). Capital pairs are reported but kept out of the variance: an F's
    arm or a C's mouth sits inside the x-height band, so the band daylight
    from the STEM of F to an l is large while l visibly tucks under the arm —
    that is the letterform, not a gap, and letting it drive the numbers
    solved for the wrong thing (it forced the word gap past a third of an em
    on every atlas when it was tried).

    The word-splitting guard is measured on CLOSEST APPROACH: a pair reads as
    joined if it nearly touches anywhere in the band (ff's hooks, ld's tail),
    however far apart its stems are; a space reads as a space only if its
    closest approach is wider than any letter pair's. `separable` requires
    every word gap's min to clear every lowercase intra-word pair's min by a
    whole pixel; `separable_all` the same against every pair including
    capitals; `separable_mean` the same test on mean daylight, reported for
    comparison (it is the test the first solve used, and it is too strict for
    overhanging glyphs).
    """
    def stats(v):
        m = sum(v) / len(v)
        sd = (sum((x - m) ** 2 for x in v) / len(v)) ** 0.5
        return m, sd, sd / m if m else 0.0

    lower = [t for t in letters if t[2].islower()] or letters
    ml, sdl, cvl = stats([t[0] for t in lower])
    ma, sda, cva = stats([t[0] for t in letters])
    wmean = sum(t[0] for t in words) / len(words) if words else 0.0
    by_min = lambda t: (t[1], t[0])
    mxl, mxa = max(lower, key=by_min), max(letters, key=by_min)
    mn = min(words, key=by_min) if words else (0.0, 0.0, "")
    mxl_mean, mn_mean = max(lower), (min(words) if words else (0.0, 0.0, ""))
    return dict(mean=ml, sd=sdl, cv=cvl, mean_all=ma, sd_all=sda, cv_all=cva,
                max_pair=mxl_mean, max_pair_all=max(letters),
                max_min=mxl, max_min_all=mxa, min_word=mn, min_word_mean=mn_mean,
                word_mean=wmean, ratio=wmean / ml if ml else 0.0,
                separable=(mn[1] >= mxl[1] + 1.0) if words else False,
                separable_all=(mn[1] >= mxa[1] + 1.0) if words else False,
                separable_mean=(mn_mean[0] >= mxl_mean[0] + 1.0) if words else False)


def read_strings(path):
    return [l.rstrip("\n") for l in open(path, encoding="utf-8")
            if l.strip() and not l.startswith("#")]


def row_extents(g, data, thresh):
    """Per-row (first, last) ink columns, indexed by row from the glyph top.

    Rows are keyed by BASELINE-relative y so two glyphs of different heights
    can be compared row against row. offset_y is "px below the ascender", so
    baseline-relative row = offset_y + row - ascent; we only need a consistent
    key, so offset_y + row serves.
    """
    out = {}
    for row in range(g["h"]):
        base = g["off"] + row * g["w"]
        first = last = None
        for col in range(g["w"]):
            if data[base + col] >= thresh:
                if first is None:
                    first = col
                last = col
        if first is not None:
            out[g["oy"] + row] = (first, last)
    return out


def gap(ga, gb, data, kern, tracking, ia, ib, thresh):
    """Visual daylight: the MINIMUM horizontal clearance over the rows where
    both glyphs actually have ink.

    Comparing each glyph's extreme column over all rows (the obvious version,
    and the one this tool shipped with first) is wrong: 'y' reaches furthest
    right at its top arm while 'z' reaches furthest left at its baseline, so
    that pair scores as wide open when the shapes never approach each other at
    the same height. Only rows where both have ink can collide.
    """
    ra, rb = row_extents(ga, data, thresh), row_extents(gb, data, thresh)
    if not ra or not rb:
        return None
    xa = 0 + ga["ox"]
    pen = ga["adv"] + kern.get((ia, ib), 0) * 2
    if ia != 0:                        # text.c track_adv: not on the way OUT of a space
        pen += tracking
    xb = ((pen + 32) >> 6) + gb["ox"]
    shared = set(ra) & set(rb)
    if not shared:
        return None                    # never adjacent at any height
    return min((xb + rb[y][0]) - (xa + ra[y][1]) - 1 for y in shared)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--worst", type=int, default=10)
    ap.add_argument("--atlas", default=None)
    ap.add_argument("--atlas-dir", default=ATLAS_DIR,
                    help="measure the atlases in this directory instead of "
                         "the shipped ones — for judging a candidate face "
                         "baked into a scratch tree before it is committed")
    ap.add_argument("--triplets", action="store_true",
                    help="check all 52x52x52 letter triplets, including the "
                         "OUTER pair's clearance across a narrow middle glyph "
                         "— which no pairwise check can see")
    ap.add_argument("--strings", default=None,
                    help="measure RHYTHM over the strings in this file (one "
                         "per line, e.g. tools/ui_strings.txt): for every "
                         "adjacent letter pair, the mean daylight over the "
                         "x-height-band rows both glyphs ink, and its "
                         "coefficient of variation across all pairs. The "
                         "min-clearance number above can be perfectly "
                         "uniform while this is not — 'ff' and 'oo' can share "
                         "a 2px minimum and differ 3x in the daylight the eye "
                         "integrates. Also the word-splitting guard: every "
                         "word gap must clear every intra-word gap by 1px.")
    ap.add_argument("--target", type=float, default=None,
                    help="solve for the tracking (px) that puts each atlas's "
                         "soft mean gap at this many pixels, and print a "
                         "TRACKING_PX table")
    args = ap.parse_args()

    files = sorted(f for f in glob.glob(os.path.join(args.atlas_dir, "*.h"))
                   if not f.endswith("glyphmap.h"))
    if args.atlas:
        files = [f for f in files if args.atlas in os.path.basename(f)]
    letters = ([chr(c) for c in range(0x41, 0x5B)] +
               [chr(c) for c in range(0x61, 0x7B)])

    print(f"{'atlas':<22}{'track':>6}{'core mean':>11}{'core<=0':>9}"
          f"{'soft mean':>11}{'p10':>6}{'p90':>6}")
    print("-" * 71)
    report = {}
    for path in files:
        glyphs, data, kern, tracking = load_atlas(path)
        name = os.path.basename(path).replace("nunito_", "").replace(".h", "")
        core, soft = [], []
        for a in letters:
            for b in letters:
                ia, ib = ord(a) - 0x20, ord(b) - 0x20
                if ia not in glyphs or ib not in glyphs:
                    continue
                gc = gap(glyphs[ia], glyphs[ib], data, kern, tracking,
                         ia, ib, 128)
                gs = gap(glyphs[ia], glyphs[ib], data, kern, tracking,
                         ia, ib, 1)
                if gc is not None:
                    core.append((gc, a + b))
                if gs is not None:
                    soft.append((gs, a + b))
        core.sort()
        soft.sort()
        cm = sum(g for g, _ in core) / len(core)
        sm = sum(g for g, _ in soft) / len(soft)
        touch = sum(1 for g, _ in core if g <= 0) * 100.0 / len(core)
        p10 = soft[len(soft) // 10][0]
        p90 = soft[len(soft) * 9 // 10][0]
        print(f"{name:<22}{tracking/64:>6.2f}{cm:>11.2f}{touch:>8.1f}%"
              f"{sm:>11.2f}{p10:>6d}{p90:>6d}")
        report[name] = (core, soft)

    if args.strings:
        lines = read_strings(args.strings)
        print(f"\n--- rhythm over {len(lines)} strings from {args.strings} "
              f"(x-height-band daylight, row by row, any alpha) ---")
        print(f"{'atlas':<12}{'lower':>6}{'mean':>6}{'sd':>5}{'CV':>6}"
              f"{'max lower':>11}{'closest':>10}{'min word':>11}{'ratio':>6}"
              f" guard   loosest lowercase pairs (mean)")
        for path in files:
            glyphs, data, kern, track = load_atlas(path)
            name = os.path.basename(path).replace("nunito_", "").replace(".h", "")
            letters, words = string_gaps(glyphs, data, kern, track,
                                         atlas_band(path), lines)
            if not letters:
                continue
            r = rhythm(letters, words)
            lower = [t for t in letters if t[2].islower()]
            loose = " ".join(f"{t[2]}({t[0]:.1f})" for t in
                             sorted(lower, reverse=True)[:4])
            guard = ("ok/ok " if r["separable_all"] else
                     "ok    " if r["separable"] else "SPLIT ")
            print(f"{name:<12}{len(lower):>6}{r['mean']:>6.2f}{r['sd']:>5.2f}"
                  f"{r['cv']:>6.2f}"
                  f"{r['max_pair'][0]:>7.1f} {r['max_pair'][2]:<3}"
                  f"{r['max_min'][1]:>6.1f} {r['max_min'][2]:<3}"
                  f"{r['min_word'][1]:>6.1f} {r['min_word'][2]:<4}"
                  f"{r['ratio']:>6.2f} {guard}  {loose}")
        print("closest = the lowercase intra-word pair with the widest closest "
              "approach; min word = the word gap with the narrowest.\n"
              "guard: ok = every word gap's closest approach clears every "
              "lowercase pair's by 1px; ok/ok = clears every pair incl. capitals")
        return 0

    if args.triplets:
        letters = ([chr(c) for c in range(0x41, 0x5B)] +
                   [chr(c) for c in range(0x61, 0x7B)])
        print(f"\n--- all {len(letters)}^3 = {len(letters)**3} letter triplets ---")
        print(f"{'atlas':<14}{'triplets':>10}{'A-B/B-C bad':>13}"
              f"{'A-C touch':>11}{'worst A-C':>11}")
        for path in files:
            glyphs, data, kern, track = load_atlas(path)
            name = os.path.basename(path).replace("nunito_", "").replace(".h", "")
            idx = {c: ord(c) - 0x20 for c in letters}
            ext = {c: row_extents(glyphs[idx[c]], data, 1) for c in letters
                   if idx[c] in glyphs}
            keys = [c for c in letters if c in ext and ext[c]]
            # pen step exactly as core/ui/text.c pen_step(): whole pixels
            step = {}
            for a in keys:
                for b in keys:
                    ia, ib = idx[a], idx[b]
                    adv = glyphs[ia]["adv"]
                    tr = 0 if (ia == 0 or ib == 0) else track
                    step[(a, b)] = (adv + tr + kern.get((ia, ib), 0) * 2
                                    + 32) >> 6
            bad_adj = 0; ac_touch = 0; ac_worst = 99; ac_ex = ""
            n = 0
            for a in keys:
                ea = ext[a]; oxa = glyphs[idx[a]]["ox"]
                for b in keys:
                    eb = ext[b]; oxb = glyphs[idx[b]]["ox"]
                    s1 = step[(a, b)]
                    g1 = min((s1 + oxb + eb[y][0]) - (oxa + ea[y][1]) - 1
                             for y in (set(ea) & set(eb))) if (set(ea) & set(eb)) else 99
                    for c in keys:
                        n += 1
                        ec = ext[c]; oxc = glyphs[idx[c]]["ox"]
                        s2 = step[(b, c)]
                        sh2 = set(eb) & set(ec)
                        g2 = min((s1 + s2 + oxc + ec[y][0]) -
                                 (s1 + oxb + eb[y][1]) - 1
                                 for y in sh2) if sh2 else 99
                        if g1 < 1 or g2 < 1:
                            bad_adj += 1
                        # OUTER pair: A's ink vs C's ink, across B
                        shac = set(ea) & set(ec)
                        if shac:
                            gac = min((s1 + s2 + oxc + ec[y][0]) -
                                      (oxa + ea[y][1]) - 1 for y in shac)
                            if gac < ac_worst:
                                ac_worst = gac; ac_ex = a + b + c
                            if gac < 1:
                                ac_touch += 1
            print(f"{name:<14}{n:>10}{bad_adj:>13}{ac_touch:>11}"
                  f"{ac_worst:>8}px {ac_ex}")
        return 0

    if args.target is not None:
        # Solve per atlas. Tracking shifts every gap by (nearly) itself, but
        # the pen rounds to whole pixels, so the relationship is a staircase —
        # sweep rather than divide, and take the smallest tracking that
        # reaches the target so we never over-space.
        print(f"\n--- tracking to put the soft mean gap at {args.target:.2f}px ---")
        print("TRACKING_PX = {")
        for path in files:
            glyphs, data, kern, _ = load_atlas(path)
            name = os.path.basename(path).replace("nunito_", "").replace(".h", "")
            bold = "bold" in name
            px = int(name.split("_")[-1])
            best, best_err = None, None
            for t64 in range(0, 129):
                gaps = []
                for a in letters:
                    for b in letters:
                        ia, ib = ord(a) - 0x20, ord(b) - 0x20
                        if ia not in glyphs or ib not in glyphs:
                            continue
                        g = gap(glyphs[ia], glyphs[ib], data, kern, t64,
                                ia, ib, 1)
                        if g is not None:
                            gaps.append(g)
                m = sum(gaps) / len(gaps)
                err = abs(m - args.target)
                if best_err is None or err < best_err:
                    best, best_err = t64, err
            print(f"    ({bold}, {px:2d}): {best/64:.2f},"
                  f"   # {name}: {best}/64, soft mean err {best_err:+.2f}px")
        print("}")
        return 0

    n = args.worst
    for name, (core, soft) in report.items():
        tight = [p for g, p in core if g <= 0][:n]
        airy = [f"{p}({g})" for g, p in reversed(core[-n:])]
        print(f"\n{name}")
        print(f"  tightest (cores touching): {' '.join(tight) if tight else '-'}")
        print(f"  airiest:                   {' '.join(airy)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
