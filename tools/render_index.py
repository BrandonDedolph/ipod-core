#!/usr/bin/env python3
"""Regenerate build/headless/index.html sorted by mtime (newest first)."""
from pathlib import Path
import time, html, os

SHOTS = Path(__file__).resolve().parent.parent / "build/headless"
SHOTS.mkdir(parents=True, exist_ok=True)

pngs = sorted(SHOTS.glob("*.png"), key=lambda p: p.stat().st_mtime, reverse=True)

cards = []
now = time.time()
for p in pngs:
    age = max(0, int(now - p.stat().st_mtime))
    age_s = f"{age}s ago" if age < 60 else f"{age // 60}m ago"
    cards.append(
        f'<figure><img src="{html.escape(p.name)}?t={int(p.stat().st_mtime)}" alt="">'
        f'<figcaption><span class="label">{html.escape(p.name)}</span> · '
        f'<span class="age">{age_s}</span></figcaption></figure>'
    )

doc = f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<title>Cabinet — headless screenshots ({len(pngs)})</title>
<style>
  body {{ margin: 0; background: #1a1714; color: #e8e4dd;
          font-family: 'Nunito', system-ui, sans-serif; padding: 24px; }}
  h1 {{ font-size: 20px; font-weight: 700; letter-spacing: -0.2px; margin: 0 0 4px; }}
  p.note {{ color: #a89e92; margin: 0 0 24px; font-size: 13px; }}
  .grid {{ display: grid; grid-template-columns: repeat(auto-fill, minmax(360px, 1fr)); gap: 18px; }}
  figure {{ margin: 0; background: #0e0d0c; border-radius: 6px; overflow: hidden;
           box-shadow: 0 4px 16px rgba(0,0,0,0.3); }}
  figure img {{ width: 100%; display: block; }}
  figcaption {{ padding: 8px 12px; font-size: 12px; font-weight: 600; color: #d8d2c8;
               display: flex; justify-content: space-between; }}
  .label {{ color: #f4ede2; }}
  .age {{ color: #a89e92; font-weight: 500; }}
</style>
<script>setTimeout(() => location.reload(), 5000);</script>
</head>
<body>
<h1>Cabinet plugin · headless screenshots ({len(pngs)})</h1>
<p class="note">Newest first. Auto-refreshes every 5 s.</p>
<div class="grid">
{chr(10).join(cards)}
</div>
</body></html>
"""
(SHOTS / "index.html").write_text(doc)
print(f"index.html regenerated with {len(pngs)} entries")
