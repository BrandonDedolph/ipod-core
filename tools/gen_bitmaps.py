#!/usr/bin/env python3
"""Generate bitmap chrome for Linen / Paper / Card / Ink themes.

Each theme gets its own dir under theme/.rockbox/wps/<theme>/ with the same
filenames; only the colors differ.
"""

from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
WPS_ROOT = ROOT / "theme/.rockbox/wps"

# Palettes: (surface_bg, ink_fg, accent_for_progress)
PALETTES = {
    "linen": ((0xF4, 0xF1, 0xEC), (0x1A, 0x17, 0x14), (0x1A, 0x17, 0x14)),
    "paper": ((0xFA, 0xF8, 0xF4), (0x1A, 0x17, 0x14), (0x1A, 0x17, 0x14)),
    "card":  ((0xEE, 0xEA, 0xE3), (0x1A, 0x17, 0x14), (0x1A, 0x17, 0x14)),
    # Ink: true dark, terracotta accent
    "ink":   ((0x0E, 0x0D, 0x0C), (0xE8, 0xE4, 0xDD), (0xD8, 0x7A, 0x4D)),
}


def save(img: Image.Image, theme: str, name: str) -> None:
    out_dir = WPS_ROOT / theme
    out_dir.mkdir(parents=True, exist_ok=True)
    img.convert("RGB").save(out_dir / name, "BMP")


def battery_strip(theme: str) -> None:
    """12 cells (32x11): 11 levels + charging."""
    bg, ink, _ = PALETTES[theme]
    cell_w, cell_h = 32, 11
    levels = 11
    img = Image.new("RGB", (cell_w, cell_h * (levels + 1)), bg)
    d = ImageDraw.Draw(img)
    for i in range(levels):
        y0 = i * cell_h
        d.rectangle((0, y0 + 1, 21, y0 + cell_h - 1), outline=ink, width=1)
        d.rectangle((22, y0 + 3, 23, y0 + cell_h - 4), fill=ink)
        fill_w = round(18 * (i / (levels - 1)))
        if fill_w > 0:
            d.rectangle((2, y0 + 3, 2 + fill_w - 1, y0 + cell_h - 4), fill=ink)
    yc = levels * cell_h
    d.rectangle((0, yc + 1, 21, yc + cell_h - 1), outline=ink, width=1)
    d.rectangle((22, yc + 3, 23, yc + cell_h - 4), fill=ink)
    bolt = [(11, yc + 2), (7, yc + 6), (10, yc + 6),
            (9, yc + 9), (14, yc + 5), (11, yc + 5)]
    d.polygon(bolt, fill=ink)
    save(img, theme, "battery.bmp")


def hold_strip(theme: str) -> None:
    """2 cells (9x10)."""
    bg, ink, _ = PALETTES[theme]
    cell_w, cell_h = 9, 10
    img = Image.new("RGB", (cell_w, cell_h * 2), bg)
    d = ImageDraw.Draw(img)
    yo = cell_h
    d.rectangle((0, yo + 4, 7, yo + 9), fill=ink)
    d.arc((2, yo + 1, 6, yo + 5), 180, 360, fill=ink, width=1)
    save(img, theme, "hold.bmp")


def shuffle_strip(theme: str) -> None:
    """2 cells (12x9): off (faded), on (solid)."""
    bg, ink, _ = PALETTES[theme]
    cell_w, cell_h = 12, 9
    img = Image.new("RGB", (cell_w, cell_h * 2), bg)
    # Faded color = blend ink with bg at 35%
    faded = tuple(round(bg[i] * 0.65 + ink[i] * 0.35) for i in range(3))
    for state in range(2):
        yo = state * cell_h
        color = faded if state == 0 else ink
        sub = Image.new("RGB", (cell_w, cell_h), bg)
        sd = ImageDraw.Draw(sub)
        sd.line((1, 1, 4, 1), fill=color, width=1)
        sd.line((4, 1, 7, 7), fill=color, width=1)
        sd.line((7, 7, 10, 7), fill=color, width=1)
        sd.line((1, 7, 4, 7), fill=color, width=1)
        sd.line((4, 7, 5, 5), fill=color, width=1)
        sd.line((6, 3, 7, 1), fill=color, width=1)
        sd.polygon([(8, 0), (11, 1), (8, 2)], fill=color)
        sd.polygon([(8, 6), (11, 7), (8, 8)], fill=color)
        img.paste(sub, (0, yo))
    save(img, theme, "shuffle.bmp")


def repeat_strip(theme: str) -> None:
    """3 cells (12x9): off, all, one."""
    bg, ink, _ = PALETTES[theme]
    cell_w, cell_h = 12, 9
    img = Image.new("RGB", (cell_w, cell_h * 3), bg)
    d = ImageDraw.Draw(img)
    faded = tuple(round(bg[i] * 0.65 + ink[i] * 0.35) for i in range(3))
    for state in range(3):
        yo = state * cell_h
        color = faded if state == 0 else ink
        d.line((2, yo + 1, 9, yo + 1), fill=color, width=1)
        d.line((9, yo + 1, 9, yo + 3), fill=color, width=1)
        d.polygon([(8, yo), (11, yo + 1), (8, yo + 2)], fill=color)
        d.line((2, yo + 7, 9, yo + 7), fill=color, width=1)
        d.line((2, yo + 5, 2, yo + 7), fill=color, width=1)
        d.polygon([(3, yo + 6), (0, yo + 7), (3, yo + 8)], fill=color)
        if state == 2:
            d.line((6, yo + 4, 6, yo + 6), fill=color, width=1)
            d.point((5, yo + 4), fill=color)
    save(img, theme, "repeat.bmp")


def progress_bar(theme: str) -> None:
    """3px-tall 2-row bitmap: track row + fill row.

    Rockbox slices this for solid progress rendering when given as %pb bitmap.
    """
    bg, ink, accent = PALETTES[theme]
    w, h = 284, 3
    img = Image.new("RGB", (w, h * 2), bg)
    d = ImageDraw.Draw(img)
    # Track: 12% blend of ink toward bg
    track = tuple(round(bg[i] * 0.88 + ink[i] * 0.12) for i in range(3))
    d.rectangle((0, 0, w - 1, h - 1), fill=track)
    # Fill: accent (terracotta on Ink, ink on light themes)
    d.rectangle((0, h, w - 1, 2 * h - 1), fill=accent)
    save(img, theme, "progress.bmp")


def divider(theme: str) -> None:
    """1x8 vertical hairline."""
    bg, ink, _ = PALETTES[theme]
    img = Image.new("RGB", (1, 8), bg)
    d = ImageDraw.Draw(img)
    color = tuple(round(bg[i] * 0.85 + ink[i] * 0.15) for i in range(3))
    for y in range(8):
        d.point((0, y), fill=color)
    save(img, theme, "divider.bmp")


def stars_strip(theme: str) -> None:
    """11 cells (44x8): 0..10 stars filled. Rockbox %rr returns 0-10.
    5 stars total, each 8x8, gap 1px → cell width 5*8+4*1 = 44."""
    bg, ink, _ = PALETTES[theme]
    cell_w, cell_h = 44, 8
    img = Image.new("RGB", (cell_w, cell_h * 11), bg)
    d = ImageDraw.Draw(img)
    muted = tuple(round(bg[i] * 0.82 + ink[i] * 0.18) for i in range(3))
    star_pts = [(4, 0), (5, 3), (8, 3), (5, 5), (6, 8), (4, 6), (2, 8), (3, 5), (0, 3), (3, 3)]
    for level in range(11):
        yo = level * cell_h
        # Map 0-10 to star count (5 stars total): 0=0, 2=1, 4=2, 6=3, 8=4, 10=5
        full_stars = level // 2
        half = level % 2
        for s in range(5):
            xo = s * 9
            color = ink if s < full_stars else (
                ink if (s == full_stars and half) else muted)
            pts = [(xo + p[0], yo + p[1]) for p in star_pts]
            d.polygon(pts, fill=color)
    save(img, theme, "stars.bmp")


def format_box(theme: str) -> None:
    """Single 28x11 cell: outlined "MP3"-style box for codec name.
    Used as decorative frame; codec text rendered separately as text."""
    bg, ink, _ = PALETTES[theme]
    w, h = 28, 11
    img = Image.new("RGB", (w, h), bg)
    d = ImageDraw.Draw(img)
    d.rectangle((0, 0, w - 1, h - 1), outline=ink, width=1)
    save(img, theme, "fmtbox.bmp")


if __name__ == "__main__":
    for theme in ["linen"]:
        battery_strip(theme)
        hold_strip(theme)
        shuffle_strip(theme)
        repeat_strip(theme)
        progress_bar(theme)
        divider(theme)
        stars_strip(theme)
        format_box(theme)
        print(f"  {theme}: ok")
    print("done.")
