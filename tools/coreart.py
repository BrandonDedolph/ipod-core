#!/usr/bin/env python3
"""coreart.py — extract embedded FLAC album art into a CoreArt RGB565 sidecar.

The firmware can't decode JPEG on the FPU-less ARM7 in real time, so album art
is pre-rendered on the host to a fixed-size RGB565 bitmap the device just blits.
This is the reference implementation of that step (the companion loader app will
do the same). ffmpeg does the heavy lifting: it exposes a FLAC's embedded
PICTURE block as a video stream, and can scale + convert to raw rgb565le in one
pass.

CoreArt sidecar format (little-endian):
    off 0  : magic  "CART"        (4 bytes)
    off 4  : u16    version = 1
    off 6  : u16    width
    off 8  : u16    height
    off 10 : u16    reserved = 0
    off 12 : width*height * u16   RGB565 pixels, row-major (rgb565le)

Usage:
    coreart.py <input.flac> <output.art> [size]      # one file
    coreart.py --album <folder> <output.art> [size]  # first FLAC in a folder
"""
import sys, subprocess, struct, glob, os

MAGIC = b"CART"
VERSION = 1


def extract(flac_path, size):
    raw = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", flac_path, "-an", "-map", "0:v:0",
         "-vf", f"scale={size}:{size}:flags=lanczos",
         "-pix_fmt", "rgb565le", "-f", "rawvideo", "-"],
        capture_output=True).stdout
    if len(raw) != size * size * 2:
        raise RuntimeError(f"{flac_path}: got {len(raw)} bytes, "
                           f"expected {size*size*2} (no embedded art?)")
    return raw


def write_art(out_path, raw, size):
    hdr = MAGIC + struct.pack("<HHHH", VERSION, size, size, 0)
    with open(out_path, "wb") as f:
        f.write(hdr + raw)
    print(f"{out_path}: {size}x{size} ({len(hdr)+len(raw)} bytes)")


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__); sys.exit(2)
    if args[0] == "--album":
        folder, out = args[1], args[2]
        size = int(args[3]) if len(args) > 3 else 120
        flacs = sorted(glob.glob(os.path.join(folder, "*.flac")) +
                       glob.glob(os.path.join(folder, "*.FLAC")))
        if not flacs:
            raise RuntimeError(f"no FLAC in {folder}")
        src = flacs[0]
    else:
        src, out = args[0], args[1]
        size = int(args[2]) if len(args) > 2 else 120
    write_art(out, extract(src, size), size)


if __name__ == "__main__":
    main()
