#!/usr/bin/env python3
"""Generate Linen theme bitmap chrome by rendering SVG markup from the design.

The SVG strings here are lifted directly from themes.jsx components
(Battery, ShuffleIcon, RepeatIcon, StarRating). Rendering goes through
cairosvg so we get pixel-true geometry matching the design — no hand-coded
PIL approximations.
"""

from __future__ import annotations
from io import BytesIO
from pathlib import Path

import cairosvg
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "theme/.rockbox/wps/linen"
OUT.mkdir(parents=True, exist_ok=True)

# Linen palette (hex strings for SVG fill/stroke)
INK = "#1a1714"
SURFACE = "#f4f1ec"
MUTED = "#9a8e80"
MUTED_DEEP = "#5a5048"
MUTED_RGBA_18 = "rgba(26,23,20,0.18)"


def render_svg(svg: str, width: int, height: int) -> Image.Image:
    """Render SVG markup at exact pixel dimensions, on cream surface."""
    png = cairosvg.svg2png(
        bytestring=svg.encode("utf-8"),
        output_width=width,
        output_height=height,
    )
    fg = Image.open(BytesIO(png)).convert("RGBA")
    bg = Image.new("RGB", (width, height), (0xF4, 0xF1, 0xEC))
    bg.paste(fg, (0, 0), fg)
    return bg


# SVG fragments — adapted from themes.jsx
def battery_svg(level: float, charging: bool = False, color: str = INK) -> str:
    """Outline-only battery (no text — text is overlaid in WPS).
    Width 32, height 11 to match design's showPct=true variant."""
    fill_w = 25 * level
    bolt = ('<path d="M14 1.5 L10.5 5.5 L13 5.5 L11.5 9.5 L15.5 5 L13 5 Z" '
            f'fill="{color}" />') if charging else ""
    return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 11">
  <rect x="0.5" y="0.5" width="28" height="10" rx="1.5" fill="none"
        stroke="{color}" stroke-width="1" />
  <rect x="29" y="3" width="2" height="5" fill="{color}" />
  <rect x="2" y="2" width="{fill_w}" height="7" fill="{color}" opacity="0.18" />
  {bolt}
</svg>'''


def shuffle_svg(on: bool, color: str = INK) -> str:
    """ShuffleIcon — viewBox 14x11 in JSX. Render at 12x9 to match our slot."""
    op = 1.0 if on else 0.25
    return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 14 11" opacity="{op}">
  <path d="M1 2 L4 2 L8 8.5 L11 8.5" stroke="{color}" stroke-width="1.4"
        fill="none" stroke-linecap="round" stroke-linejoin="round" />
  <path d="M1 8.5 L4 8.5 L5.2 6.5" stroke="{color}" stroke-width="1.4"
        fill="none" stroke-linecap="round" />
  <path d="M7.2 4.5 L8 3.2 L11 3.2" stroke="{color}" stroke-width="1.4"
        fill="none" stroke-linecap="round" stroke-linejoin="round" />
  <path d="M9.5 1.4 L11.7 3.2 L9.5 5" stroke="{color}" stroke-width="1.4"
        fill="none" stroke-linecap="round" stroke-linejoin="round" />
  <path d="M9.5 6.7 L11.7 8.5 L9.5 10.3" stroke="{color}" stroke-width="1.4"
        fill="none" stroke-linecap="round" stroke-linejoin="round" />
</svg>'''


def repeat_svg(mode: str, color: str = INK) -> str:
    """RepeatIcon. mode: 'off' | 'all' | 'one'."""
    op = 0.25 if mode == "off" else 1.0
    one_text = (
        f'<text x="7" y="7.4" text-anchor="middle" font-family="Nunito,sans-serif" '
        f'font-size="5" font-weight="800" fill="{color}">1</text>'
        if mode == "one" else "")
    return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 14 11" opacity="{op}">
  <path d="M3 2 L11 2 L11 5" stroke="{color}" stroke-width="1.4" fill="none"
        stroke-linecap="round" stroke-linejoin="round" />
  <path d="M11 9 L3 9 L3 6" stroke="{color}" stroke-width="1.4" fill="none"
        stroke-linecap="round" stroke-linejoin="round" />
  <path d="M9 0.4 L11.4 2 L9 3.6" stroke="{color}" stroke-width="1.4" fill="none"
        stroke-linecap="round" stroke-linejoin="round" />
  <path d="M5 7.4 L2.6 9 L5 10.6" stroke="{color}" stroke-width="1.4" fill="none"
        stroke-linecap="round" stroke-linejoin="round" />
  {one_text}
</svg>'''


def hold_svg(locked: bool, color: str = INK) -> str:
    """Closed padlock when locked, blank otherwise. From menus.jsx StatusStrip."""
    if not locked:
        return f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 9 11"></svg>'
    return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 9 11" fill="none">
  <rect x="0.5" y="4.5" width="8" height="6" rx="1" fill="{color}" />
  <path d="M2.2 4.5V3a2.3 2.3 0 1 1 4.6 0v1.5"
        stroke="{color}" stroke-width="1.1" fill="none" />
</svg>'''


def stars_svg(value: int, color: str = INK, muted: str = MUTED_RGBA_18) -> str:
    """5 stars × 8px each, gap 1.5. Total ~46.5x8. value: 0..5."""
    stars = []
    for i in range(5):
        c = color if i < value else muted
        x = i * 9.5
        stars.append(
            f'<polygon points="{x+5},0.5 {x+6.3},3.7 {x+9.7},3.9 '
            f'{x+7.1},6.1 {x+7.9},9.5 {x+5},7.7 {x+2.1},9.5 '
            f'{x+2.9},6.1 {x+0.3},3.9 {x+3.7},3.7" fill="{c}" />')
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 47 10">'
            f'{"".join(stars)}</svg>')


def stack_strip(cells: list[Image.Image], filename: str) -> None:
    """Stack cells vertically into a single BMP."""
    w = cells[0].width
    h = cells[0].height
    out = Image.new("RGB", (w, h * len(cells)), (0xF4, 0xF1, 0xEC))
    for i, cell in enumerate(cells):
        out.paste(cell, (0, i * h))
    out.convert("RGB").save(OUT / filename, "BMP")


# ============================================================
# Build all strips
# ============================================================

def build_battery() -> None:
    """11 cells (32x11): 0..100% in 10% steps, plus 12th cell for charging."""
    cells = []
    for i in range(11):
        cells.append(render_svg(battery_svg(i / 10.0), 32, 11))
    cells.append(render_svg(battery_svg(0.5, charging=True), 32, 11))
    stack_strip(cells, "battery.bmp")


def build_shuffle() -> None:
    """2 cells (12x9): off, on."""
    cells = [render_svg(shuffle_svg(False), 12, 9),
             render_svg(shuffle_svg(True), 12, 9)]
    stack_strip(cells, "shuffle.bmp")


def build_repeat() -> None:
    """3 cells (12x9): off, all, one."""
    cells = [render_svg(repeat_svg("off"), 12, 9),
             render_svg(repeat_svg("all"), 12, 9),
             render_svg(repeat_svg("one"), 12, 9)]
    stack_strip(cells, "repeat.bmp")


def build_hold() -> None:
    """2 cells (9x11): unlocked (blank), locked."""
    cells = [render_svg(hold_svg(False), 9, 11),
             render_svg(hold_svg(True), 9, 11)]
    stack_strip(cells, "hold.bmp")


def build_stars() -> None:
    """11 cells (47x10): 0..10 ratings. Rockbox %rr is 0-10; we map 2-step to 1 star."""
    cells = []
    for level in range(11):
        # 0->0★, 1->half (treat as 0★ for simple), 2->1★, etc.
        v = level // 2
        cells.append(render_svg(stars_svg(v), 47, 10))
    stack_strip(cells, "stars.bmp")


def build_progress() -> None:
    """3px-tall, 2-row bitmap: track row + fill row."""
    from PIL import ImageDraw
    w, h = 284, 3
    img = Image.new("RGB", (w, h * 2), (0xF4, 0xF1, 0xEC))
    d = ImageDraw.Draw(img)
    # Track: 12% blend of ink toward bg ≈ #DEDAD3
    d.rectangle((0, 0, w - 1, h - 1), fill=(0xDE, 0xDA, 0xD3))
    # Fill: ink
    d.rectangle((0, h, w - 1, 2 * h - 1), fill=(0x1A, 0x17, 0x14))
    img.save(OUT / "progress.bmp", "BMP")


def build_divider() -> None:
    """1x8 vertical hairline at 15% ink."""
    from PIL import ImageDraw
    img = Image.new("RGB", (1, 8), (0xF4, 0xF1, 0xEC))
    d = ImageDraw.Draw(img)
    d.line((0, 0, 0, 7), fill=(0xD2, 0xCD, 0xC4))
    img.save(OUT / "divider.bmp", "BMP")


def build_format_box() -> None:
    """Small outlined box for "MP3"-style codec frame, 28x11."""
    from PIL import ImageDraw
    w, h = 28, 11
    img = Image.new("RGB", (w, h), (0xF4, 0xF1, 0xEC))
    d = ImageDraw.Draw(img)
    d.rectangle((0, 0, w - 1, h - 1), outline=(0x1A, 0x17, 0x14), width=1)
    img.save(OUT / "fmtbox.bmp", "BMP")


if __name__ == "__main__":
    build_battery()
    build_shuffle()
    build_repeat()
    build_hold()
    build_stars()
    build_progress()
    build_divider()
    build_format_box()
    print("Generated:")
    for p in sorted(OUT.glob("*.bmp")):
        print(f"  {p.name}: {p.stat().st_size} bytes")
