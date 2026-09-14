#!/usr/bin/env python3
"""
Faithful 320x240 renderer for the Core (iPod firmware) "Linen" warm-light UI.

Reproduces the on-device look of core/kernel/main.c + core/ui/screen_settings.c:
the status strip, titled headers, list rows (single + two-line with art chips),
album detail, now-playing, the volume overlay, the Hold lock/unlock banner, the
About dashboard and the Boot Details diagnostics page.

Text is drawn with the real Nunito faces via PIL, gamma-correct (sRGB->linear,
blend by glyph coverage, re-encode) so it reads as crisply as the device's
gamma-aware atlas — and it lays glyphs out with the DEVICE's metrics rather
than PIL's defaults (see the Fonts section): tracking, the KERN TABLE and the
per-pair pen rounding all come out of core/ui/atlas/*.h at runtime, so a
screenshot cannot quietly drift from the firmware. The kerning in particular
is the atlas's optically solved table, not the font's own — deriving it from
the font (what this did until 2026-09-14) drew spacing the panel has never
shown. Album art is quantized to RGB565 to match the panel.

Outputs PNGs (3x, NEAREST) + a marquee demo GIF into this directory.

Regenerate:  tools/.venv/bin/python3 docs/screens/render.py
"""

import os
import re
import subprocess
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
# Where the PNGs/GIFs land. Defaults to this directory (the shipped gallery);
# CORE_SCREENS_OUT lets a work-in-progress run write somewhere else.
OUT = os.environ.get("CORE_SCREENS_OUT", HERE)
os.makedirs(OUT, exist_ok=True)
# Repo-relative, not an absolute path into one person's home directory (which
# is what this was, and which meant the script only ran on one machine).
# CORE_FONTS_DIR overrides it if the faces ever live somewhere else.
REPO = os.path.dirname(os.path.dirname(HERE))
FONTS = os.environ.get("CORE_FONTS_DIR", os.path.join(REPO, "tools", "fonts-src"))
ATLAS = os.path.join(REPO, "core", "ui", "atlas")
ART = os.path.join(HERE, "art")


def _git_describe(args, fallback):
    """One `git describe` against the repo, with meson's fallback semantics.

    The firmware's two version strings come from meson vcs_tag (core/meson.build),
    which runs git describe at build time and substitutes a fallback when git
    fails or is absent. The gallery used to paint a hard-coded hash, which meant
    the stills drifted from the firmware the moment anything was committed; run
    the same commands here instead, with the same fallbacks, so a still shows
    what a build off this tree would show.
    """
    try:
        out = subprocess.run(["git", "describe"] + args, cwd=REPO,
                             capture_output=True, text=True, check=True)
        return out.stdout.strip() or fallback
    except (OSError, subprocess.CalledProcessError):
        # No git, not a checkout, or — for --abbrev=0 — a repo with no tags
        # yet, which is a non-zero exit with empty output.
        return fallback


# Mirrors core/meson.build: CORE_BUILD_ID (full stamp, boot screen + Boot
# Details header) and CORE_VERSION (nearest tag alone, the About chip).
BUILD_ID = _git_describe(["--tags", "--always", "--dirty", "--abbrev=7"], "unknown")
VERSION = _git_describe(["--tags", "--abbrev=0"], "v0.0.0")

# Release override. For the gallery that ships with a tagged commit the stills
# have to show the tag the commit is about to get, which `git describe` cannot
# know yet (the tag does not exist while the render runs). Set these and the
# value is used verbatim, no git call.
BUILD_ID = os.environ.get("CORE_STAMP_BUILD_ID", BUILD_ID)
VERSION = os.environ.get("CORE_STAMP_VERSION", VERSION)

if not os.path.isdir(FONTS):
    raise SystemExit(
        f"font directory not found: {FONTS}\n"
        f"Expected the Nunito faces in <repo>/tools/fonts-src; set "
        f"CORE_FONTS_DIR to point somewhere else.")

W, H = 320, 240
SCALE = 3          # PNG upscale (NEAREST)
GIF_SCALE = 2      # GIF upscale (keep file small)

# ---------------------------------------------------------------------------
# Palette — the Linen theme (RGB565 -> RGB888)
# ---------------------------------------------------------------------------
def rgb565(v):
    r = (v >> 11) & 0x1F
    g = (v >> 5) & 0x3F
    b = v & 0x1F
    return (round(r * 255 / 31), round(g * 255 / 63), round(b * 255 / 31))

BATT_RED = rgb565(0xE125)   # low-battery warning, theme-independent

# Seven themes, all RGB565, values transcribed verbatim from core/ui/palette.c
# (PAL_LINEN ... PAL_MUSHROOM). SEL_BG/SEL_FG derive from INK/SURFACE so the
# inverted selection bar reads right in every one (light themes: dark bar/
# light text; dark themes: the reverse). apply_palette() rebinds the module
# globals the screen builders read.
LINEN = dict(
    SURFACE=0xF79D, INK=0x18A2, MUTED=0x7B8D, MUTED2=0x9C70, MUTED_D=0x5A89,
    ACCENT=0xC348, BORDER=0xE71B, PLATE=0xF7BE, TRK=0xDEDA, SB_TRK=0xE73C,
    SB_THMB=0xAD34, SEL_SUB=0xB595, CHEVRON=0xB575, SEL_TRK=0x41E7,
    PILL_OFF=0xCE58,
)
ONYX = dict(
    SURFACE=0x18C2, INK=0xEF3C, MUTED=0xACF2, MUTED2=0xB533, MUTED_D=0x8C0E,
    ACCENT=0xC348, BORDER=0x3185, PLATE=0x2944, TRK=0x39A5, SB_TRK=0x2924,
    SB_THMB=0x6B0B, SEL_SUB=0x5A89, CHEVRON=0x4A27, SEL_TRK=0xCE16,
    PILL_OFF=0x39A5,
)
SAGE = dict(
    SURFACE=0x31A6, INK=0xEF3B, MUTED=0xAD74, MUTED2=0x7C2F, MUTED_D=0xC657,
    ACCENT=0xC3CA, BORDER=0x3A28, PLATE=0x3A07, TRK=0x4248, SB_TRK=0x3A07,
    SB_THMB=0x6BCD, SEL_SUB=0x6B4C, CHEVRON=0x5B2B, SEL_TRK=0xCE58,
    PILL_OFF=0x4248,
)
PLASTER = dict(
    SURFACE=0xE6DA, INK=0x3965, MUTED=0x8B8D, MUTED2=0xA491, MUTED_D=0x5A48,
    ACCENT=0x8A06, BORDER=0xDE57, PLATE=0xEF3B, TRK=0xD657, SB_TRK=0xDE98,
    SB_THMB=0xB4F2, SEL_SUB=0xB553, CHEVRON=0xB533, SEL_TRK=0x5248,
    PILL_OFF=0xCDF6,
)
OLIVE = dict(
    SURFACE=0xE71A, INK=0x2964, MUTED=0x6B8C, MUTED2=0x8C90, MUTED_D=0x4A67,
    ACCENT=0xABA6, BORDER=0xD6B8, PLATE=0xEF5B, TRK=0xCE77, SB_TRK=0xDED9,
    SB_THMB=0xA511, SEL_SUB=0xAD53, CHEVRON=0xA532, SEL_TRK=0x4A48,
    PILL_OFF=0xC616,
)
UMBER = dict(
    SURFACE=0x2903, INK=0xEF1A, MUTED=0xB512, MUTED2=0x8BCE, MUTED_D=0xCDF6,
    ACCENT=0xCC49, BORDER=0x3985, PLATE=0x3165, TRK=0x39A6, SB_TRK=0x3144,
    SB_THMB=0x7B2B, SEL_SUB=0x62CA, CHEVRON=0x5A68, SEL_TRK=0xCE16,
    PILL_OFF=0x39A6,
)
MUSHROOM = dict(
    SURFACE=0xE6FA, INK=0x3144, MUTED=0x7BAD, MUTED2=0x9C91, MUTED_D=0x5289,
    ACCENT=0xA2C8, BORDER=0xD678, PLATE=0xEF5C, TRK=0xD678, SB_TRK=0xDEB9,
    SB_THMB=0xAD12, SEL_SUB=0xAD54, CHEVRON=0xB553, SEL_TRK=0x4A28,
    PILL_OFF=0xC617,
)
_PAL_KEYS = list(LINEN.keys()) + ["SEL_BG", "SEL_FG"]

# THEME_* id order (core/ui/palette.h / settings.c THEME_L) — the id IS the
# row in the picker. Name + one-line subtitle come from screen_settings.c's
# TH_SUB[]; the palette dict is that theme's own PAL_* table.
THEMES = [
    ("Linen",    "Warm light - text-forward",      LINEN),
    ("Onyx",     "Warm dark - terracotta",          ONYX),
    ("Sage",     "Dark green-grey - clay",          SAGE),
    ("Plaster",  "Pink-beige limewash - oxblood",   PLASTER),
    ("Olive",    "Greige-olive - burnt ochre",       OLIVE),
    ("Umber",    "Espresso - caramel",               UMBER),
    ("Mushroom", "Warm greige - muted rust",         MUSHROOM),
]

def apply_palette(spec):
    g = globals()
    for k, v in spec.items():
        g[k] = rgb565(v)
    g["SEL_BG"] = g["INK"]        # selection bar fill
    g["SEL_FG"] = g["SURFACE"]    # text on selection bar

# default active theme = Linen (the stills are Linen)
apply_palette(LINEN)

# ---------------------------------------------------------------------------
# Gamma-correct text blend
# ---------------------------------------------------------------------------
def _s2l(c):
    cs = c / 255.0
    return cs / 12.92 if cs <= 0.04045 else ((cs + 0.055) / 1.055) ** 2.4

LIN = [_s2l(i) for i in range(256)]

def _l2s(v):
    if v <= 0.0:
        return 0
    if v >= 1.0:
        return 255
    cs = v * 12.92 if v <= 0.0031308 else 1.055 * (v ** (1 / 2.4)) - 0.055
    return int(cs * 255 + 0.5)

# ---------------------------------------------------------------------------
# Fonts — the DEVICE's text metrics, not PIL's defaults
# ---------------------------------------------------------------------------
# core/ui/text.c lays a string out in 26.6 fixed point: it carries the
# fractional advance across glyphs (rounding the pen only when a glyph is
# blitted), adds the face's kerning pair adjustment, and adds a per-atlas
# TRACKING between glyphs — suppressed on either side of a space. None of that
# is what PIL does by default, and a renderer that guesses at it produces
# screenshots that disagree with the panel by several pixels per string (which
# is exactly what these did).
#
# So mirror the generator: tools/atlas_gen.py derives every advance from THIS
# Pillow, through getlength() under the Raqm layout engine, then quantizes it.
# Doing the same arithmetic here means the advances below are the ones baked
# into core/ui/atlas/*.h, not an approximation of them.
#
# The basic layout engine is not an option: it applies no kerning at all and
# returns advances already rounded to whole pixels.
#
# KERNING IS READ OUT OF THE ATLAS, not re-derived from the font. It used to
# be re-derived (getlength(ab) - getlength(a) - getlength(b)), which is the
# FONT's kerning — but the generator overrides every letter pair with an
# optically solved value (atlas_gen.optical_kern), so the font's number and
# the device's number are different for thousands of pairs. The renderer
# therefore drew spacing the panel has never shown, and a spacing bug in the
# atlas ('Fa' tucked under the F's arm) was invisible in this gallery. Parsing
# `<face>_KERN[]` out of the generated header is the only way these stills can
# be evidence about the device. Same reason `layout()` rounds the pen ONCE PER
# PAIR below instead of carrying a 26.6 pen: that is text.c's pen_step().
ADV_ONE     = 64      # 26.6, == ATLAS_ADV_ONE   (core/ui/atlas.h)
KERN_TO_ADV = 2       # atlas_kern_t.adj is 1/32 px -> 26.6 (core/ui/atlas.h)


def _atlas_field(header, field):
    """Read `.field = N,` out of a generated atlas header. Loud on absence: a
    missing header or a renamed field must not silently degrade to untracked,
    unkerned text that looks fine in isolation and wrong beside the device."""
    path = os.path.join(ATLAS, header)
    try:
        with open(path, encoding="utf-8") as f:
            src = f.read()
    except OSError as e:
        raise SystemExit(
            f"atlas header not readable: {path} ({e})\n"
            f"The screenshots take their text metrics from the generated "
            f"atlases; regenerate them with tools/atlas_gen.sh.")
    m = re.search(r"\.%s\s*=\s*(-?\d+)" % field, src)
    if not m:
        raise SystemExit(f"no '.{field} =' in {path} — atlas format changed?")
    return int(m.group(1))


def _atlas_charset():
    """Codepoint -> glyph index for everything the atlases carry: printable
    ASCII at 0x20+i -> i, plus whatever tools/atlas_gen.py appended (read from
    the generated glyphmap, so it cannot drift from the firmware). Anything
    outside this renders as a .notdef box on the device, which is a different
    picture from what PIL would draw. The index is what the kern table is
    keyed by, so this is also the lookup `Face.kern()` needs."""
    cps = {0x20 + i: i for i in range(95)}
    path = os.path.join(ATLAS, "glyphmap.h")
    try:
        with open(path, encoding="utf-8") as f:
            src = f.read()
    except OSError as e:
        raise SystemExit(f"atlas glyphmap not readable: {path} ({e})")
    rows = re.findall(r"\{\s*0x([0-9A-Fa-f]{4})\s*,\s*(\d+)\s*\}", src)
    if not rows:
        raise SystemExit(f"no codepoint rows in {path} — atlas format changed?")
    cps.update({int(h, 16): int(i) for h, i in rows})
    return cps

ATLAS_CHARS = _atlas_charset()


def _atlas_kern(header):
    """`{ left, right, adj }` rows of the generated `<face>_KERN[]` table, as
    {(left_idx, right_idx): adj} in 1/32 px. Absent table = no kerning at all,
    which the atlas writes as a single {0,0,0} placeholder; anything else
    missing is a format change and must be loud, not silently unkerned."""
    path = os.path.join(ATLAS, header)
    try:
        with open(path, encoding="utf-8") as f:
            src = f.read()
    except OSError as e:
        raise SystemExit(f"atlas header not readable: {path} ({e})")
    m = re.search(r"_KERN\[\d+\] = \{(.*?)\};", src, re.S)
    if not m:
        raise SystemExit(f"no '_KERN[...] = {{' in {path} — atlas format changed?")
    return {(int(l), int(r)): int(adj) for l, r, adj in
            re.findall(r"\{ *(\d+), *(\d+), *(-?\d+) \}", m.group(1))}


def _atlas_space_advance(header):
    """Glyph 0's (the space's) advance in 26.6, out of the generated header.
    atlas_gen.py's fit_space_advance() overrides the font's own space width,
    so PIL's getlength(" ") is NOT what the device steps by — every word gap
    in the gallery was 1.2–2.3 px narrower than the panel's until this read
    the atlas value. Loud on absence, like the other atlas readers."""
    path = os.path.join(ATLAS, header)
    with open(path, encoding="utf-8") as f:
        src = f.read()
    m = re.search(r"\[ *0\] = \{[^}]*\.advance = *(\d+)", src)
    if not m:
        raise SystemExit(f"no glyph [0] advance in {path} — atlas format changed?")
    return int(m.group(1))


class Face:
    """One (family, size) — the host-side twin of a core/ui/atlas/*.h atlas."""

    def __init__(self, ttf, size, header):
        self.font = ImageFont.truetype(os.path.join(FONTS, ttf), size,
                                       layout_engine=ImageFont.Layout.RAQM)
        self.name = "%s@%d" % (ttf, size)
        self.tracking = _atlas_field(header, "tracking")     # 26.6
        self.ascent = _atlas_field(header, "ascent")
        self.line_height = _atlas_field(header, "line_height")
        self._adv = {" ": _atlas_space_advance(header)}   # the device's space, not PIL's
        self._kern = _atlas_kern(header)          # (left idx, right idx) -> 1/32 px
        self._len = {}

    def _length(self, s):
        v = self._len.get(s)
        if v is None:
            v = self._len[s] = self.font.getlength(s)
        return v

    def advance(self, ch):
        """Unkerned pen advance in 26.6 — atlas_gen.py's `advance` field."""
        a = self._adv.get(ch)
        if a is None:
            if ord(ch) not in ATLAS_CHARS:
                raise SystemExit(
                    f"{ch!r} (U+{ord(ch):04X}) is not in the atlas: the device "
                    f"would draw a .notdef box here. Add it to "
                    f"tools/atlas_gen.py EXTRAS or keep it out of the mock-ups.")
            a = self._adv[ch] = int(round(self._length(ch) * ADV_ONE))
        return a

    def kern(self, a, b):
        """Kern for the ordered pair in 26.6, straight out of the atlas's own
        `_KERN[]` table (adj is 1/32 px there, 26.6 here — ATLAS_KERN_TO_ADV).
        Deriving it from the font instead would be the FONT's kerning, which
        the generator's optical pass has overridden for every letter pair."""
        pair = (ATLAS_CHARS[ord(a)], ATLAS_CHARS[ord(b)])
        return self._kern.get(pair, 0) * KERN_TO_ADV

    def _pen_step(self, a, b):
        """Whole-pixel step from glyph `a` to glyph `b` — core/ui/text.c
        pen_step(): a's advance plus the pair's tracking and kerning, rounded
        ONCE. `b` may be None for the end of the string (no pair)."""
        adv = self.advance(a)
        if b is None:
            return (adv + ADV_ONE // 2) >> 6
        # A space counts as ONE tracking unit (text.c track_adv): tracking
        # goes in on the way INTO a space, not on the way out. Both sides
        # would widen every word gap by twice the tracking; neither side grows
        # the letter gaps while the word gap stays put, which is what made
        # words merge. One side moves the word gap by the same amount as every
        # letter gap, so the ratio the face was designed with survives.
        # Kerning still applies across a space if the face has such a pair.
        track = 0 if a == " " else self.tracking
        return (adv + track + self.kern(a, b) + ADV_ONE // 2) >> 6

    def layout(self, s):
        """(glyphs, width): glyphs is [(char, x offset in px)], width is the
        pen past the last glyph — core/ui/text.c's contract (text_draw puts
        each glyph at the pen, and text_width returns the same total). The pen
        is WHOLE pixels and every step is rounded on its own; carrying a 26.6
        pen and rounding at blit time (what this did before) puts identical
        pairs at different distances in one word, which is the quantisation
        artefact text.c's pen_step() exists to remove."""
        pen = 0
        prev = None
        out = []
        for ch in s:
            if prev is not None:
                pen += self._pen_step(prev, ch)
            out.append((ch, pen))
            prev = ch
        if prev is not None:
            pen += self._pen_step(prev, None)
        return out, pen

    def width(self, s):
        return self.layout(s)[1] if s else 0


# The six faces the firmware ships (core/ui/text.h): the set changed under
# this script once — regular_13 / bold_11 / bold_17 became regular_12 /
# bold_12 / bold_18 — and every screenshot silently stopped rendering. The
# names below are the atlas names, so a mismatch fails loudly at import.
regular_9  = Face("Nunito-Regular.ttf",  9, "nunito_regular_9.h")
regular_11 = Face("Nunito-Regular.ttf", 11, "nunito_regular_11.h")
regular_12 = Face("Nunito-Regular.ttf", 12, "nunito_regular_12.h")
bold_12    = Face("Nunito-Bold.ttf",    12, "nunito_bold_12.h")
bold_13    = Face("Nunito-Bold.ttf",    13, "nunito_bold_13.h")
bold_18    = Face("Nunito-Bold.ttf",    18, "nunito_bold_18.h")

# ui/chrome.h aliases, same names.
FONT_SMALL  = regular_9
FONT_SUB    = regular_11
FONT_ROW    = regular_12
FONT_HEADER = bold_13
FONT_TITLE  = bold_18
# Kept for the call sites that named the old faces; the firmware's list-row
# right values and the Now Playing state label are bold_12 today.
bold_11     = bold_12
regular_13  = regular_12
bold_17     = bold_18

# glyphs used as literals on-device
LAQUO = "‹"   # 'single left angle quote
RAQUO = "›"
MIDDOT = "·"

def text_width(s, font):
    return font.width(s)


UI_ELLIPSIS = "\u2026"


def text_ellipsis_fit(s, font, max_w):
    """chrome.c ui_text_ellipsis_fit: `s` itself when it fits in max_w,
    otherwise the longest prefix that fits WITH a trailing ellipsis, shortened
    a codepoint at a time and with trailing spaces dropped ("of the\u2026", never
    "of the \u2026"). The whole candidate is re-measured each step, as on device,
    because kerning makes per-glyph sums drift. "" when nothing fits."""
    if max_w <= 0:
        return ""
    if text_width(s, font) <= max_w:
        return s
    n = len(s)
    while n > 0:
        n -= 1
        while n > 0 and s[n - 1] == " ":
            n -= 1
        cand = s[:n] + UI_ELLIPSIS
        if text_width(cand, font) <= max_w:
            return cand
    return ""

# ---------------------------------------------------------------------------
# Surface / primitives
# ---------------------------------------------------------------------------
class Screen:
    def __init__(self, bg=None):
        # resolve the surface at call time so the active palette wins (a default
        # arg would freeze the Linen surface captured at def time)
        if bg is None:
            bg = SURFACE
        self.img = Image.new("RGB", (W, H), bg)
        self.px = self.img.load()

    # -- rectangles ------------------------------------------------------
    def fill_rect(self, x, y, w, h, c):
        x0 = max(0, x); y0 = max(0, y)
        x1 = min(W, x + w); y1 = min(H, y + h)
        if x1 <= x0 or y1 <= y0:
            return
        ImageDraw.Draw(self.img).rectangle([x0, y0, x1 - 1, y1 - 1], fill=c)

    def _isqrt(self, v):
        r = 0
        while (r + 1) * (r + 1) <= v:
            r += 1
        return r

    def fill_round_rect(self, x, y, w, h, r, c):
        """Integer-inset rounded rect — matches main.c fill_round_rect exactly."""
        if r < 1:
            self.fill_rect(x, y, w, h, c)
            return
        if 2 * r > w:
            r = w // 2
        if 2 * r > h:
            r = h // 2
        for ry in range(h):
            inset = 0
            k = -1
            if ry < r:
                k = ry
            elif ry >= h - r:
                k = h - 1 - ry
            if k >= 0:
                dy = r - k
                inset = r - self._isqrt(r * r - dy * dy)
            self.fill_rect(x + inset, y + ry, w - 2 * inset, 1, c)

    def fill_round_rect_aa(self, x, y, w, h, r, c):
        """AA rounded rect for plates: solid body + 4x4 supersampled corners
        blended onto the existing framebuffer (matches main.c)."""
        if r < 1:
            self.fill_rect(x, y, w, h, c)
            return
        if 2 * r > w:
            r = w // 2
        if 2 * r > h:
            r = h // 2
        self.fill_rect(x, y + r, w, h - 2 * r, c)
        S = 4
        cN = r * 2 * S
        lr, lg, lb = LIN[c[0]], LIN[c[1]], LIN[c[2]]
        for ry in range(r):
            self.fill_rect(x + r, y + ry, w - 2 * r, 1, c)
            self.fill_rect(x + r, y + h - 1 - ry, w - 2 * r, 1, c)
            for rx in range(r):
                inside = 0
                for sy in range(S):
                    dy = ry * 2 * S + sy * 2 + 1 - cN
                    for sx in range(S):
                        dx = rx * 2 * S + sx * 2 + 1 - cN
                        if dx * dx + dy * dy <= cN * cN:
                            inside += 1
                if inside == 0:
                    continue
                a = inside / (S * S)
                for px_ in (x + rx, x + w - 1 - rx):
                    for py_ in (y + ry, y + h - 1 - ry):
                        if 0 <= px_ < W and 0 <= py_ < H:
                            if a >= 1.0:
                                self.px[px_, py_] = c
                            else:
                                br, bg, bb = self.px[px_, py_]
                                self.px[px_, py_] = (
                                    _l2s(lr * a + LIN[br] * (1 - a)),
                                    _l2s(lg * a + LIN[bg] * (1 - a)),
                                    _l2s(lb * a + LIN[bb] * (1 - a)),
                                )

    def fill_disc_aa(self, cx, cy, r, c):
        """AA filled disc of radius r centred on the pixel EDGE (cx, cy) — it
        covers pixels cx-r .. cx+r-1. Mirrors main.c fill_disc_aa EXACTLY (the
        boot mark's ring/dot/hole): 4x4 sub-sample centres at 8*(p - c) + 2k + 1,
        inside when dx^2 + dy^2 <= (8r)^2, solid at 16/16, otherwise blended
        onto what is already there. NOT fill_round_rect_aa — the firmware's
        corner mask stops at UI_RR_MAX_R (16) and the ring is 19, which is why
        the device has a separate painter at all."""
        R2 = (8 * r) * (8 * r)
        lr, lg, lb = LIN[c[0]], LIN[c[1]], LIN[c[2]]
        for py_ in range(cy - r - 1, cy + r + 1):
            if py_ < 0 or py_ >= H:
                continue
            for px_ in range(cx - r - 1, cx + r + 1):
                if px_ < 0 or px_ >= W:
                    continue
                inside = 0
                for j in range(4):
                    dy = 8 * (py_ - cy) + 2 * j + 1
                    for i in range(4):
                        dx = 8 * (px_ - cx) + 2 * i + 1
                        if dx * dx + dy * dy <= R2:
                            inside += 1
                if inside == 0:
                    continue
                if inside >= 16:
                    self.px[px_, py_] = c
                    continue
                a = inside / 16.0
                br, bg, bb = self.px[px_, py_]
                self.px[px_, py_] = (
                    _l2s(lr * a + LIN[br] * (1 - a)),
                    _l2s(lg * a + LIN[bg] * (1 - a)),
                    _l2s(lb * a + LIN[bb] * (1 - a)),
                )

    # -- text (gamma-correct) -------------------------------------------
    def text(self, x, baseline, s, font, ink, clip=None):
        if not s:
            return x
        # Glyph BY GLYPH at the device's pen positions (the atlas's kerning and
        # tracking, one rounding per pair) — handing PIL the whole string would
        # lay it out with the font's own kerning and none of the rest. Each
        # glyph goes down at its own pen x, which is what text.c does before it
        # blits.
        glyphs, width = font.layout(s)
        mask = Image.new("L", (W, H), 0)
        md = ImageDraw.Draw(mask)
        for ch, dx in glyphs:
            if ch == " ":
                continue
            md.text((x + dx, baseline), ch, font=font.font, fill=255, anchor="ls")
        bbox = mask.getbbox()
        if bbox:
            mp = mask.load()
            x0, y0, x1, y1 = bbox
            if clip:
                x0 = max(x0, clip[0])
                x1 = min(x1, clip[1])
            lir, lig, lib = LIN[ink[0]], LIN[ink[1]], LIN[ink[2]]
            for yy in range(y0, y1):
                for xx in range(x0, x1):
                    a = mp[xx, yy]
                    if not a:
                        continue
                    if a == 255:
                        self.px[xx, yy] = ink
                        continue
                    af = a / 255.0
                    br, bg, bb = self.px[xx, yy]
                    self.px[xx, yy] = (
                        _l2s(lir * af + LIN[br] * (1 - af)),
                        _l2s(lig * af + LIN[bg] * (1 - af)),
                        _l2s(lib * af + LIN[bb] * (1 - af)),
                    )
        return x + width

    def text_centered(self, baseline, s, font, ink):
        self.text((W - text_width(s, font)) // 2, baseline, s, font, ink)

    def text_right(self, right_x, baseline, s, font, ink):
        self.text(right_x - text_width(s, font), baseline, s, font, ink)

    # -- album art -------------------------------------------------------
    def blit_art(self, x, y, dim, art_name, round_r=0):
        im = load_art(art_name, dim)
        self.img.paste(im, (x, y))
        if round_r > 0:
            # knock outer corner pixels back to background (surface)
            bg = SURFACE
            for dy in range(round_r):
                for dx in range(round_r):
                    if dx + dy >= round_r:
                        continue
                    self.px[x + dx, y + dy] = bg
                    self.px[x + dim - 1 - dx, y + dy] = bg
                    self.px[x + dx, y + dim - 1 - dy] = bg
                    self.px[x + dim - 1 - dx, y + dim - 1 - dy] = bg


# ---------------------------------------------------------------------------
# Album art cache — RGB565-quantized to match the panel
# ---------------------------------------------------------------------------
_ART_CACHE = {}

def load_art(name, dim):
    key = (name, dim)
    if key in _ART_CACHE:
        return _ART_CACHE[key]
    src = Image.open(os.path.join(ART, name + ".png")).convert("RGB")
    im = src.resize((dim, dim), Image.LANCZOS)
    # quantize to RGB565 (5/6/5) like the device framebuffer
    px = im.load()
    for y in range(dim):
        for x in range(dim):
            r, g, b = px[x, y]
            r = (r >> 3); g = (g >> 2); b = (b >> 3)
            px[x, y] = (round(r * 255 / 31), round(g * 255 / 63), round(b * 255 / 31))
    _ART_CACHE[key] = im
    return im


# ---------------------------------------------------------------------------
# Shared chrome
# ---------------------------------------------------------------------------
STATUS_H = 15
HDR_BASE = 30
HDR_DIV_Y = 38
LIST_Y0 = 42
ROW_H = 24
ROW_H2 = 32
LIST_ROWS = 8
LIST_ROWS2 = 6

def draw_battery(sc, x, y, pct):
    w, h = 22, 12
    sc.fill_rect(x, y, w, 1, MUTED2)
    sc.fill_rect(x, y + h - 1, w, 1, MUTED2)
    sc.fill_rect(x, y, 1, h, MUTED2)
    sc.fill_rect(x + w - 1, y, 1, h, MUTED2)
    sc.fill_rect(x + w, y + 4, 2, h - 8, MUTED2)
    pct = max(0, min(100, pct))
    fw = ((w - 4) * pct) // 100
    if fw > 0:
        sc.fill_rect(x + 2, y + 2, fw, h - 4, BATT_RED if pct <= 20 else INK)

def draw_lock_glyph(sc, x, y, c):
    sc.fill_rect(x, y + 4, 8, 6, c)
    sc.fill_rect(x + 1, y, 2, 5, c)
    sc.fill_rect(x + 5, y, 2, 5, c)
    sc.fill_rect(x + 1, y, 6, 2, c)

def status_strip(sc, left="", pct=78, locked=False):
    # main.c status_strip_render: the playing track's name, clipped before the
    # right cluster (12, LCD_WIDTH-70) rather than drawn full width and painted
    # over — and NOTHING when nothing is playing (the strip is a now-playing
    # readout, not a wordmark; the main menu's own header says "Core").
    if left:
        sc.text(12, STATUS_H - 4, left, FONT_SMALL, MUTED2, clip=(12, W - 70))
    bx = W - 12 - 24
    draw_battery(sc, bx, 1, pct)
    if locked:
        draw_lock_glyph(sc, bx - 14, 3, INK)

def header(sc, title, right=None, back=False):
    x = 12
    if back:
        x = sc.text(x, HDR_BASE, LAQUO, FONT_HEADER, MUTED2) + 4
    sc.text(x, HDR_BASE, title, FONT_HEADER, INK)
    if right:
        sc.text_right(W - 12, HDR_BASE - 1, right, FONT_SMALL, MUTED2)
    sc.fill_rect(12, HDR_DIV_Y, W - 24, 1, BORDER)

def scrollbar(sc, y0, top, visible, total):
    if total <= visible:
        return
    track_y = y0
    track_h = H - y0 - 4
    sc.fill_rect(W - 4, track_y, 3, track_h, SB_TRK)
    thumb_h = max(16, (visible * track_h) // total)
    denom = max(1, total - visible)
    thumb_y = track_y + (top * (track_h - thumb_h)) // denom
    sc.fill_rect(W - 4, thumb_y, 3, thumb_h, SB_THMB)


# ---------------------------------------------------------------------------
# List rows
# ---------------------------------------------------------------------------
def list_row(sc, y0, r, text, sub=None, right=None, chevron=False,
             selected=False, greyed=False, chip=None, rh=ROW_H,
             title_offset=0, title_priority=False):
    ry = y0 + r * rh
    rowmid = ry + rh // 2 + 3
    if selected:
        sc.fill_round_rect(6, ry + 1, W - 16, rh - 2, 4, SEL_BG)
        fg, subc, rightc, chevc = SEL_FG, SEL_SUB, SEL_SUB, SEL_SUB
    else:
        fg = MUTED if greyed else INK
        subc, rightc, chevc = MUTED2, MUTED_D, CHEVRON

    tx = 14
    if chip:
        cd = 28
        cy = ry + (rh - cd) // 2
        sc.blit_art(12, cy, cd, chip, round_r=2)
        cbg = SEL_BG if selected else SURFACE
        for dy in range(2):
            for dx in range(2):
                if dx + dy >= 2:
                    continue
                sc.px[12 + dx, cy + dy] = cbg
                sc.px[12 + cd - 1 - dx, cy + dy] = cbg
                sc.px[12 + dx, cy + cd - 1 - dy] = cbg
                sc.px[12 + cd - 1 - dx, cy + cd - 1 - dy] = cbg
        tx = 12 + cd + 8

    tf = FONT_HEADER if selected else FONT_ROW
    show_right = bool(right)
    if right:
        reserved = W - 16 - text_width(right, bold_11) - 6
        # title_priority: a long title spans the full width (over the value) and
        # the value drops, so it truncates/marquees at the row edge (main.c).
        if title_priority and text_width(text, tf) > reserved - tx:
            title_right = W - 16
            show_right = False
        else:
            title_right = reserved
    elif chevron:
        title_right = W - 18 - 4
    else:
        title_right = W - 16
    avail = title_right - tx

    base = (ry + 16) if sub else rowmid
    sub_y = ry + rh - 4
    # marquee/clip: title clipped to [tx, tx+avail); title_offset scrolls it
    sc.text(tx - title_offset, base, text, tf, fg, clip=(tx, tx + avail))
    if sub:
        sc.text(tx, sub_y, sub, FONT_SMALL, subc)
    if show_right:
        sc.text_right(W - 16, rowmid, right, bold_11, rightc)
    elif chevron:
        sc.text(W - 18, rowmid, RAQUO, FONT_ROW, chevc)


# ---------------------------------------------------------------------------
# Screens
# ---------------------------------------------------------------------------
# The firmware sorts the album list A->Z by ALBUM TITLE (the bold main line,
# case-insensitive) — not by artist. Sort here so the render matches.
ALBUMS = sorted([
    ("AUSTIN", "Post Malone", "austin"),
    ("Rearrange My World / There's a Field (That's Only Yours)", "Daniel Caesar", "rearrange"),
    ("F-1 Trillion", "Post Malone", "f1"),
    ("Hollywood's Bleeding", "Post Malone", "hollywood"),
    ("Malibu Nights", "LANY", "malibu"),
    ("Changes", "Justin Bieber", "changes"),
], key=lambda a: a[0].lower())
# keep the selection on the long-titled album (now sorts near the end) so it
# still demonstrates truncation / the marquee.
ALBUMS_SEL = next(i for i, a in enumerate(ALBUMS) if a[0].startswith("Rearrange"))

def screen_albums(sel=ALBUMS_SEL, title_offset=0):
    sc = Screen()
    status_strip(sc)
    header(sc, "Albums", "%d / %d" % (sel + 1, len(ALBUMS)), back=True)
    for r, (t, a, art) in enumerate(ALBUMS):
        s = (r == sel)
        # main.c albumlist_row_draw: list_row_tall(..., chevron=1, right=0) —
        # a disclosure chevron, no per-row right value.
        list_row(sc, LIST_Y0, r, t, sub=a, chevron=True, selected=s, chip=art,
                 rh=ROW_H2, title_offset=title_offset if s else 0)
    scrollbar(sc, LIST_Y0, 0, LIST_ROWS2, len(ALBUMS))
    return sc.img


GENRES = [
    ("Hip-Hop", 214), ("Pop", 186), ("R&B", 98), ("Indie", 74),
    ("Country", 63), ("Pop Punk", 52), ("Alternative", 47), ("Hyperpop", 39),
    ("Folk", 21), ("Rock", 17),
]
GENRES_SEL = 2

def screen_genres():
    sc = Screen()
    status_strip(sc)
    header(sc, "Genres", "%d / %d" % (GENRES_SEL + 1, len(GENRES)), back=True)
    for r in range(LIST_ROWS):
        if r >= len(GENRES):
            break
        name, cnt = GENRES[r]
        list_row(sc, LIST_Y0, r, name, right=str(cnt), selected=(r == GENRES_SEL))
    scrollbar(sc, LIST_Y0, 0, LIST_ROWS, len(GENRES))
    return sc.img


TRACKS = [
    ("Don't Understand", "3:14"),
    ("Something Real", "3:02"),
    ("Chemical", "2:46"),
    ("Novacandy", "3:38"),
    ("Mourning", "2:52"),
]
DETAIL_SEL = 1
# The album has 17 tracks (header count + meta line); only the first 5 fit, and
# the device sizes the scrollbar from the WHOLE tracklist, not the visible slice.
DETAIL_N = 17

def screen_detail(sel=DETAIL_SEL):
    sc = Screen()
    status_strip(sc)
    header(sc, "Albums", "%d / %d" % (sel + 1, DETAIL_N), back=True)
    sc.blit_art(12, 42, 56, "austin")
    tx = 12 + 56 + 12
    sc.text(tx, 42 + 15, "AUSTIN", FONT_HEADER, INK)
    sc.text(tx, 42 + 31, "Post Malone", FONT_SUB, MUTED_D)
    sc.text(tx, 42 + 47, "17 tracks " + MIDDOT + " 52m", FONT_SMALL, MUTED2)
    sc.fill_rect(12, 108 - 6, W - 24, 1, BORDER)
    for r, (t, dur) in enumerate(TRACKS):
        ry = 108 + r * ROW_H
        sel = (r == DETAIL_SEL)
        if sel:
            sc.fill_round_rect(6, ry + 1, W - 16, ROW_H - 2, 4, SEL_BG)
        fg = SEL_FG if sel else INK
        nc = SEL_SUB if sel else MUTED2
        num = str(r + 1)
        sc.text_right(24, ry + 15, num, FONT_SMALL, nc)
        dw = text_width(dur, bold_11)
        sc.text_right(W - 16, ry + 15, dur, bold_11, SEL_SUB if sel else MUTED_D)
        sc.text(30, ry + 15, t, FONT_HEADER if sel else FONT_ROW, fg,
                clip=(30, W - 16 - dw - 8))
    scrollbar(sc, 108, 0, 5, DETAIL_N)
    return sc.img


def _now_playing_base(vol_overlay=None, elapsed=73, total=182, locked=False):
    sc = Screen()
    # top status row
    sc.text(12, 15, "Now Playing", bold_11, INK)
    bx = W - 12 - 19
    draw_battery(sc, bx, 3, 78)
    # Persistent Hold padlock in the strip while locked (main.c: drawn just left
    # of the battery at bx-14). The shuffle token shifts left to clear it.
    if locked:
        draw_lock_glyph(sc, bx - 14, 3, INK)
    sc.text_right(bx - (18 if locked else 6), 13, "SHUF", FONT_SMALL, MUTED2)
    # art 120x120 at (16,44)
    sc.blit_art(16, 44, 120, "austin")
    mx = 16 + 120 + 14
    mr = W - 14
    # eyebrow
    eb = "TRACK   3     OF     12"
    sc.text(mx, 72, eb, FONT_SMALL, MUTED2)
    sc.text(mx, 94, "Something Real", FONT_TITLE, INK, clip=(mx, mr))
    sc.text(mx, 114, "Post Malone", FONT_SUB, MUTED_D, clip=(mx, mr))
    sc.text(mx, 130, "AUSTIN", FONT_SUB, MUTED2, clip=(mx, mr))
    def fmt(s):
        return "%d:%02d" % (s // 60, s % 60)
    sc.text(18, 198, fmt(elapsed), FONT_SUB, MUTED_D)
    sc.text_right(W - 18, 198, "-" + fmt(total - elapsed), FONT_SUB, MUTED_D)
    # progress bar
    pbx, by, bw, bh = 18, 209, W - 36, 8
    sc.fill_round_rect_aa(pbx, by, bw, bh, bh // 2, TRK)
    fw = int(elapsed * bw / total)
    if fw >= bh:
        sc.fill_round_rect_aa(pbx, by, fw, bh, bh // 2, INK)
    elif fw > 0:
        sc.fill_rect(pbx, by, fw, bh, INK)
    if vol_overlay is not None:
        volume_overlay(sc, vol_overlay)
    return sc


def draw_speaker(sc, sx, sy, c, vol):
    sc.fill_round_rect(sx - 8, sy - 3, 4, 6, 1, c)          # cabinet
    for dx in range(5):                                     # cone, opening right
        half = 2 + dx
        sc.fill_rect(sx - 4 + dx, sy - half, 1, 2 * half, c)
    if vol <= 0:                                            # muted: an X (firmware)
        for i in range(6):
            sc.fill_rect(sx + 3 + i, sy - 3 + i, 2, 1, c)   # '\'
            sc.fill_rect(sx + 3 + i, sy + 2 - i, 2, 1, c)   # '/'
        return

    def arc(R, span):                                       # skinny 1px crescents
        for dy in range(-span, span + 1):
            dxx = sc._isqrt(R * R - dy * dy)
            sc.fill_rect(sx + dxx, sy + dy, 1, 1, c)
    if vol > 5:
        arc(3, 2)
    if vol > 40:
        arc(6, 4)
    if vol > 72:
        arc(9, 5)


def volume_overlay(sc, vol):
    PX, PY, PW, PH = 60, 101, 200, 32
    sc.fill_round_rect_aa(PX, PY, PW, PH, 8, PLATE)
    draw_speaker(sc, PX + 16, PY + PH // 2, INK, vol)
    bx = PX + 34
    by = PY + PH // 2 - 3
    bw = PW - 34 - 42
    bh = 6
    sc.fill_rect(bx, by, bw, bh, TRK)
    fw = max(0, min(bw, bw * vol // 100))
    sc.fill_rect(bx, by, fw, bh, INK)
    sc.text_right(PX + PW - 14, PY + PH // 2 + 4, str(vol), bold_11, INK)


def screen_nowplaying():
    return _now_playing_base().img


def screen_volume():
    return _now_playing_base(vol_overlay=78).img


# -- Hold banner -------------------------------------------------------------
# main.c "Top banner": a Hold edge does NOT raise a centred 180x110 plate any
# more. The TOP CHROME INVERTS for LOCK_FLASH_US and then settles back into the
# persistent strip padlock. Ported row for row from top_banner_render /
# lock_banner_render, including the bitmaps and the explicit-colour battery.

# Dot-keyhole padlock, 14 px wide, as row bitmasks (bit 13 = left column).
# Closed = 16 rows; open = 18, the shackle "popped" clear of the body. Values
# verbatim from main.c LOCK_BM_CLOSED / LOCK_BM_OPEN.
LOCK_BM_CLOSED = [
    0x03F0, 0x07F8, 0x0E1C, 0x0C0C, 0x0C0C, 0x0C0C,
    0x1FFE, 0x3FFF, 0x3FFF, 0x3F3F, 0x3F3F, 0x3FFF,
    0x3FFF, 0x3FFF, 0x3FFF, 0x1FFE,
]
LOCK_BM_OPEN = [
    0x03F0, 0x07F8, 0x0E1C, 0x0C0C, 0x0C0C, 0x0C0C,
    0x0000, 0x0000, 0x1FFE, 0x3FFF, 0x3FFF, 0x3F3F,
    0x3F3F, 0x3FFF, 0x3FFF, 0x3FFF, 0x3FFF, 0x1FFE,
]


def draw_bitmap14(sc, x, y, rows, c):
    """main.c draw_bitmap14: blit a 14-wide row-mask bitmap at (x, y) as
    horizontal runs (bit 13 is the leftmost column)."""
    for r, m in enumerate(rows):
        run = -1
        for col in range(15):
            on = col < 14 and (m & (1 << (13 - col)))
            if on and run < 0:
                run = col
            elif not on and run >= 0:
                sc.fill_rect(x + run, y + r, col - run, 1, c)
                run = -1


def draw_battery_c(sc, x, y, pct, outline, fill):
    """main.c draw_battery_c: draw_battery in explicit colours, so the right
    cluster can sit on an inverted band without flickering."""
    w, h = 22, 12
    sc.fill_rect(x, y, w, 1, outline)
    sc.fill_rect(x, y + h - 1, w, 1, outline)
    sc.fill_rect(x, y, 1, h, outline)
    sc.fill_rect(x + w - 1, y, 1, h, outline)
    sc.fill_rect(x + w, y + 4, 2, h - 8, outline)
    pct = max(0, min(100, pct))
    fw = ((w - 4) * pct) // 100
    if fw > 0:
        sc.fill_rect(x + 2, y + 2, fw, h - 4, BATT_RED if pct <= 20 else fill)


def _as_screen(img):
    """Wrap an already-rendered frame so the banner can be painted over its top
    chrome (the screen builders hand back a PIL image, not the Screen)."""
    sc = Screen()
    sc.img = img
    sc.px = img.load()
    return sc


def top_banner(sc, inverted, bm, bm_dy, label, token, screen,
               left="", pct=78):
    """main.c top_banner_render: paint a banner over the top chrome of whatever
    is already in the framebuffer. `inverted` picks the selected-row pair (INK
    band / SURFACE marks / SEL_SUB secondaries), else surface + the ordinary
    border rule. `bm_dy` drops a taller bitmap so its body stays put. `screen`
    is which top chrome the band covers (top_banner_h): "np" = the 22 px status
    row (Now Playing and the chrome-less modals), "list" and "settings" = strip
    + header through the divider (HDR_DIV_Y + 1 = 39) — the firmware keeps the
    strip on Settings too, painted over the painter's clear band. `left` is the
    strip row's track name, blank when nothing is playing. The label is
    ellipsised to the room left of the token, as the header does for a title
    next to its count."""
    band = INK if inverted else SURFACE
    fg = SURFACE if inverted else INK
    sub = SEL_SUB if inverted else MUTED2
    h = 23 if screen == "np" else HDR_DIV_Y + 1

    if h == 23:
        # Now Playing's own top row: glyph + label left, battery right. The
        # un-inverted band gets the border rule under it (row 22) so a surface
        # band on the surface still reads as a banner; the present is 23 rows.
        sc.fill_rect(0, 0, W, 22, band)
        if not inverted:
            sc.fill_rect(0, 22, W, 1, BORDER)
        draw_bitmap14(sc, 12, 3 + bm_dy, bm, fg)
        sc.text(12 + 14 + 6, 15, label, bold_12, fg)
        draw_battery_c(sc, W - 12 - 19, 3, pct, sub, fg)
        return
    sc.fill_rect(0, 0, W, h, band)
    # Strip row, recoloured (every screen with a strip — Settings included),
    # then the header line as the announcement, then the divider in the band's
    # own secondary colour so the inverted block ends where the header does.
    if left:
        sc.text(12, STATUS_H - 4, left, FONT_SMALL, sub, clip=(12, W - 70))
    draw_battery_c(sc, W - 12 - 24, 1, pct, sub, fg)
    draw_bitmap14(sc, 12, HDR_BASE - 12 + bm_dy, bm, fg)
    lx = 12 + 14 + 6
    tok_w = text_width(token, FONT_SMALL) if token else 0
    label_max = (W - 12 - tok_w - 8 if token else W - 12) - lx
    sc.text(lx, HDR_BASE, text_ellipsis_fit(label, FONT_HEADER, label_max),
            FONT_HEADER, fg)
    if token:
        sc.text_right(W - 12, HDR_BASE - 1, token, FONT_SMALL, sub)
    sc.fill_rect(12, HDR_DIV_Y, W - 24, 1, sub if inverted else BORDER)


def lock_banner(sc, locked, screen, **kw):
    """main.c lock_banner_render: locked = inverted + closed padlock +
    "Locked" / HOLD ON; unlocked = surface + popped-open padlock +
    "Unlocked" / HOLD OFF."""
    if locked:
        top_banner(sc, True, LOCK_BM_CLOSED, 0, "Locked", "HOLD ON",
                   screen, **kw)
    else:
        top_banner(sc, False, LOCK_BM_OPEN, -1, "Unlocked", "HOLD OFF",
                   screen, **kw)


def _lock_screen(locked, elapsed=73, glyph=False):
    # `glyph` draws the persistent Hold padlock in the status strip (engaged);
    # while the banner is up the band covers it, which is what the device does.
    sc = _now_playing_base(elapsed=elapsed, locked=glyph)
    lock_banner(sc, locked, "np")
    return sc.img


def screen_lock():
    return _lock_screen(False)

def screen_locked():
    return _lock_screen(True)

def screen_locked_list():
    """The locked banner over list chrome: the strip row keeps its track name
    (blank here — nothing is playing) and the battery, recoloured; the header
    line carries the announcement where the title and the count were."""
    sc = _as_screen(screen_albums())
    lock_banner(sc, True, "list")
    return sc.img


# -- about -------------------------------------------------------------------
def fmt_gb(mb):
    """settings_about_render's fmt_gb: whole megabytes as 'W.F GB'."""
    whole = mb // 1024
    frac = (mb % 1024) * 10 // 1024
    return "%d.%d GB" % (whole, frac)

# Device dashboard values (core/ui/screen_settings.c settings_about_render):
#   ‹ About
#   iPod 5.5G                                  [Core]
#      4127          318           142
#      SONGS        ALBUMS        ARTISTS
#   ┌ STORAGE ──────────┐  ┌ BATTERY ──────────┐
#   │ 21.0 GB free       │  │ 73%                │
#   │ ▓▓▓▓▓▓▓▓▓░░░       │  │ [▓▓▓▓▓▓▓░░]▏       │
#   │ 53.5 of 74.5 GB    │  │ 3912 mV            │
#   └────────────────────┘  └────────────────────┘
#              ADC 2731 · LOG 6 on
AB_SONGS, AB_ALBUMS, AB_ARTISTS = 4127, 318, 142
AB_TOTAL_MB, AB_FREE_MB = 76288, 21504          # -> "74.5 GB" total, "21.0 GB" free
AB_BATT_PCT, AB_BATT_MV, AB_BATT_RAW = 73, 3912, 2731
AB_LOG_SEQ, AB_LOG_ON = 6, True
AB_LIB_TRUNCATED = False

AB_CARD_Y, AB_CARD_H, AB_CARD_W, AB_CARD_PAD = 142, 80, 140, 10   # 16|140|8|140|16=320

def _about_card(sc, x, label):
    sc.fill_round_rect(x, AB_CARD_Y, AB_CARD_W, AB_CARD_H, 6, PLATE)
    sc.text(x + AB_CARD_PAD, AB_CARD_Y + 18, label, FONT_SMALL, MUTED)
    return x + AB_CARD_PAD

def screen_about(lib_truncated=AB_LIB_TRUNCATED):
    sc = Screen()
    header(sc, "About", back=True)
    status_strip(sc)                 # main.c settings_render_cur

    # --- device row: name left, firmware chip right, one baseline ---
    sc.text(16, 66, "iPod 5.5G", FONT_TITLE, INK)
    chip = "Core " + VERSION if VERSION else "Core"
    cw = text_width(chip, FONT_SUB)
    chw, chx, chy = cw + 16, W - 16 - (cw + 16), 52
    sc.fill_round_rect(chx, chy, chw, 16, 8, INK)
    sc.text(chx + 8, chy + 12, chip, FONT_SUB, SURFACE)
    if lib_truncated:
        sc.text_centered(84, "Library too large " + MIDDOT + " some items not shown",
                          FONT_SMALL, BATT_RED)

    # --- three stat columns: Songs / Albums / Artists ---
    lbl = ["SONGS", "ALBUMS", "ARTISTS"]
    val = [AB_SONGS, AB_ALBUMS, AB_ARTISTS]
    colw = W // 3
    for i in range(3):
        cx = colw * i + colw // 2
        v = str(val[i])
        sc.text(cx - text_width(v, FONT_TITLE) // 2, 108, v, FONT_TITLE, INK)
        sc.text(cx - text_width(lbl[i], FONT_SMALL) // 2, 124, lbl[i], FONT_SMALL, MUTED)
        if i:
            sc.fill_rect(colw * i, 94, 1, 36, BORDER)

    # --- STORAGE plate: free space big, used-fraction bar, capacity caption ---
    ix = _about_card(sc, 16, "STORAGE")
    bw = AB_CARD_W - 2 * AB_CARD_PAD
    v = fmt_gb(AB_FREE_MB)
    pen = sc.text(ix, AB_CARD_Y + 44, v, FONT_TITLE, INK)
    sc.text(pen + 5, AB_CARD_Y + 44, "free", FONT_SUB, MUTED_D)
    by, bh = AB_CARD_Y + 52, 6
    sc.fill_round_rect(ix, by, bw, bh, 3, TRK)
    used = AB_TOTAL_MB - AB_FREE_MB
    fw = int(used * bw / AB_TOTAL_MB)
    if fw < bh and used > 0:
        fw = bh
    fw = min(fw, bw)
    sc.fill_round_rect(ix, by, fw, bh, 3, INK)
    used_str = fmt_gb(used).split(" ")[0]          # "53.5 GB" -> "53.5"
    sc.text(ix, AB_CARD_Y + 72, used_str + " of " + fmt_gb(AB_TOTAL_MB), FONT_SMALL, MUTED_D)

    # --- BATTERY plate: percent big, a battery pictogram, millivolts caption ---
    ix = _about_card(sc, 164, "BATTERY")
    sc.text(ix, AB_CARD_Y + 44, str(AB_BATT_PCT) + "%", FONT_TITLE, INK)
    gx, gy, gw, gh = ix, AB_CARD_Y + 50, AB_CARD_W - 2 * AB_CARD_PAD - 4, 10
    sc.fill_round_rect(gx, gy, gw, gh, 3, TRK)
    sc.fill_rect(gx + gw, gy + 3, 3, gh - 6, TRK)
    pct = min(AB_BATT_PCT, 100)
    fw2 = (gw - 4) * pct // 100
    if fw2 < 2 and pct > 0:
        fw2 = 2
    sc.fill_round_rect(gx + 2, gy + 2, fw2, gh - 4, 2, INK)
    sc.text(ix, AB_CARD_Y + 72, str(AB_BATT_MV) + " mV", FONT_SMALL, MUTED_D)

    # --- diagnostics footer: raw ADC code + the event log sequence ---
    v = "ADC " + str(AB_BATT_RAW) + " " + MIDDOT + " "
    v += ("LOG " + str(AB_LOG_SEQ) + " on") if AB_LOG_ON else "LOG off"
    sc.text_centered(236, v, FONT_SMALL, MUTED)
    return sc.img


# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------
def upscale(im, scale):
    return im.resize((W * scale, H * scale), Image.NEAREST)

def save_png(im, name):
    path = os.path.join(OUT, name)
    upscale(im, SCALE).save(path)
    return path


# ---------------------------------------------------------------------------
# Menu screens (main + Music submenu) — real rows from core/kernel/main.c
# ---------------------------------------------------------------------------
MAIN_MENU = [   # (label, active) — idle: "Now Playing" row is hidden
    ("Music", True), ("Playlists", True), ("Podcasts", False),
    ("Audiobooks", False), ("Settings", True),
]
MUSIC_MENU = [
    ("Playlists", True), ("Artists", True), ("Albums", True), ("Songs", True),
    ("Shuffle Songs", True), ("Genres", True), ("Composers", False),
    ("Audiobooks", False),
]

def screen_menu(title, items, sel, back):
    sc = Screen()
    status_strip(sc)
    header(sc, title, back=back)
    for i, (label, active) in enumerate(items):
        if i >= LIST_ROWS:
            break
        list_row(sc, LIST_Y0, i, label, chevron=True, selected=(i == sel),
                 greyed=not active)
    return sc.img


def walkthrough_spec(marquee=True):
    """A little "someone using the iPod" story: main menu -> Music -> Albums
    (with the long title marqueeing) -> album detail -> now playing, with the
    selection bar visibly stepping row to row and the progress bar advancing.

    Returns a `_save_gif` spec so the same sequence can be reused (the hero GIF
    appends it to the boot frames). `marquee=False` drops the title-reveal
    frames, which is the cheapest way to shrink a GIF that carries this story."""
    spec = []

    def add(im, hold=1, ms=140):
        spec.append((im, hold, ms))

    # 1) MAIN MENU — dwell on ACTIVE rows; greyed rows (Podcasts/Audiobooks) are
    #    passed over quickly (1 frame), never selected.
    add(screen_menu("Core", MAIN_MENU, 0, False), hold=4)   # Music (active)
    add(screen_menu("Core", MAIN_MENU, 1, False), hold=2)   # Playlists (active)
    for i in (2, 3):                                         # pass greyed rows
        add(screen_menu("Core", MAIN_MENU, i, False), hold=1)
    add(screen_menu("Core", MAIN_MENU, 4, False), hold=3)   # Settings (active) — pause
    for i in (3, 2, 1):                                      # pass back up
        add(screen_menu("Core", MAIN_MENU, i, False), hold=1)
    add(screen_menu("Core", MAIN_MENU, 0, False), hold=4)   # settle on Music -> enter

    # 2) MUSIC SUBMENU — step from Artists down to Albums.
    add(screen_menu("Music", MUSIC_MENU, 1, True), hold=3)  # Artists
    add(screen_menu("Music", MUSIC_MENU, 2, True), hold=4)  # Albums

    # 3) ALBUMS LIST (sorted A->Z by title) — bar steps DOWN onto the long-titled
    #    album, which now sorts near the end; the marquee then scrolls it.
    LONG = ALBUMS_SEL
    for s in range(0, LONG):                                # step down to the long one
        add(screen_albums(sel=s), hold=2 if s else 3)
    add(screen_albums(sel=LONG, title_offset=0), hold=3)    # long title (start)
    # marquee reveal
    if marquee:
        t = ALBUMS[LONG][0]
        tx = 12 + 28 + 8
        avail = (W - 16) - tx
        max_off = max(0, text_width(t, FONT_HEADER) - avail)
        o = 0
        while o < max_off:
            o = min(max_off, o + 6)
            add(screen_albums(sel=LONG, title_offset=o), hold=1, ms=90)
        add(screen_albums(sel=LONG, title_offset=max_off), hold=3)  # dwell on tail
    add(screen_albums(sel=0), hold=3)                        # bar back to AUSTIN, select

    # 4) ALBUM DETAIL — step down a couple of tracks, settle on "Something Real".
    add(screen_detail(sel=0), hold=3)                        # Don't Understand
    add(screen_detail(sel=1), hold=2)                        # Something Real
    add(screen_detail(sel=2), hold=3)                        # Chemical
    add(screen_detail(sel=1), hold=4)                        # settle -> select

    # 5) NOW PLAYING — the selected song starts; the clock ticks up one second at
    #    a time (elapsed + -remaining = length each frame) and the bar creeps.
    for e in range(0, 6):
        add(_now_playing_base(elapsed=e).img, hold=2, ms=220)
    add(_now_playing_base(elapsed=6).img, hold=5, ms=220)   # hold a beat, then loop

    return spec


def build_walkthrough_gif():
    """demo.gif — the walkthrough on its own."""
    return _save_gif("demo.gif", walkthrough_spec())


# ---------------------------------------------------------------------------
# Per-feature GIFs — short focused loops that pair with the still grids
# ---------------------------------------------------------------------------
def _save_gif(name, spec, colors=96):
    """spec = list of (img, hold_frames, ms). Consecutive identical frames are
    collapsed by the encoder, so holds are cheap."""
    frames, durations = [], []
    for img, hold, ms in spec:
        p = upscale(img, GIF_SCALE).convert("P", palette=Image.ADAPTIVE, colors=colors)
        for _ in range(hold):
            frames.append(p)
            durations.append(ms)
    path = os.path.join(OUT, name)
    frames[0].save(path, save_all=True, append_images=frames[1:], loop=0,
                   duration=durations, optimize=True, disposal=2)
    from PIL import Image as _I
    n = _I.open(path).n_frames
    return path, n, sum(durations)


# One consistent track across every Now Playing GIF: Something Real / Post Malone
# / AUSTIN / TRACK 3 OF 12, battery 78, SHUF — all baked into _now_playing_base.
NP_TOTAL = 182

def _np(elapsed, vol=None, theme=None, locked=False):
    """A Now Playing frame at `elapsed` seconds (elapsed + -remaining = NP_TOTAL
    every frame; the progress bar tracks elapsed). Optional volume overlay, theme
    (Onyx), and the persistent Hold padlock in the status strip (`locked`)."""
    fn = lambda: _now_playing_base(vol_overlay=vol, elapsed=elapsed,
                                   total=NP_TOTAL, locked=locked).img
    return with_palette(theme, fn) if theme else fn()


def gif_browse():
    """LIBRARY: album list (sorted A->Z by title). The selection bar steps DOWN
    through the albums and lands on the long-titled one (which sorts near the
    end); its title then marquees to reveal the tail. Loops."""
    spec = []
    LONG = ALBUMS_SEL
    for s in range(0, LONG):                                 # step down the list
        spec.append((screen_albums(sel=s), 3 if s == 0 else 2, 150))
    spec.append((screen_albums(sel=LONG, title_offset=0), 3, 150))  # land, truncated
    # marquee: dwell (above), scroll once to reveal the tail, dwell, then reset
    t = ALBUMS[LONG][0]
    tx = 12 + 28 + 8
    max_off = max(0, text_width(t, FONT_HEADER) - ((W - 16) - tx))
    o = 0
    while o < max_off:
        o = min(max_off, o + 6)
        spec.append((screen_albums(sel=LONG, title_offset=o), 1, 90))
    spec.append((screen_albums(sel=LONG, title_offset=max_off), 4, 150))  # dwell tail
    spec.append((screen_albums(sel=LONG, title_offset=0), 3, 150))        # reset -> loop
    return _save_gif("browse.gif", spec, colors=80)


def gif_volume():
    """VOLUME: the overlay ramps volume 0 -> 100 -> 0 ON TOP of a track that keeps
    playing — the clock ticks up and the progress bar creeps the whole time. The
    speaker shows the mute X at 0; wave crescents grow at >5 / >40 / >72; the fill
    bar and the percent match the volume."""
    ups = [0, 4, 10, 20, 30, 41, 50, 60, 73, 82, 92, 100]
    e0, clock_ms = 73, 0
    spec = []
    def push(vol, hold, ms):
        nonlocal clock_ms
        elapsed = e0 + clock_ms // 1000           # playback advances with GIF time
        spec.append((_np(elapsed, vol=vol), hold, ms))
        clock_ms += hold * ms
    push(0, 6, 150)                               # MUTE: speaker X (held)
    for v in ups[1:]:
        push(v, 3 if v in (41, 73) else 2, 130)   # linger as waves 2 & 3 pop in
    push(100, 4, 160)                             # all three waves, full
    for v in (82, 60, 41, 20, 4):                 # coarser down-ramp (size)
        push(v, 2, 120)
    push(0, 5, 150)                               # back to MUTE
    return _save_gif("volume.gif", spec, colors=64)


def gif_themes():
    """SEVEN THEMES: cross-cut the SAME playing track through every theme in
    picker order (THEMES). The track keeps playing across the cuts, so the
    clock ticks up at each flip. ~900ms per theme, longer on the first."""
    spec = []
    e = 73
    for i, (name, sub, pal) in enumerate(THEMES):
        theme = None if name == "Linen" else pal   # Linen == the live default
        ms = 1400 if i == 0 else 900
        spec.append((_np(e, theme=theme), 1, ms))
        e += 1                                     # one hold -> +1s playback
    return _save_gif("themes.gif", spec, colors=64)


def gif_lock():
    """LOCK: Now Playing (no lock) -> the LOCKED banner inverting the top row ->
    the screen WHILE locked with the persistent padlock in the status strip ->
    the UNLOCKED banner -> back to Now Playing (glyph gone). Playback keeps
    running, so the clock ticks up throughout and the padlock stays in the strip
    the whole time Hold is engaged."""
    e = 73
    spec = []
    spec.append((_np(e), 3, 160)); e += 1
    # Hold engaged: the top row inverts, and the strip padlock appears under it
    # (glyph=True) ready for when the banner goes.
    spec.append((_lock_screen(True, elapsed=e, glyph=True), 5, 170)); e += 1
    # Banner gone but still locked: the small padlock persists top-right.
    spec.append((_np(e, locked=True), 5, 170)); e += 1
    # Hold disengaged: the UNLOCKED banner, and the strip padlock is gone.
    spec.append((_lock_screen(False, elapsed=e, glyph=False), 5, 170)); e += 1
    spec.append((_np(e), 3, 160))
    return _save_gif("hold.gif", spec)


def gif_settings():
    """SETTINGS: the Sound screen's Volume slider ramps up then back down, then
    settles on the seven-theme picker."""
    def sound_vol(v):
        rows = list(SOUND_ROWS)
        rows[0] = ("Volume", "%d%%" % v, v, 100)
        return screen_sound(rows=rows, sel_row=0)
    vals = [20, 35, 50, 65, 80, 92]
    spec = [(sound_vol(20), 3, 150)]
    for v in vals[1:]:
        spec.append((sound_vol(v), 2, 130))
    spec.append((sound_vol(92), 3, 150))
    for v in reversed(vals[:-1]):
        spec.append((sound_vol(v), 2, 130))
    spec.append((sound_vol(20), 3, 150))
    spec.append((screen_theme(), 2, 700))     # end on the theme picker
    return _save_gif("settings.gif", spec)


def _boot_spec():
    """BOOT: one screen from power-on to the menu (main.c never swaps screens
    here, it repaints the same one). The pre-mount splash carries no bar; the
    bar appears once the settings read has told us the theme and the library
    load starts reporting percent; the menu replaces it at the end."""
    return [
        (boot_screen("LOADING", -1),               1, 1400),  # pre-mount splash
        (boot_screen("LOADING LIBRARY", 0),        1,  300),
        (boot_screen("LOADING LIBRARY", 18),       1,  220),
        (boot_screen("LOADING LIBRARY", 41),       1,  220),
        (boot_screen("LOADING LIBRARY", 64),       1,  220),
        (boot_screen("LOADING LIBRARY", 83),       1,  220),
        (boot_screen("LOADING LIBRARY", 100),      1,  350),
        (screen_menu("Core", MAIN_MENU, 0, False), 1, 1600),  # main menu, Music
    ]


def gif_boot():
    return _save_gif("boot.gif", _boot_spec(), colors=64)


def gif_jump():
    """TRANSPORT: Right on a list pushes Now Playing over it, and Menu comes
    back to the row you left (Albums, "Changes" selected)."""
    spec = [
        (screen_albums(sel=1), 1,  900),
        (_np(73),              1, 1500),   # RIGHT: Now Playing, playback running
        (_np(74),              1,  600),
        (screen_albums(sel=1), 1, 1200),   # MENU: back on the same row
    ]
    return _save_gif("jump.gif", spec, colors=80)


def gif_hero():
    """The README hero: cold boot to the menu, then the walkthrough."""
    return _save_gif("hero.gif", _boot_spec() + walkthrough_spec(), colors=96)


# ---------------------------------------------------------------------------
# Extra library / browsing screens
# ---------------------------------------------------------------------------
MAIN_MENU_FULL = [  # full menu, a track is loaded so "Now Playing" shows active
    ("Music", True), ("Playlists", True), ("Podcasts", False),
    ("Audiobooks", False), ("Settings", True), ("Now Playing", True),
]

def screen_mainmenu():
    return screen_menu("Core", MAIN_MENU_FULL, 0, back=False)

def screen_music():
    return screen_menu("Music", MUSIC_MENU, 2, back=True)   # Albums selected


# Firmware sorts Artists A->Z by ARTIST NAME (case-insensitive).
ARTISTS = sorted([
    ("Post Malone", 6), ("Justin Bieber", 5), ("The Kid LAROI", 5),
    ("Daniel Caesar", 4), ("LANY", 3), ("Morgan Wallen", 3),
    ("Juice WRLD", 2), ("Rex Orange County", 2), ("Steely Dan", 2),
    ("XXXTENTACION", 2),
], key=lambda a: a[0].lower())
ARTISTS_SEL = next(i for i, a in enumerate(ARTISTS) if a[0] == "LANY")

def screen_artists():
    sc = Screen()
    status_strip(sc)
    header(sc, "Artists", "%d / %d" % (ARTISTS_SEL + 1, len(ARTISTS)), back=True)
    for r in range(LIST_ROWS):
        if r >= len(ARTISTS):
            break
        name, cnt = ARTISTS[r]
        # main.c artists_row_draw: list_row(..., right=0, chevron=1) — a
        # disclosure chevron, no per-row album count (the count is header-only).
        list_row(sc, LIST_Y0, r, name, chevron=True, selected=(r == ARTISTS_SEL))
    scrollbar(sc, LIST_Y0, 0, LIST_ROWS, len(ARTISTS))
    return sc.img


# Firmware sorts Songs A->Z by SONG TITLE (case-insensitive).
SONGS = sorted([
    ("Something Real", "Post Malone", "3:02"),
    ("Sunflower", "Post Malone", "2:38"),
    ("Ghost", "Justin Bieber", "2:33"),
    ("STAY", "The Kid LAROI", "2:21"),
    ("Rearrange My World / There's a Field (That's Only Yours)", "Daniel Caesar", "5:16"),
    ("Malibu Nights", "LANY", "3:48"),
], key=lambda s: s[0].lower())
# keep the long-titled song selected (demonstrates truncation)
SONGS_SEL = next(i for i, s in enumerate(SONGS) if s[0].startswith("Rearrange"))

def screen_songs():
    sc = Screen()
    status_strip(sc)
    header(sc, "Songs", "%d / %d" % (SONGS_SEL + 1, len(SONGS)), back=True)
    for r, (t, a, dur) in enumerate(SONGS):
        list_row(sc, LIST_Y0, r, t, sub=a, right=dur, selected=(r == SONGS_SEL),
                 rh=ROW_H2, title_priority=True)
    scrollbar(sc, LIST_Y0, 0, LIST_ROWS2, len(SONGS))
    return sc.img


# An ARTIST's "All Songs" — the synthetic row at the top of a filtered album
# list opens the whole discography in title order (main.c songview_build +
# songs_render). Two things differ from the plain Songs list above: the header
# is the ARTIST, and each row's sub-line is the ALBUM. It used to repeat the
# artist, which under an artist header spent the only sub-line on the one fact
# the reader already had (main.c songs_row_draw).
ALLSONGS_ARTIST = "Post Malone"
ALLSONGS = sorted([
    ("Chemical", "AUSTIN", "2:46"),
    ("Circles", "Hollywood's Bleeding", "3:35"),
    ("Don't Understand", "AUSTIN", "3:14"),
    ("Hollywood's Bleeding", "Hollywood's Bleeding", "2:36"),
    ("I Had Some Help", "F-1 Trillion", "3:58"),
    ("Mourning", "AUSTIN", "2:52"),
    ("Novacandy", "AUSTIN", "3:38"),
    ("Pour Me a Drink", "F-1 Trillion", "3:04"),
    ("Something Real", "AUSTIN", "3:02"),
    ("Sunflower", "Hollywood's Bleeding", "2:38"),
], key=lambda s: s[0].lower())
# the track Now Playing is on, so the screens tell one story
ALLSONGS_SEL = next(i for i, s in enumerate(ALLSONGS) if s[0] == "Something Real")

def screen_allsongs(sel=ALLSONGS_SEL):
    sc = Screen()
    status_strip(sc)
    header(sc, ALLSONGS_ARTIST, "%d / %d" % (sel + 1, len(ALLSONGS)), back=True)
    top = scroll_window(sel, len(ALLSONGS), LIST_ROWS2)
    for vr in range(LIST_ROWS2):
        r = top + vr
        if r >= len(ALLSONGS):
            break
        t, album, dur = ALLSONGS[r]
        list_row(sc, LIST_Y0, vr, t, sub=album, right=dur, selected=(r == sel),
                 rh=ROW_H2, title_priority=True)
    scrollbar(sc, LIST_Y0, top, LIST_ROWS2, len(ALLSONGS))
    return sc.img


# Music -> Playlists (main.c playlists_render / playlists_row_draw): a plain
# single-line list (list_row, ROW_H) of the .m3u8 files in Music/Playlists,
# each row a name + disclosure chevron — no sub-line, no per-row right value
# (the count lives in the header only, like Artists).
PLAYLISTS = [
    "Road Trip", "Late Night", "Sunday Morning", "Gym", "Favourites", "Focus",
]
PLAYLISTS_SEL = 1

def screen_playlists(sel=PLAYLISTS_SEL):
    sc = Screen()
    status_strip(sc)
    header(sc, "Playlists", "%d / %d" % (sel + 1, len(PLAYLISTS)), back=True)
    for r in range(LIST_ROWS):
        if r >= len(PLAYLISTS):
            break
        list_row(sc, LIST_Y0, r, PLAYLISTS[r], chevron=True, selected=(r == sel))
    scrollbar(sc, LIST_Y0, 0, LIST_ROWS, len(PLAYLISTS))
    return sc.img


# ---------------------------------------------------------------------------
# Settings sub-screens (no status strip — the header sits at the top band)
# ---------------------------------------------------------------------------
def _sel_bar(sc, y0, rowh, r):
    sc.fill_round_rect(6, y0 + r * rowh + 1, W - 16, rowh - 2, 4, SEL_BG)

# The root Settings list — core/ui/settings.c ROOT_L, all nine rows. Nine rows
# do not fit in the eight the panel has room for, so this list SCROLLS and
# carries a scrollbar (core/ui/screen_settings.c list_render / st_scrollbar).
ROOT_L = ["Playback", "Sound", "Theme", "Display", "Clicker", "About",
          "Boot Details", "Disk Mode", "Reset Settings"]
ROOT_SEL = 1   # Sound

def scroll_window(sel, total, visible):
    """main.c scroll_window / screen_settings.c st_scroll_window: keep the
    selection about 1/3 down the window, clamped to the ends."""
    if total <= visible:
        return 0
    return max(0, min(sel - visible // 3, total - visible))

def screen_settings(sel=ROOT_SEL):
    sc = Screen()
    header(sc, "Settings", back=True)
    status_strip(sc)                 # main.c settings_render_cur
    right_vals = {2: "Linen", 4: "Tick"}   # Theme + Clicker carry their choice
    top = scroll_window(sel, len(ROOT_L), LIST_ROWS)
    for vr in range(LIST_ROWS):
        r = top + vr
        if r >= len(ROOT_L):
            break
        ry = LIST_Y0 + vr * ROW_H
        is_sel = (r == sel)
        if is_sel:
            _sel_bar(sc, LIST_Y0, ROW_H, vr)
        fg = SEL_FG if is_sel else INK
        rightc = SEL_SUB if is_sel else MUTED_D
        chevc = SEL_SUB if is_sel else CHEVRON
        sc.text(14, ry + 15, ROOT_L[r], FONT_HEADER if is_sel else FONT_ROW, fg)
        if r in right_vals:
            sc.text_right(W - 16, ry + 15, right_vals[r], regular_11, rightc)
        else:
            sc.text(W - 18, ry + 15, RAQUO, FONT_ROW, chevc)
    scrollbar(sc, LIST_Y0, top, LIST_ROWS, len(ROOT_L))
    return sc.img


# ---------------------------------------------------------------------------
# Boot Details (Settings > Boot Details) — core/ui/screen_settings.c
# settings_diag_render(). Where a cold boot's 3.3s actually goes.
# ---------------------------------------------------------------------------
# Phase colours are fixed literals on the device too, not palette slots: the
# segments have to stay distinguishable from each other in any theme.
C_LCD, C_DISK, C_LIB, C_RES, C_OTHER = 0x3BDB, 0x3DAD, 0xE546, 0xE125, 0x94B2

def fmt_ms(ms):
    """settings_diag_render's fmt_ms: sub-second in ms (a 40ms seek must not
    round to '0.0s'), a second or more with one decimal."""
    if ms is None:
        return "--"
    if ms < 1000:
        return "%dms" % ms
    return "%d.%ds" % (ms // 1000, (ms // 100) % 10)

# Measured on the device: a cold boot is ~3.3s, and it is the library scan that
# owns it. OTHER is NOT an input — the firmware derives it as the unattributed
# remainder (total minus the four measured phases), so it is whatever the other
# phases leave behind; setting it here would let the picture disagree with the
# figures, which is the one thing this screen must never do.
DIAG_TOTAL = 3300
DIAG_LCD, DIAG_DISK, DIAG_LIB, DIAG_RESUME = 210, 1900, 610, 480
DIAG_RES_DIR, DIAG_RES_OPEN, DIAG_RES_SEEK = 120, 300, 40
DIAG_DECODE_PCT = 34          # of the 22676 us/kframe 44.1kHz real-time budget
DIAG_SEQ = 500
DIAG_LBA = (49236472, 49236474)     # CONFIG slot LBAs (config_save())
DIAG_LOG_LBA = (49238456, 49238464)  # event log header / next-flush LBAs

def screen_diag():
    sc = Screen()
    # The full build id rides in the header's right-hand slot — the one free
    # text row on a page whose bars, legend and LBA rows reach y=230.
    header(sc, "Boot Details", right=BUILD_ID, back=True)
    status_strip(sc)                 # main.c settings_render_cur

    # headline: label left, total right, on one line
    sc.text(16, 60, "COLD BOOT", FONT_SMALL, MUTED)
    sc.text_right(W - 16, 63, fmt_ms(DIAG_TOTAL), FONT_TITLE, INK)

    # stacked proportional bar — same numbers as the legend, so the picture
    # cannot disagree with the figures; the remainder is drawn last, in grey.
    bx, by, bw, bh = 16, 74, W - 32, 10
    sc.fill_round_rect(bx, by, bw, bh, 5, TRK)
    segs = [(DIAG_LCD, C_LCD), (DIAG_DISK, C_DISK), (DIAG_LIB, C_LIB),
            (DIAG_RESUME, C_RES)]
    x = bx
    for ms, col in segs:
        w = ms * bw // DIAG_TOTAL
        if w <= 0:
            continue
        w = min(w, bx + bw - x)
        sc.fill_rect(x, by, w, bh, rgb565(col))
        x += w
    if x < bx + bw:
        sc.fill_rect(x, by, bx + bw - x, bh, rgb565(C_OTHER))

    # legend: six entries in two columns of three (six stacked rows ran into
    # the config block at the bottom of a 240px panel)
    known = DIAG_LCD + DIAG_DISK + DIAG_LIB + DIAG_RESUME
    other = max(0, DIAG_TOTAL - known)
    names = ["LCD", "DISK", "LIBRARY", "RESUME", "OTHER", "DECODE"]
    cols = [C_LCD, C_DISK, C_LIB, C_RES, C_OTHER, None]
    vals = [DIAG_LCD, DIAG_DISK, DIAG_LIB, DIAG_RESUME, other]
    colx, colw = (16, 168), 136
    for i in range(6):
        cx = colx[i // 3]
        y = 104 + (i % 3) * 18
        sc.fill_rect(cx, y - 7, 6, 6, rgb565(cols[i]) if cols[i] else MUTED2)
        sc.text(cx + 11, y, names[i], FONT_SMALL, MUTED)
        if i < 5:
            sc.text_right(cx + colw, y, fmt_ms(vals[i]), FONT_SUB, INK)
        else:
            # decode headroom against the real-time budget; red inside 20% of it
            sc.text_right(cx + colw, y, "%d%%" % DIAG_DECODE_PCT, FONT_SUB,
                          BATT_RED if DIAG_DECODE_PCT >= 80 else INK)
    sc.fill_rect(16, 152, W - 32, 1, BORDER)

    # the resume split, as one indented dimmer line: it is a breakdown OF the
    # RESUME entry above, not three more phases
    x = 16
    sc.text(x, 168, "RESUME", FONT_SMALL, MUTED2)
    x += text_width("RESUME", FONT_SMALL) + 10
    for lbl, ms in (("dir", DIAG_RES_DIR), ("open", DIAG_RES_OPEN),
                    ("seek", DIAG_RES_SEEK)):
        sc.text(x, 168, lbl, FONT_SMALL, MUTED2)
        x += text_width(lbl, FONT_SMALL) + 4
        v = fmt_ms(ms)
        sc.text(x, 168, v, FONT_SMALL, INK)
        x += text_width(v, FONT_SMALL) + 12
    sc.fill_rect(16, 196, W - 32, 1, BORDER)

    # settings-file locator: two rows, label left, "a / b" right — the
    # absolute LBAs config_save() writes to, then the event log's header /
    # next-flush LBAs. With no serial cable, the panel is the only place to
    # read them back.
    sc.text(16, 214, "CONFIG seq %d" % DIAG_SEQ, FONT_SMALL, MUTED)
    sc.text_right(W - 16, 214, "%d / %d" % DIAG_LBA, FONT_SMALL, MUTED_D)
    sc.text(16, 230, "LOG", FONT_SMALL, MUTED)
    sc.text_right(W - 16, 230, "%d / %d" % DIAG_LOG_LBA, FONT_SMALL, MUTED_D)
    return sc.img


SOUND_ROWS = [
    # (label, value, num, den)
    ("Volume", "72%", 72, 100),
    ("Bass", "+3 dB", 3 + 12, 24),
    ("Treble", "0 dB", 0 + 12, 24),
    ("Balance", "Center", 0 + 100, 200),
]
SOUND_SEL = 1   # Bass (boosted)

def screen_sound(rows=None, sel_row=SOUND_SEL):
    rows = rows if rows is not None else SOUND_ROWS
    sc = Screen()
    header(sc, "Sound", back=True)
    status_strip(sc)                 # main.c settings_render_cur
    for r, (label, val, num, den) in enumerate(rows):
        ry = LIST_Y0 + r * ROW_H
        sel = (r == sel_row)
        if sel:
            _sel_bar(sc, LIST_Y0, ROW_H, r)
        fg = SEL_FG if sel else INK
        rightc = SEL_SUB if sel else MUTED_D
        sc.text(14, ry + 11, label, FONT_HEADER if sel else FONT_ROW, fg)
        sc.text_right(W - 16, ry + 11, val, regular_11, rightc)
        # slider bar
        bx, bw, by, bh = 14, W - 16 - 14, ry + 17, 3
        sc.fill_rect(bx, by, bw, bh, SEL_TRK if sel else TRK)
        fw = max(0, min(bw, bw * num // den))
        sc.fill_rect(bx, by, fw, bh, SEL_FG if sel else INK)
    return sc.img


CLICK_L = ["Off", "Tick", "Click", "Pop", "Blip", "Tock", "Double", "Chirp"]
CLICK_ACTIVE = 1   # Tick
CLICK_SEL = 2      # Click

def screen_clicker():
    sc = Screen()
    header(sc, "Clicker", back=True)
    status_strip(sc)                 # main.c settings_render_cur
    for r, label in enumerate(CLICK_L):
        ry = LIST_Y0 + r * ROW_H
        sel = (r == CLICK_SEL)
        if sel:
            _sel_bar(sc, LIST_Y0, ROW_H, r)
        fg = SEL_FG if sel else INK
        # settings.c: the active profile's row draws a middot in the generic
        # list_render right-value slot — regular_11 (FONT_SUB), MUTED_D when
        # not selected, same as any other row's right value.
        markc = SEL_SUB if sel else MUTED_D
        sc.text(14, ry + 15, label, FONT_HEADER if sel else FONT_ROW, fg)
        if r == CLICK_ACTIVE:
            sc.text_right(W - 16, ry + 15, MIDDOT, regular_11, markc)  # marks active
    return sc.img


# Theme picker (core/ui/screen_settings.c theme_render): 39px rows, five
# visible ((240-42)/39), windowed + scrollbarred like every other list.
TH_ROW_H = 39
TH_ROWS = (H - LIST_Y0) // TH_ROW_H     # 5
TH_CURRENT = 0                  # Linen active
TH_SEL = 1                      # cursor on Onyx

def screen_theme(sel=TH_SEL, current=TH_CURRENT):
    sc = Screen()
    header(sc, "Theme", "%d themes" % len(THEMES), back=True)
    status_strip(sc)                 # main.c settings_render_cur
    n = len(THEMES)
    top = scroll_window(sel, n, TH_ROWS)
    for vr in range(TH_ROWS):
        r = top + vr
        if r >= n:
            break
        name, sub, pal = THEMES[r]
        ry = LIST_Y0 + vr * TH_ROW_H
        is_sel = (r == sel)
        if is_sel:
            _sel_bar(sc, LIST_Y0, TH_ROW_H, vr)
        # Swatch tile: that theme's OWN surface/ink/accent (never the live
        # palette), so a row previews the theme it would switch to.
        sw, sx = 26, 14
        sy = ry + (TH_ROW_H - sw) // 2
        sc.fill_rect(sx - 1, sy - 1, sw + 2, sw + 2, SEL_SUB if is_sel else BORDER)
        sc.fill_rect(sx, sy, sw, sw, rgb565(pal["SURFACE"]))
        sc.fill_rect(sx + 6, sy + 10, 14, 3, rgb565(pal["INK"]))
        sc.fill_rect(sx + 6, sy + 16, 4, 4, rgb565(pal["ACCENT"]))
        tx = sx + sw + 10
        fg = SEL_FG if is_sel else INK
        subc = SEL_SUB if is_sel else MUTED
        sc.text(tx, ry + 17, name, FONT_HEADER, fg)
        sc.text(tx, ry + 31, sub, FONT_SMALL, subc)
        if r == current:
            sc.text_right(W - 14, ry + 20, "CURRENT", FONT_SMALL,
                          SEL_FG if is_sel else MUTED2)
    scrollbar(sc, LIST_Y0, top, TH_ROWS, n)
    return sc.img


# ---------------------------------------------------------------------------
# Onyx (warm-dark) variants — same builders under the swapped palette
# ---------------------------------------------------------------------------
def with_palette(spec, fn):
    apply_palette(spec)
    try:
        return fn()
    finally:
        apply_palette(LINEN)

def screen_albums_onyx():
    return with_palette(ONYX, lambda: screen_albums())

def screen_nowplaying_onyx():
    return with_palette(ONYX, lambda: screen_nowplaying())

def screen_nowplaying_sage():
    return with_palette(SAGE, screen_nowplaying)


# ---------------------------------------------------------------------------
# System screens: charging + boot splash
# ---------------------------------------------------------------------------
def _bolt(sc, cx, y0, bh, c):
    """Lightning bolt polygon, scanline-filled (port of screen_charging.c)."""
    pxb = [0, 0, 3, 3, 10, 6, 10]
    pyb = [0, 11, 11, 20, 8, 8, 0]
    N, box_w, box_h = 7, 10, 20
    bw = (box_w * bh) // box_h
    x0 = cx - bw // 2
    sx = [x0 + (pxb[i] * bw) // box_w for i in range(N)]
    sy = [y0 + (pyb[i] * bh) // box_h for i in range(N)]
    for y in range(y0, y0 + bh):
        xs = []
        for i in range(N):
            a, b = i, (i + 1) % N
            ya, yb, xa, xb = sy[a], sy[b], sx[a], sx[b]
            if ya == yb:
                continue
            if ya < yb:
                ylo, yhi, xlo, xhi = ya, yb, xa, xb
            else:
                ylo, yhi, xlo, xhi = yb, ya, xb, xa
            if y < ylo or y >= yhi:
                continue
            xs.append(xlo + (xhi - xlo) * (y - ylo) // (yhi - ylo))
        xs.sort()
        for k in range(0, len(xs) - 1, 2):
            L, R = xs[k], xs[k + 1]
            if R > L:
                sc.fill_rect(L, y, R - L, 1, c)

def screen_charging(pct=64, charging=True, external=True):
    CHG_BG = rgb565(0x0861)
    CHG_OUTLINE = rgb565(0x5A89)
    CHG_FILL = rgb565(0xEF3B)
    CHG_GREEN = rgb565(0x3E4D)
    CHG_RED = rgb565(0xDA46)
    CHG_TEXT = rgb565(0xEF3B)
    CHG_UNIT = rgb565(0xACF2)
    CHG_MUTED = rgb565(0x7B8D)
    BW, BH = 150, 68
    BX, BY, BT, INSET = (W - BW) // 2, 56, 3, 8
    NUB_W, NUB_H = 6, 24
    sc = Screen(CHG_BG)
    fill = CHG_GREEN if charging else (CHG_RED if pct < 20 else CHG_FILL)
    # outline (4 strokes) + softened corners
    sc.fill_rect(BX, BY, BW, BT, CHG_OUTLINE)
    sc.fill_rect(BX, BY + BH - BT, BW, BT, CHG_OUTLINE)
    sc.fill_rect(BX, BY, BT, BH, CHG_OUTLINE)
    sc.fill_rect(BX + BW - BT, BY, BT, BH, CHG_OUTLINE)
    for cxx, cyy in ((BX, BY), (BX + BW - 1, BY), (BX, BY + BH - 1), (BX + BW - 1, BY + BH - 1)):
        sc.px[cxx, cyy] = CHG_BG
    # nub
    sc.fill_rect(BX + BW, BY + (BH - NUB_H) // 2, NUB_W, NUB_H, CHG_OUTLINE)
    # inner fill
    ix, iy = BX + INSET, BY + INSET
    iw, ih = BW - 2 * INSET, BH - 2 * INSET
    fw = max(8, min(iw, iw * max(0, min(100, pct)) // 100))
    sc.fill_rect(ix, iy, fw, ih, fill)
    if charging:
        _bolt(sc, W // 2, iy + 4, ih - 8, CHG_BG)
    # big percent + unit
    num = str(max(0, min(100, pct)))
    wn = text_width(num, bold_17)
    wu = text_width("%", bold_13)
    nx = (W - (wn + 2 + wu)) // 2
    sc.text(nx, 168, num, bold_17, CHG_TEXT)
    sc.text(nx + wn + 2, 168, "%", bold_13, CHG_UNIT)
    # status line
    if charging:
        status, sink = "CHARGING", CHG_GREEN
    elif not external:
        status, sink = "CONNECT CABLE", CHG_MUTED
    else:
        status, sink = "NOT CHARGING", CHG_MUTED
    sc.text_centered(196, status, bold_11, sink)
    return sc.img


# ---------------------------------------------------------------------------
# Low-battery full-screen warnings (core/ui/screen_battery.c
# screen_battery_render) — the charging screen's dark field + battery glyph
# (0%, red stub), with the DISKSAFE / SHUTOFF copy.
# ---------------------------------------------------------------------------
def screen_battery_low(kind="disksafe"):
    CHG_BG = rgb565(0x0861)
    CHG_OUTLINE = rgb565(0x5A89)
    CHG_RED = rgb565(0xDA46)
    CHG_TEXT = rgb565(0xEF3B)
    CHG_UNIT = rgb565(0xACF2)
    CHG_MUTED = rgb565(0x7B8D)
    BW, BH = 150, 68
    BX, BY, BT, INSET = (W - BW) // 2, 56, 3, 8
    NUB_W, NUB_H = 6, 24
    sc = Screen(CHG_BG)
    # battery glyph: outline + nub + a pct=0 red stub (min 8px, never bare)
    sc.fill_rect(BX, BY, BW, BT, CHG_OUTLINE)
    sc.fill_rect(BX, BY + BH - BT, BW, BT, CHG_OUTLINE)
    sc.fill_rect(BX, BY, BT, BH, CHG_OUTLINE)
    sc.fill_rect(BX + BW - BT, BY, BT, BH, CHG_OUTLINE)
    for cxx, cyy in ((BX, BY), (BX + BW - 1, BY), (BX, BY + BH - 1), (BX + BW - 1, BY + BH - 1)):
        sc.px[cxx, cyy] = CHG_BG
    sc.fill_rect(BX + BW, BY + (BH - NUB_H) // 2, NUB_W, NUB_H, CHG_OUTLINE)
    ix, iy = BX + INSET, BY + INSET
    iw, ih = BW - 2 * INSET, BH - 2 * INSET
    fw = max(8, min(iw, iw * 0 // 100))
    sc.fill_rect(ix, iy, fw, ih, CHG_RED)

    head, body, hint = bold_13, regular_11, regular_9
    if kind == "shutoff":
        # SHUTOFF: three lines, no dismiss hint — there is nothing to press.
        sc.text_centered(150, "Battery empty", head, CHG_TEXT)
        sc.text_centered(172, "Powering off now", body, CHG_UNIT)
        sc.text_centered(190, "Plug in to charge", body, CHG_UNIT)
    else:
        # DISKSAFE (default): the write-gate warning.
        sc.text_centered(150, "Battery very low", head, CHG_TEXT)
        sc.text_centered(172, "Plug in now to keep listening.", body, CHG_UNIT)
        sc.text_centered(190, "Settings will not be saved until then.", body, CHG_UNIT)
        sc.text_centered(218, "Press any button to dismiss", hint, CHG_MUTED)
    return sc.img


# The boot screen's build stamp is CORE_BUILD_ID: `git describe --tags --always
# --dirty --abbrev=7`, baked in at build time (core/meson.build vcs_tag). It is
# computed at render time from the same command (BUILD_ID, above), so the still
# carries the stamp an image built from this tree would show — including
# "-dirty" when the tree is uncommitted.
BOOT_BUILD_ID = BUILD_ID
BOOT_MARK_CY  = 86
BOOT_BAR_Y    = H - 34
BOOT_BAR_X    = 60
BOOT_BAR_W    = W - 120


def boot_screen(phase, pct, build=BOOT_BUILD_ID):
    """main.c boot_screen_render(): the click-wheel mark, "Core", the device
    line, and along the bottom a 2 px bar with the phase in small caps. pct < 0
    draws no bar (the pre-mount splash)."""
    sc = Screen(SURFACE)
    cx, cy = W // 2, BOOT_MARK_CY
    sc.fill_disc_aa(cx, cy, 19, INK)        # ring: ink disc ...
    sc.fill_disc_aa(cx, cy, 17, SURFACE)    # ... with a surface disc
    sc.fill_disc_aa(cx, cy, 6, INK)         # centre dot
    sc.fill_disc_aa(cx, cy, 2, SURFACE)     # its hole
    sc.text_centered(cy + 19 + 30, "Core", FONT_TITLE, INK)
    sc.text_centered(cy + 19 + 46, "IPOD VIDEO  " + MIDDOT + "  5.5 GEN",
                     FONT_SMALL, MUTED2)
    if pct >= 0:
        pct = min(pct, 100)
        sc.fill_rect(BOOT_BAR_X, BOOT_BAR_Y, BOOT_BAR_W, 2, TRK)
        fw = BOOT_BAR_W * pct // 100
        if fw > 0:
            sc.fill_rect(BOOT_BAR_X, BOOT_BAR_Y, fw, 2, INK)
    sc.text_centered(H - 16, phase, FONT_SMALL, MUTED2)
    sc.text_right(W - 8, H - 4, build, FONT_SMALL, BORDER)
    return sc.img


def screen_boot():
    """Pre-mount: the panel is ours, the disk is still spinning up."""
    return boot_screen("LOADING", -1)


def screen_loading():
    """Mid library load — the same screen, with the bar and the phase."""
    return boot_screen("LOADING LIBRARY", 62)


def screen_loading_onyx():
    return with_palette(ONYX, screen_loading)


def main():
    outputs = []
    outputs.append(save_png(screen_albums(), "albums.png"))
    outputs.append(save_png(screen_nowplaying(), "nowplaying.png"))
    outputs.append(save_png(screen_detail(), "detail.png"))
    outputs.append(save_png(screen_genres(), "genres.png"))
    outputs.append(save_png(screen_about(), "about.png"))
    outputs.append(save_png(screen_volume(), "volume.png"))
    outputs.append(save_png(screen_lock(), "hold_unlocked.png"))
    outputs.append(save_png(screen_locked(), "hold_locked.png"))
    outputs.append(save_png(screen_locked_list(), "hold_locked_list.png"))
    # --- new: library / browsing ---
    outputs.append(save_png(screen_mainmenu(), "mainmenu.png"))
    outputs.append(save_png(screen_music(), "music.png"))
    outputs.append(save_png(screen_artists(), "artists.png"))
    outputs.append(save_png(screen_songs(), "songs.png"))
    outputs.append(save_png(screen_allsongs(), "allsongs.png"))
    outputs.append(save_png(screen_playlists(), "playlists.png"))
    # --- new: settings ---
    outputs.append(save_png(screen_settings(), "settings.png"))
    outputs.append(save_png(screen_diag(), "bootdetails.png"))
    outputs.append(save_png(screen_sound(), "sound.png"))
    outputs.append(save_png(screen_clicker(), "clicker.png"))
    outputs.append(save_png(screen_theme(), "theme.png"))
    # --- new: dual theme (Onyx) ---
    outputs.append(save_png(screen_nowplaying_onyx(), "nowplaying_onyx.png"))
    outputs.append(save_png(screen_nowplaying_sage(), "nowplaying_sage.png"))
    outputs.append(save_png(screen_albums_onyx(), "albums_onyx.png"))
    # --- new: system ---
    outputs.append(save_png(screen_charging(), "charging.png"))
    outputs.append(save_png(screen_battery_low(), "battery_low.png"))
    outputs.append(save_png(screen_boot(), "boot.png"))
    outputs.append(save_png(screen_loading(), "loading.png"))
    outputs.append(save_png(screen_loading_onyx(), "loading_onyx.png"))
    # --- big walkthrough gif (unchanged) ---
    gifs = [build_walkthrough_gif()]
    # --- per-feature gifs ---
    gifs.append(gif_browse())
    gifs.append(gif_volume())
    gifs.append(gif_themes())
    gifs.append(gif_lock())
    gifs.append(gif_settings())
    gifs.append(gif_boot())
    gifs.append(gif_jump())
    gifs.append(gif_hero())
    for p in outputs:
        print("wrote", p, os.path.getsize(p), "bytes")
    for path, nframes, total_ms in gifs:
        print("wrote %s  %d frames, %dx%d, %.1fs, %d bytes" %
              (path, nframes, W * GIF_SCALE, H * GIF_SCALE,
               total_ms / 1000.0, os.path.getsize(path)))


if __name__ == "__main__":
    main()
