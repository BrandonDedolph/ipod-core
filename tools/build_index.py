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

REC_FMT = "<IHH64s64s48s40s24sII"        # 256 bytes (…24s + folder_hash + file_hash)
assert struct.calcsize(REC_FMT) == 256

BAD = re.compile(r'[\\/:*?"<>|]')
def fat_safe(s): return BAD.sub("_", s).rstrip(" .")
def straighten(s): return (s or "").replace('’', "'").replace('‘', "'")

def utf8_field(s, n):
    """Store the name as UTF-8 (the firmware atlas covers Latin-1 + smart
    punctuation now), dropping only C0 control chars. Truncate to n-1 bytes on a
    char boundary (decode('ignore') drops any split trailing sequence)."""
    s = "".join(c for c in (s or "") if ord(c) >= 0x20)
    return s.encode("utf-8")[:n-1].decode("utf-8", "ignore").encode("utf-8")

def norm_key(s):
    """Canonical form for name matching: fold smart quotes/dashes to ASCII and
    lowercase A-Z, so quote-style drift between the index and the on-disk name
    can't break a match. MUST stay identical to name_hash() in kernel/main.c."""
    out = []
    for ch in (s or ""):
        o = ord(ch)
        if   o in (0x2018, 0x2019): ch = "'"
        elif o in (0x201C, 0x201D): ch = '"'
        elif o in (0x2013, 0x2014): ch = "-"
        if "A" <= ch <= "Z": ch = ch.lower()
        out.append(ch)
    return "".join(out)

# Per-artist primary genre. The FLAC tags mostly lack a genre (or carry messy
# comma-lists), so the Genres menu was 96% "unknown". This maps each artist in
# the library to ONE clean genre; build applies it when the tag is empty or a
# multi-value comma-list. Keyed by the folder-derived artist (exact).
ARTIST_GENRE = {
    "Andrew Lucier": "Indie", "Arizona Zervas": "Hip-Hop",
    "Cameron Dallas": "Pop", "Daniel Caesar": "R&B",
    "Jaydes Archive": "Hip-Hop", "Jeremih": "R&B",
    "Jeremy Zucker": "Indie", "Joshua Slone": "Folk",
    "Juice WRLD": "Hip-Hop", "Justin Bieber": "Pop",
    "Kid Ink": "Hip-Hop", "LANY": "Pop", "Lil Skies": "Hip-Hop",
    "Lil Wayne": "Hip-Hop", "Morgan Wallen": "Country",
    "Olivver the Kid": "Indie", "Pink Sweat$": "R&B",
    "Post Malone": "Hip-Hop", "ROLE MODEL": "Indie",
    "Rex Orange County": "Indie", "Ruel": "Pop", "Russ": "Hip-Hop",
    "Solon Holt": "Country", "Steely Dan": "Rock",
    "The All-American Rejects": "Pop Punk", "The Kid LAROI": "Hip-Hop",
    "Thomas Day": "Pop", "Tommy Richman": "R&B",
    "XXXTENTACION": "Hip-Hop", "Young the Giant": "Alternative",
    "blackbear": "Pop", "ericdoa": "Hyperpop",
}

def name_hash(s):
    """FNV-1a 32 over the UTF-8 bytes of norm_key(s). Mirrors name_hash() in
    kernel/main.c byte-for-byte."""
    h = 0x811c9dc5
    for b in norm_key(s).encode("utf-8"):
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h

def split_album_artist(folder):
    i = folder.rfind(" - ")
    return (folder[i+3:].strip(), folder[:i].strip()) if i >= 0 else (None, folder)

def track_title(fname):
    stem = os.path.splitext(fname)[0]
    parts = stem.split(" - ")
    return (" - ".join(parts[2:]) if len(parts) >= 3
            else parts[1] if len(parts) == 2 else stem).strip()

def disc_tracks(adir):
    """Return [(path, disc)] — disc from the 'Disc N' subfolder (ground truth),
    else disc 1 for a flat single-disc album."""
    discs = sorted(d for d in glob.glob(os.path.join(adir, "Disc *")) if os.path.isdir(d))
    if discs:
        out = []
        for d in discs:
            m = re.search(r"Disc\s+(\d+)", os.path.basename(d))
            dn = int(m.group(1)) if m else 1
            for f in sorted(glob.glob(os.path.join(d, "*.flac"))):
                out.append((f, dn))
        return out
    return [(f, 1) for f in sorted(glob.glob(os.path.join(adir, "*.flac")))]

def leadint(s):
    m = re.match(r"\s*(\d+)", s or "")
    return int(m.group(1)) if m else 0

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
        dest = fat_safe(f"{artist_f} - {album_f}")   # FAT-safe: hash + device match
        tracks = disc_tracks(adir)
        if not tracks:
            continue
        folder_hash = name_hash(dest)                 # locator (fat-safe on both sides)
        album_disp = None                             # real album name for DISPLAY
        seen = set()
        for i, (t, disc) in enumerate(tracks, 1):
            # dest filename mirrors import_music.py: continuous "NN. Title.flac"
            ftitle = fat_safe(track_title(os.path.basename(t)))
            fname = f"{i:02d}. {ftitle}.flac"
            while fname.lower() in seen:
                fname = f"{i:02d}. {ftitle} ({len(seen)}).flac"
            seen.add(fname.lower())
            title, tartist, talbum, genre, dur, ttrk, tdisc = probe(t)
            # Trust structure over tags: disc from the "Disc N" folder, track
            # number from the filename's NN, album/artist from the folder name;
            # title from the tag with a filename fallback.
            trk = leadint(os.path.basename(t)) or ttrk or i
            if not title: title = track_title(os.path.basename(t))
            artist = artist_f or tartist
            # Genre: prefer a clean per-artist genre; only keep the tag genre if
            # the artist isn't mapped AND the tag is a single (non-comma) value.
            mapped = ARTIST_GENRE.get(artist_f)
            if mapped:
                genre = mapped
            elif genre and "," in genre:
                genre = genre.split(",")[0].strip()
            # DISPLAY folder = real "Artist - Album": use the tag album when it
            # only differs from the folder by FAT sanitization (so ?,*,:,/ come
            # back on screen — the album list & detail header read this field).
            # The locator stays fat_safe via folder_hash, so matching is unaffected.
            # Match tag album to the folder album ignoring apostrophe STYLE (the
            # folder uses straight ', some tags use curly ’); display the tag's
            # real chars but straightened, so ?,*,:,/ return while ' stays ASCII.
            if album_disp is None:
                if talbum and straighten(fat_safe(talbum)) == straighten(album_f):
                    album_disp = straighten(talbum)
                else:
                    album_disp = album_f
            folder_disp = f"{artist_f} - {album_disp}"
            recs.append(struct.pack(
                REC_FMT, dur, trk & 0xFFFF, disc & 0xFFFF,
                utf8_field(folder_disp, 64), utf8_field(fname, 64),
                utf8_field(title, 48), utf8_field(artist, 40),
                utf8_field(genre, 24),
                folder_hash, name_hash(fname)))   # folder + file locators
        print(f"  {dest}: {len(tracks)}")
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "wb") as f:
        f.write(b"CIDX" + struct.pack("<HHI", 1, 256, len(recs)))
        for r in recs:
            f.write(r)
    print(f"\nCORELIB.IDX: {len(recs)} songs, {os.path.getsize(OUT)} bytes -> {OUT}")

if __name__ == "__main__":
    main()
