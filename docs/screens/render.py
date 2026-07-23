#!/usr/bin/env python3
"""
Faithful 320x240 renderer for the Core (iPod firmware) "Linen" warm-light UI.

Reproduces the on-device look of core/kernel/main.c + core/ui/screen_settings.c:
the status strip, titled headers, list rows (single + two-line with art chips),
album detail, now-playing, the volume overlay, the lock/unlock modal, and the
About dashboard. Text is drawn with the real Nunito faces via PIL, gamma-correct
(sRGB->linear, blend by glyph coverage, re-encode) so it reads as crisply as the
device's gamma-aware atlas. Album art is quantized to RGB565 to match the panel.

Outputs PNGs (3x, NEAREST) + a marquee demo GIF into this directory.

Regenerate:  tools/.venv/bin/python3 docs/screens/render.py
"""

import os
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
FONTS = "/home/brando/Projects/ipod_theme/tools/fonts-src"
ART = os.path.join(HERE, "art")

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

SURFACE = rgb565(0xF79D)
INK     = rgb565(0x18A2)
MUTED   = rgb565(0x7B8D)
MUTED2  = rgb565(0x9C70)
MUTED_D = rgb565(0x5A89)
ACCENT  = rgb565(0xC348)   # terracotta
BORDER  = rgb565(0xE71B)
PLATE   = rgb565(0xF7BE)
TRK     = rgb565(0xDEDA)
SB_TRK  = rgb565(0xE73C)
SB_THMB = rgb565(0xAD34)
SEL_SUB = rgb565(0xB595)
BATT_RED = rgb565(0xE125)

SEL_BG = INK        # selection bar fill
SEL_FG = SURFACE    # text on selection bar

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
# Fonts
# ---------------------------------------------------------------------------
def _font(name, size):
    return ImageFont.truetype(os.path.join(FONTS, name), size)

regular_9  = _font("Nunito-Regular.ttf", 9)
regular_11 = _font("Nunito-Regular.ttf", 11)
regular_13 = _font("Nunito-Regular.ttf", 13)
bold_9     = _font("Nunito-Bold.ttf", 9)
bold_11    = _font("Nunito-Bold.ttf", 11)
bold_13    = _font("Nunito-Bold.ttf", 13)
bold_17    = _font("Nunito-Bold.ttf", 17)

FONT_SMALL  = regular_9
FONT_SUB    = regular_11
FONT_ROW    = regular_13
FONT_HEADER = bold_13
FONT_TITLE  = bold_17

# glyphs used as literals on-device
LAQUO = "‹"   # 'single left angle quote
RAQUO = "›"
MIDDOT = "·"

def text_width(s, font):
    return int(round(font.getlength(s)))

# ---------------------------------------------------------------------------
# Surface / primitives
# ---------------------------------------------------------------------------
class Screen:
    def __init__(self, bg=SURFACE):
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

    # -- text (gamma-correct) -------------------------------------------
    def text(self, x, baseline, s, font, ink, clip=None):
        if not s:
            return x
        mask = Image.new("L", (W, H), 0)
        ImageDraw.Draw(mask).text((x, baseline), s, font=font, fill=255, anchor="ls")
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
        return x + text_width(s, font)

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

def status_strip(sc, left="CORE", pct=78, locked=False):
    sc.text(12, STATUS_H - 4, left, FONT_SMALL, MUTED2)
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
             title_offset=0):
    ry = y0 + r * rh
    rowmid = ry + rh // 2 + 3
    if selected:
        sc.fill_round_rect(6, ry + 1, W - 16, rh - 2, 4, SEL_BG)
        fg, subc, rightc, chevc = SEL_FG, SEL_SUB, SEL_SUB, SEL_SUB
    else:
        fg = MUTED if greyed else INK
        subc, rightc, chevc = MUTED2, MUTED_D, MUTED2

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
ALBUMS = [
    ("AUSTIN", "Post Malone", "austin"),
    ("Rearrange My World / There's a Field (That's Only Yours)", "Daniel Caesar", "rearrange"),
    ("F-1 Trillion", "Post Malone", "f1"),
    ("Hollywood's Bleeding", "Post Malone", "hollywood"),
    ("Malibu Nights", "LANY", "malibu"),
    ("Changes", "Justin Bieber", "changes"),
]
ALBUMS_SEL = 1

def screen_albums(sel=ALBUMS_SEL, title_offset=0):
    sc = Screen()
    status_strip(sc, "CORE")
    header(sc, "Albums", "%d / %d" % (sel + 1, len(ALBUMS)), back=True)
    for r, (t, a, art) in enumerate(ALBUMS):
        s = (r == sel)
        list_row(sc, LIST_Y0, r, t, sub=a, selected=s, chip=art, rh=ROW_H2,
                 title_offset=title_offset if s else 0)
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
    status_strip(sc, "CORE")
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

def screen_detail(sel=DETAIL_SEL):
    sc = Screen()
    status_strip(sc, "CORE")
    header(sc, "Albums", "%d / %d" % (sel + 1, 17), back=True)
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
    scrollbar(sc, 108, 0, 5, len(TRACKS))
    return sc.img


def _now_playing_base(vol_overlay=None, elapsed=73, total=182):
    sc = Screen()
    # top status row
    sc.text(12, 15, "Now Playing", bold_11, INK)
    bx = W - 12 - 19
    draw_battery(sc, bx, 3, 78)
    # shuffle token
    sc.text_right(bx - 6, 13, "SHUF", FONT_SMALL, MUTED2)
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
    sc.fill_round_rect(sx - 8, sy - 3, 4, 6, 1, c)
    for dx in range(5):
        half = 2 + dx
        sc.fill_rect(sx - 4 + dx, sy - half, 1, 2 * half, c)

    def arc(R, span):
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


# -- lock modal --------------------------------------------------------------
def draw_ring_top(sc, cx, cy, Ro, Ri, c):
    for dy in range(-Ro, 1):
        xo = sc._isqrt(Ro * Ro - dy * dy)
        if -dy <= Ri:
            xi = sc._isqrt(Ri * Ri - dy * dy)
            sc.fill_rect(cx - xo, cy + dy, xo - xi + 1, 1, c)
            sc.fill_rect(cx + xi, cy + dy, xo - xi + 1, 1, c)
        else:
            sc.fill_rect(cx - xo, cy + dy, 2 * xo + 1, 1, c)

def draw_shackle(sc, sx, ay, Ro, Ri, lL, rL, c):
    w = Ro - Ri + 1
    draw_ring_top(sc, sx, ay, Ro, Ri, c)
    sc.fill_rect(sx - Ro, ay, w, lL, c)
    sc.fill_rect(sx + Ri, ay, w, rL, c)

def draw_keyhole(sc, kx, ky, bg):
    sc.fill_round_rect(kx - 3, ky - 3, 6, 6, 3, bg)
    sc.fill_rect(kx - 1, ky + 2, 2, 4, bg)

def draw_lock_icon(sc, cx, cy, open_, c, bg):
    if open_:
        draw_shackle(sc, cx, cy - 14, 10, 6, 6, 6, c)
    else:
        draw_shackle(sc, cx, cy - 9, 10, 6, 8, 8, c)
    sc.fill_round_rect(cx - 16, cy - 3, 32, 22, 4, c)
    draw_keyhole(sc, cx, cy + 7, bg)

def lock_plate(sc, locked):
    PX, PY, PW, PH = 70, 65, 180, 110
    plate = INK if locked else SURFACE
    fg = SURFACE if locked else INK
    if not locked:
        sc.fill_round_rect_aa(PX - 1, PY - 1, PW + 2, PH + 2, 11, BORDER)
    sc.fill_round_rect_aa(PX, PY, PW, PH, 10, plate)
    draw_lock_icon(sc, PX + PW // 2, PY + 45, not locked, fg, plate)
    label = "LOCKED" if locked else "UNLOCKED"
    sc.text_centered_at(PY + 84, PX, PW, label, FONT_HEADER, fg)


def _lock_screen(locked):
    sc = _now_playing_base()
    # add a helper for centered-in-plate text
    def centered(baseline, x, w, s, font, ink):
        sc.text(x + (w - text_width(s, font)) // 2, baseline, s, font, ink)
    sc.text_centered_at = lambda baseline, x, w, s, font, ink: centered(baseline, x, w, s, font, ink)
    lock_plate(sc, locked)
    return sc.img


def screen_lock():
    return _lock_screen(False)

def screen_locked():
    return _lock_screen(True)


# -- about -------------------------------------------------------------------
def screen_about():
    sc = Screen()
    header(sc, "About", back=True)
    sc.text_centered(62, "iPod 5.5G", FONT_TITLE, INK)
    lbl = ["SONGS", "ALBUMS", "ARTISTS"]
    val = [911, 94, 32]
    colw = W // 3
    for i in range(3):
        cx = colw * i + colw // 2
        v = str(val[i])
        sc.text(cx - text_width(v, FONT_TITLE) // 2, 100, v, FONT_TITLE, INK)
        sc.text(cx - text_width(lbl[i], FONT_SMALL) // 2, 116, lbl[i], FONT_SMALL, MUTED)
        if i:
            sc.fill_rect(colw * i, 86, 1, 36, BORDER)
    sc.fill_rect(16, 130, W - 32, 1, BORDER)
    # firmware row + Core chip
    sc.text(16, 150, "FIRMWARE", FONT_SMALL, MUTED)
    cw = text_width("Core", FONT_SUB)
    chw = cw + 16
    chx = W - 16 - chw
    sc.fill_round_rect(chx, 140, chw, 15, 7, ACCENT)
    sc.text(chx + 8, 151, "Core", FONT_SUB, SURFACE)
    # storage
    sc.text(16, 176, "STORAGE", FONT_SMALL, MUTED)
    total_mb = 76319
    free_mb = 41988
    def fmt_gb(mb):
        whole = mb // 1024
        frac = (mb % 1024) * 10 // 1024
        return "%d.%d GB" % (whole, frac)
    sc.text_right(W - 16, 176, fmt_gb(free_mb) + " free", FONT_SUB, MUTED_D)
    bx, by, bw, bh = 16, 182, W - 32, 8
    sc.fill_round_rect(bx, by, bw, bh, 4, TRK)
    used = total_mb - free_mb
    fw = int(used * bw / total_mb)
    sc.fill_round_rect(bx, by, fw, bh, 4, ACCENT)
    # battery
    pct = 78
    sc.text(16, 212, "BATTERY", FONT_SMALL, MUTED)
    sc.text_right(W - 16, 212, str(pct) + "%", FONT_SUB, INK)
    gx, gy, gw, gh = 16, 218, W - 32 - 5, 12
    sc.fill_round_rect(gx, gy, gw, gh, 3, TRK)
    sc.fill_rect(gx + gw, gy + 3, 4, gh - 6, TRK)
    fw2 = (gw - 4) * pct // 100
    sc.fill_round_rect(gx + 2, gy + 2, fw2, gh - 4, 2, ACCENT)
    return sc.img


# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------
def upscale(im, scale):
    return im.resize((W * scale, H * scale), Image.NEAREST)

def save_png(im, name):
    path = os.path.join(HERE, name)
    upscale(im, SCALE).save(path)
    return path


# ---------------------------------------------------------------------------
# Menu screens (main + Music submenu) — real rows from core/kernel/main.c
# ---------------------------------------------------------------------------
MAIN_MENU = [   # (label, active) — idle: "Now Playing" row is hidden
    ("Music", True), ("Playlists", False), ("Podcasts", False),
    ("Audiobooks", False), ("Settings", True),
]
MUSIC_MENU = [
    ("Playlists", False), ("Artists", True), ("Albums", True), ("Songs", True),
    ("Shuffle Songs", True), ("Genres", True), ("Composers", False),
    ("Audiobooks", False),
]

def screen_menu(title, items, sel, back):
    sc = Screen()
    status_strip(sc, "CORE")
    header(sc, title, back=back)
    for i, (label, active) in enumerate(items):
        if i >= LIST_ROWS:
            break
        list_row(sc, LIST_Y0, i, label, chevron=True, selected=(i == sel),
                 greyed=not active)
    return sc.img


def build_walkthrough_gif():
    """A little "someone using the iPod" story: main menu -> Music -> Albums
    (with the long title marqueeing) -> album detail -> now playing, with the
    selection bar visibly stepping row to row and the progress bar advancing."""
    frames = []
    durations = []

    def add(im, hold=1, ms=140):
        p = upscale(im, GIF_SCALE).convert("P", palette=Image.ADAPTIVE, colors=96)
        for _ in range(hold):
            frames.append(p)
            durations.append(ms)

    # 1) MAIN MENU — bar starts on Music, browses down, settles back on Music.
    add(screen_menu("Core", MAIN_MENU, 0, False), hold=3)   # Music
    add(screen_menu("Core", MAIN_MENU, 1, False), hold=2)   # Playlists
    add(screen_menu("Core", MAIN_MENU, 2, False), hold=3)   # Podcasts
    add(screen_menu("Core", MAIN_MENU, 1, False), hold=2)   # back up
    add(screen_menu("Core", MAIN_MENU, 0, False), hold=4)   # settle on Music

    # 2) MUSIC SUBMENU — step from Artists down to Albums.
    add(screen_menu("Music", MUSIC_MENU, 1, True), hold=3)  # Artists
    add(screen_menu("Music", MUSIC_MENU, 2, True), hold=4)  # Albums

    # 3) ALBUMS LIST — bar steps down onto the long-titled album; marquee scrolls.
    add(screen_albums(sel=0), hold=3)                       # AUSTIN
    add(screen_albums(sel=1, title_offset=0), hold=3)       # long title (start)
    # marquee reveal
    t = ALBUMS[1][0]
    tx = 12 + 28 + 8
    avail = (W - 16) - tx
    max_off = max(0, text_width(t, FONT_HEADER) - avail)
    o = 0
    while o < max_off:
        o = min(max_off, o + 6)
        add(screen_albums(sel=1, title_offset=o), hold=1, ms=90)
    add(screen_albums(sel=1, title_offset=max_off), hold=3)  # dwell on tail
    add(screen_albums(sel=0), hold=3)                        # bar back to AUSTIN, select

    # 4) ALBUM DETAIL — step down a couple of tracks, settle on "Something Real".
    add(screen_detail(sel=0), hold=3)                        # Don't Understand
    add(screen_detail(sel=1), hold=2)                        # Something Real
    add(screen_detail(sel=2), hold=3)                        # Chemical
    add(screen_detail(sel=1), hold=4)                        # settle -> select

    # 5) NOW PLAYING — progress bar advances ~5% -> ~35%, times tick.
    total = 182
    for pct in range(5, 36, 4):
        add(_now_playing_base(elapsed=int(total * pct / 100)).img, hold=1, ms=150)
    add(_now_playing_base(elapsed=int(total * 0.35)).img, hold=6)  # hold, then loop

    path = os.path.join(HERE, "demo.gif")
    frames[0].save(path, save_all=True, append_images=frames[1:], loop=0,
                   duration=durations, optimize=True, disposal=2)
    return path, len(frames), sum(durations)


def main():
    outputs = []
    outputs.append(save_png(screen_albums(), "albums.png"))
    outputs.append(save_png(screen_nowplaying(), "nowplaying.png"))
    outputs.append(save_png(screen_detail(), "detail.png"))
    outputs.append(save_png(screen_genres(), "genres.png"))
    outputs.append(save_png(screen_about(), "about.png"))
    outputs.append(save_png(screen_volume(), "volume.png"))
    outputs.append(save_png(screen_lock(), "lock.png"))
    outputs.append(save_png(screen_locked(), "locked.png"))
    gif_path, nframes, total_ms = build_walkthrough_gif()
    outputs.append(gif_path)
    for p in outputs:
        print("wrote", p, os.path.getsize(p), "bytes")
    print("demo.gif: %d frames, %dx%d, %.1fs loop" %
          (nframes, W * GIF_SCALE, H * GIF_SCALE, total_ms / 1000.0))


if __name__ == "__main__":
    main()
