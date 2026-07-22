#!/usr/bin/env python3
"""build_index.py — build the device library index (CORELIB.IDX) from a source
music tree, matching the on-device layout that import_music.py produced.

The iPod's FAT volume is READ-ONLY to the firmware, so the library index can't
be built on-device. Instead this host tool reads each track's tags (ffprobe)
and emits ONE binary index the firmware loads in a single read — no per-file
tag scan at boot. The device falls back to scanning only if this file is absent.

Layout it must agree with (see import_music.py): album folders are
"Artist - Album", tracks are "NN. Title.flac", multi-disc flattened.

CORELIB.IDX (little-endian):
  header: magic "CIDX"(4), u16 version=1, u16 rec_size=256, u32 count
  record (256 B): u32 duration_s, u16 track, u16 disc,
                  folder[64], file[64], title[48], artist[40], genre[24], pad
"""
import os, re, sys, glob, json, struct, subprocess

SRC = "/mnt/c/Users/brandon-home/Music/MC"
OUT = "/mnt/c/Users/brandon-home/ipod-staging/CORELIB.IDX"
DEST_WIN = "D:"

REC_FMT = "<IHH64s64s48s40s24s8x"        # 256 bytes
assert struct.calcsize(REC_FMT) == 256

BAD = re.compile(r'[\\/:*?"<>|]')
def fat_safe(s): return BAD.sub("_", s).rstrip(" .")

def ascii_field(s, n):
    b = "".join(c for c in (s or "") if 0x20 <= ord(c) <= 0x7E).encode("ascii", "ignore")
    return b[:n-1]                          # leave room for NUL

def split_album_artist(folder):
    i = folder.rfind(" - ")
    return (folder[i+3:].strip(), folder[:i].strip()) if i >= 0 else (None, folder)

def track_title(fname):
    stem = os.path.splitext(fname)[0]
    parts = stem.split(" - ")
    return (" - ".join(parts[2:]) if len(parts) >= 3
            else parts[1] if len(parts) == 2 else stem).strip()

def disc_tracks(adir):
    discs = sorted(d for d in glob.glob(os.path.join(adir, "Disc *")) if os.path.isdir(d))
    if discs:
        out = []
        for d in discs:
            out += sorted(glob.glob(os.path.join(d, "*.flac")))
        return out
    return sorted(glob.glob(os.path.join(adir, "*.flac")))

def probe(path):
    """Return (title, artist, album, genre, duration_s, track, disc) from tags."""
    try:
        out = subprocess.run(
            ["ffprobe", "-v", "quiet", "-print_format", "json",
             "-show_format", path], capture_output=True, text=True).stdout
        fmt = json.loads(out).get("format", {})
        tags = { k.lower(): v for k, v in fmt.get("tags", {}).items() }
        dur = int(float(fmt.get("duration", 0)))
        def leadint(s):
            m = re.match(r"\s*(\d+)", s or "");  return int(m.group(1)) if m else 0
        return (tags.get("title", ""), tags.get("artist", tags.get("album_artist", "")),
                tags.get("album", ""), tags.get("genre", ""), dur,
                leadint(tags.get("track", "")), leadint(tags.get("disc", "")))
    except Exception:
        return ("", "", "", "", 0, 0, 0)

def main():
    recs = []
    for folder in sorted(os.listdir(SRC)):
        adir = os.path.join(SRC, folder)
        if not os.path.isdir(adir):
            continue
        artist_f, album_f = split_album_artist(folder)
        if not artist_f:
            continue
        dest = fat_safe(f"{artist_f} - {album_f}")
        tracks = disc_tracks(adir)
        if not tracks:
            continue
        seen = set()
        for i, t in enumerate(tracks, 1):
            ftitle = fat_safe(track_title(os.path.basename(t)))
            fname = f"{i:02d}. {ftitle}.flac"
            while fname.lower() in seen:
                fname = f"{i:02d}. {ftitle} ({len(seen)}).flac"
            seen.add(fname.lower())
            title, artist, album, genre, dur, trk, disc = probe(t)
            if not title:  title = track_title(os.path.basename(t))
            if not artist: artist = artist_f
            recs.append(struct.pack(
                REC_FMT, dur, trk & 0xFFFF, disc & 0xFFFF,
                ascii_field(dest, 64), ascii_field(fname, 64),
                ascii_field(title, 48), ascii_field(artist, 40),
                ascii_field(genre, 24)))
        print(f"  {dest}: {len(tracks)}")
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "wb") as f:
        f.write(b"CIDX" + struct.pack("<HHI", 1, 256, len(recs)))
        for r in recs:
            f.write(r)
    print(f"\nCORELIB.IDX: {len(recs)} songs, {os.path.getsize(OUT)} bytes -> {OUT}")

if __name__ == "__main__":
    main()
