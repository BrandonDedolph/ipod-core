#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""check_build_index.py — the host index builder (tools/build_index.py)
against a synthetic music tree.

build_index.py had no test at all, and two of its rules were silently
inconsistent with each other and with the device:

  * the filename it predicts for each track ("NN. Title.flac", NN = position
    in a lexicographic glob) and the record's `track` field came from
    different places, so the device's gutter (the record) and its row order
    (the directory, i.e. the filename order) disagreed for any album whose
    source files were not already numbered;
  * a leading number in a filename was a track number no matter what it was:
    a song called "1999" was track 1999, and "50 Cent - X.flac" track 50;
  * the disc tag was probed and then ignored, so a flattened multi-disc album
    was 1,2,3,1,2,3 all on disc 1.

This test builds a small tree that exercises each of those, stubs out ffprobe
(the tags come from a table here), runs the real main(), and reads the index
back with the documented layout. It also pins the filename contract: NN must
STILL be the lexicographic position, because that is what the files already
on the device are called — the fix is to stop deriving the track number from
it, not to renumber the files.
"""

import importlib.util
import io
import os
import pathlib
import struct
import sys
import tempfile
import zlib
from contextlib import redirect_stdout

HERE = pathlib.Path(__file__).resolve()
REPO = HERE.parent.parent.parent.parent     # <repo>
BUILD_INDEX = REPO / "tools" / "build_index.py"

REC_FMT = "<IHH64s64s48s40s24sII"
HDR_FMT = "<4sHHII"

FAILS = 0


def check(label, cond):
    global FAILS
    print(f"[{label}] {'PASS' if cond else 'FAIL'}")
    if not cond:
        FAILS += 1


def load_build_index():
    spec = importlib.util.spec_from_file_location("build_index", BUILD_INDEX)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# ---- the tree ----------------------------------------------------------------
#
# basename -> (title, track tag, disc tag). Everything else probe() returns is
# irrelevant to what is under test and left blank.
TAGS = {
    # Unnumbered source files, tagged out of alphabetical order. Lexicographic
    # enumeration is Alpha, Bravo, Charlie; the tags say Bravo is track 1.
    "Alpha.flac":   ("Alpha",   2, 0),
    "Bravo.flac":   ("Bravo",   1, 0),
    "Charlie.flac": ("Charlie", 3, 0),
    # A title that is a number, with no track tag: must NOT become track 1999.
    "1999.flac":    ("1999",    0, 0),
    # The flattened multi-disc filename convention, no tags.
    "2-03 Whatever.flac": ("Whatever", 0, 0),
    # An "Artist - Title" name where the artist starts with digits.
    "50 Cent - In Da Club.flac": ("In Da Club", 0, 0),
    # A flat album whose discs are only in the tags.
    "A.flac": ("A", 1, 1), "B.flac": ("B", 2, 1),
    "C.flac": ("C", 1, 2), "D.flac": ("D", 2, 2),
    # Disc folders: the folder wins over a (wrong) tag.
    "x.flac": ("x", 0, 9), "y.flac": ("y", 0, 9),
}

# SOURCE folders are "Album - Artist" (split_album_artist takes the artist from
# after the LAST " - "); the index and the device use "Artist - Album". The
# assertions below look records up by the destination form.
TREE = {
    "Album One - Artist A": ["Alpha.flac", "Bravo.flac", "Charlie.flac"],
    "Numbers - Prince":     ["1999.flac", "2-03 Whatever.flac"],
    "Get Rich - 50 Cent":   ["50 Cent - In Da Club.flac"],
    "Double - Band":        ["A.flac", "B.flac", "C.flac", "D.flac"],
    "Boxed - Band/Disc 1":  ["x.flac"],
    "Boxed - Band/Disc 2":  ["y.flac"],
}


def make_tree(root):
    for folder, files in TREE.items():
        d = os.path.join(root, folder)
        os.makedirs(d, exist_ok=True)
        for f in files:
            with open(os.path.join(d, f), "wb") as fh:
                fh.write(b"fLaC")          # never read: probe() is stubbed


def fake_probe(path):
    title, trk, disc = TAGS[os.path.basename(path)]
    return (title, "", "", "", 100 + trk, trk, disc)


def cstr(b):
    return b.split(b"\0", 1)[0].decode("utf-8")


def read_index(path):
    data = pathlib.Path(path).read_bytes()
    magic, ver, rec, count, crc = struct.unpack_from(HDR_FMT, data, 0)
    body = data[16:]
    recs = []
    for i in range(count):
        dur, trk, disc, folder, fname, title, artist, genre, fh, filh = \
            struct.unpack_from(REC_FMT, body, i * 256)
        recs.append(dict(dur=dur, track=trk, disc=disc, folder=cstr(folder),
                         file=cstr(fname), title=cstr(title),
                         folder_hash=fh, file_hash=filh))
    return dict(magic=magic, ver=ver, rec=rec, count=count, crc=crc,
                body=body, size=len(data)), recs


def by_folder(recs, folder):
    return [r for r in recs if r["folder"] == folder]


def main():
    bi = load_build_index()

    # ---- 1. the leading-number reader, in isolation ------------------------
    lt = bi.lead_track
    check("lead_track: '01. Title' is track 1", lt("01. Title") == (0, 1))
    check("lead_track: '12 Title' is track 12", lt("12 Title") == (0, 12))
    check("lead_track: '01 - Title' is track 1", lt("01 - Title") == (0, 1))
    check("lead_track: '1-01 Title' is disc 1 track 1", lt("1-01 Title") == (1, 1))
    check("lead_track: '1999' is not a track number", lt("1999") == (0, 0))
    check("lead_track: '1999 - Title' is not a track number",
          lt("1999 - Title") == (0, 0))
    check("lead_track: '2Pac - Title' is not a track number",
          lt("2Pac - Title") == (0, 0))
    check("lead_track: a bare number is not a track number", lt("7") == (0, 0))
    check("lead_track: the album's own artist is not a track number",
          lt("50 Cent - In Da Club", "50 Cent") == (0, 0))
    check("lead_track: ...but a different leading number still is",
          lt("50 Cent - In Da Club", "Eminem") == (0, 50))
    check("track_number: the tag beats the filename",
          bi.track_number("03. Foo", 7, 0, 0) == (1, 7))
    check("track_number: a Disc folder beats the disc tag",
          bi.track_number("x", 0, 9, 2) == (2, 0))
    check("track_number: the disc tag is used for a flat album",
          bi.track_number("x", 4, 2, 0) == (2, 4))

    # ---- 2. the tool refuses to guess paths --------------------------------
    for k in ("CORELIB_SRC", "CORELIB_OUT"):
        os.environ.pop(k, None)
    try:
        with redirect_stdout(io.StringIO()):
            sys.stderr, saved = io.StringIO(), sys.stderr
            try:
                bi.parse_args([])
            finally:
                sys.stderr = saved
        check("no --src/--out and no env: refused", False)
    except SystemExit as e:
        check("no --src/--out and no env: refused", e.code != 0)
    os.environ["CORELIB_SRC"] = "/nowhere/src"
    os.environ["CORELIB_OUT"] = "/nowhere/out.idx"
    a = bi.parse_args([])
    check("CORELIB_SRC / CORELIB_OUT are honoured",
          a.src == "/nowhere/src" and a.out == "/nowhere/out.idx")
    a = bi.parse_args(["--src", "/x", "--out", "/y"])
    check("explicit flags beat the environment", a.src == "/x" and a.out == "/y")
    for k in ("CORELIB_SRC", "CORELIB_OUT"):
        os.environ.pop(k, None)
    check("no machine-specific path is left in the tool",
          "/mnt/c/" not in BUILD_INDEX.read_text(encoding="utf-8"))

    # ---- 3. a real run over the tree ---------------------------------------
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "src")
        out = os.path.join(tmp, "CORELIB.IDX")
        make_tree(src)
        bi.probe = fake_probe                 # main() looks probe up by name
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = bi.main(["--src", src, "--out", out, "--genre-map", "",
                          "--max-songs", "6000", "-q", "--show-drift"])
        report = buf.getvalue()
        check("main() succeeds", rc == 0)
        hdr, recs = read_index(out)

    # -- the file ----------------------------------------------------------
    check("header: magic", hdr["magic"] == b"CIDX")
    check("header: version 2", hdr["ver"] == 2)
    check("header: record size 256", hdr["rec"] == 256)
    check("header: count matches the tree",
          hdr["count"] == sum(len(f) for f in TREE.values()) == len(recs))
    check("file size == 16 + 256 * count", hdr["size"] == 16 + 256 * hdr["count"])
    check("header: CRC-32 is zlib's over the records",
          hdr["crc"] == zlib.crc32(hdr["body"]) & 0xFFFFFFFF)
    check("pack_index(): header + records, CRC included",
          bi.pack_index([b"\0" * 256]) ==
          struct.pack(HDR_FMT, b"CIDX", 2, 256, 1, zlib.crc32(b"\0" * 256)) +
          b"\0" * 256)

    # -- the filename contract: NN is the lexicographic position ------------
    one = by_folder(recs, "Artist A - Album One")
    files = sorted(r["file"] for r in one)
    check("filenames keep the importer's enumeration (lexicographic)",
          files == ["01. Alpha.flac", "02. Bravo.flac", "03. Charlie.flac"])
    check("file_hash is over that filename",
          all(r["file_hash"] == bi.name_hash(r["file"]) for r in recs))
    check("folder_hash is over the FAT-safe folder name",
          all(r["folder_hash"] == bi.name_hash(r["folder"]) for r in one))

    # -- the track number is the tag, and the records are in that order ------
    check("track numbers come from the tags, not the filename position",
          {r["file"]: r["track"] for r in one} ==
          {"01. Alpha.flac": 2, "02. Bravo.flac": 1, "03. Charlie.flac": 3})
    check("records within an album are in (disc, track) order",
          [r["title"] for r in one] == ["Bravo", "Alpha", "Charlie"])

    # -- a leading number that is not a track number --------------------------
    nums = {r["title"]: r for r in by_folder(recs, "Prince - Numbers")}
    check("'1999' with no track tag is track 1 (its position), not 1999",
          nums["1999"]["track"] == 1)
    check("'2-03 Whatever' is disc 2 track 3",
          (nums["Whatever"]["disc"], nums["Whatever"]["track"]) == (2, 3))
    cent = by_folder(recs, "50 Cent - Get Rich")[0]
    check("'50 Cent - In Da Club' is not track 50", cent["track"] == 1)

    # -- discs -------------------------------------------------------------------
    dbl = by_folder(recs, "Band - Double")
    check("a flat album's disc tags are read",
          [(r["disc"], r["track"]) for r in dbl] == [(1, 1), (1, 2), (2, 1), (2, 2)])
    box = {r["title"]: r for r in by_folder(recs, "Band - Boxed")}
    check("a Disc folder beats the disc tag",
          (box["x"]["disc"], box["y"]["disc"]) == (1, 2))
    check("Disc-folder tracks are numbered continuously across discs",
          (box["x"]["track"], box["y"]["track"]) == (1, 2))

    # -- the drift report: the evidence for the "52 tracks" question ----------
    # Alpha (pos 1, track 2), Bravo (2, 1), Whatever (2, 3), C (3, 1), D (4, 2).
    check("the run reports how many tracks are numbered off their position",
          "5 track(s) are numbered differently" in report)
    check("--show-drift names them",
          "02. Bravo.flac: disc 1 track 1 (tag)" in report and
          "02. 2-03 Whatever.flac" in report and "(filename)" in report)

    if FAILS:
        print(f"\nbuild_index: FAIL ({FAILS} failure{'s' if FAILS != 1 else ''})")
        return 1
    print("\nbuild_index: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
