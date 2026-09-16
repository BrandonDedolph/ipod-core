// SPDX-License-Identifier: Apache-2.0

package syncer

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"
	"time"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/cidx"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/coreart"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/devicefs"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flac"
)

// --- fixtures ---------------------------------------------------------------

// coverPNG is a small, non-uniform picture: uniform art would pass a broken
// resampler just as happily as a working one.
func coverPNG(t *testing.T) []byte {
	t.Helper()
	img := image.NewRGBA(image.Rect(0, 0, 8, 8))
	for y := 0; y < 8; y++ {
		for x := 0; x < 8; x++ {
			img.Set(x, y, color.RGBA{R: uint8(x * 31), G: uint8(y * 31), B: 0x40, A: 0xFF})
		}
	}
	var buf bytes.Buffer
	if err := png.Encode(&buf, img); err != nil {
		t.Fatal(err)
	}
	return buf.Bytes()
}

// writeTrack lays down one real (tiny) FLAC.
func writeTrack(t *testing.T, dir, name string, tags map[string]string, cover []byte) string {
	t.Helper()
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	var pics []flac.Picture
	if cover != nil {
		pics = []flac.Picture{{Type: flac.PictureTypeFrontCover, MIME: "image/png", Data: cover}}
	}
	raw := flac.BuildFile(
		flac.StreamInfo{SampleRate: 44100, Channels: 2, BitsPerSample: 16, TotalSamples: 44100},
		tags, pics)
	p := filepath.Join(dir, name)
	if err := os.WriteFile(p, raw, 0o644); err != nil {
		t.Fatal(err)
	}
	return p
}

// srcTree is the source library every test below syncs from:
//
//	Blue - Artist A/     Alpha.flac, Beta.flac   (with an embedded cover)
//	Red - Artist B/      Gamma.flac              (no cover)
//	Playlists/mix.m3u8   two real lines (one with backslashes) + one that
//	                     names nothing
//
// Device side: "Artist A - Blue/01. Alpha.flac" and so on.
func srcTree(t *testing.T) string {
	t.Helper()
	src := t.TempDir()
	cover := coverPNG(t)

	blue := filepath.Join(src, "Blue - Artist A")
	writeTrack(t, blue, "Alpha.flac", map[string]string{
		"title": "Alpha", "artist": "Artist A", "album": "Blue",
		"tracknumber": "1", "genre": "Pop",
	}, cover)
	writeTrack(t, blue, "Beta.flac", map[string]string{
		"title": "Beta", "artist": "Artist A", "album": "Blue",
		"tracknumber": "2", "genre": "Pop",
	}, cover)

	red := filepath.Join(src, "Red - Artist B")
	writeTrack(t, red, "Gamma.flac", map[string]string{
		"title": "Gamma", "artist": "Artist B", "album": "Red",
		"tracknumber": "1", "genre": "Rock",
	}, nil)

	pl := filepath.Join(src, devicefs.PlaylistDir)
	if err := os.MkdirAll(pl, 0o755); err != nil {
		t.Fatal(err)
	}
	// Line 2 is relative to the playlist's own folder and spelled with
	// backslashes (what a Windows player writes); line 3 is relative to the
	// source root; line 4 names a track that is not in the tree.
	m3u := "#EXTM3U\n" +
		"..\\Blue - Artist A\\Alpha.flac\n" +
		"Red - Artist B/Gamma.flac\n" +
		"Nowhere - Artist Z/Missing.flac\n"
	if err := os.WriteFile(filepath.Join(pl, "mix.m3u8"), []byte(m3u), 0o644); err != nil {
		t.Fatal(err)
	}
	return src
}

func opts(src, dst string) Options {
	return Options{Src: src, Dst: dst}
}

func planFor(t *testing.T, o Options) *Plan {
	t.Helper()
	scan, err := ScanSource(o)
	if err != nil {
		t.Fatalf("ScanSource: %v", err)
	}
	p, err := MakePlan(o, scan)
	if err != nil {
		t.Fatalf("MakePlan: %v", err)
	}
	return p
}

func syncOnce(t *testing.T, o Options) (*Plan, *Report) {
	t.Helper()
	p := planFor(t, o)
	rep, err := Execute(context.Background(), p, o)
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}
	return p, rep
}

// walkFiles lists every file under root, relative and slash-separated.
func walkFiles(t *testing.T, root string) []string {
	t.Helper()
	var out []string
	err := filepath.Walk(root, func(p string, fi os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		if fi.IsDir() {
			return nil
		}
		rel, err := filepath.Rel(root, p)
		if err != nil {
			return err
		}
		out = append(out, filepath.ToSlash(rel))
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	sort.Strings(out)
	return out
}

// --- tests ------------------------------------------------------------------

// TestFreshSyncWritesEverythingAndIndexLast is the whole slice in one test:
// the tree, the sidecars, the playlist, the two device files and the index,
// with the index provably the last thing written. The ordered write log is
// used rather than mtimes: FAT keeps time to two seconds, so "the index is
// newer" is not a question a timestamp can answer.
func TestFreshSyncWritesEverythingAndIndexLast(t *testing.T) {
	src, dst := srcTree(t), t.TempDir()
	o := opts(src, dst)

	var albums []string
	o.Progress = func(e Event) {
		if e.Kind == EventAlbum {
			albums = append(albums, fmt.Sprintf("%s copied=%d skipped=%d art=%s", e.Album, e.Copied, e.Skipped, e.Art))
		}
	}
	p, rep := syncOnce(t, o)

	if p.Albums != 2 || p.Tracks != 3 {
		t.Fatalf("plan = %d albums / %d tracks, want 2/3", p.Albums, p.Tracks)
	}
	if rep.Copied != 3 || rep.Skipped != 0 {
		t.Errorf("report: copied %d skipped %d, want 3/0", rep.Copied, rep.Skipped)
	}
	if !rep.ConfigCreated || !rep.LogCreated {
		t.Errorf("device files: config created %v, log created %v; want both", rep.ConfigCreated, rep.LogCreated)
	}
	if !rep.IndexWritten {
		t.Error("the index was not written on a fresh sync")
	}

	want := []string{
		"CORECFG.DAT",
		"CORELOG.BIN",
		"Music/Artist A - Blue/01. Alpha.flac",
		"Music/Artist A - Blue/02. Beta.flac",
		"Music/Artist A - Blue/folder.art",
		"Music/Artist A - Blue/folder.thm",
		"Music/Artist B - Red/01. Gamma.flac",
		"Music/CORELIB.IDX",
		"Music/Playlists/mix.m3u8",
	}
	if got := walkFiles(t, dst); !equalStrings(got, want) {
		t.Errorf("device tree:\n got %q\nwant %q", got, want)
	}

	// The rule: nothing is written after CORELIB.IDX.
	if n := len(rep.Written); n == 0 || filepath.Base(rep.Written[n-1]) != devicefs.IndexName {
		t.Fatalf("%s is not the last thing written; the write log is %v", devicefs.IndexName, rep.Written)
	}
	for i, w := range rep.Written[:len(rep.Written)-1] {
		if filepath.Base(w) == devicefs.IndexName {
			t.Errorf("the index was written at position %d as well as last", i)
		}
	}

	// Art is exactly what the firmware would accept, at the two dimensions
	// it reads.
	mustSidecar(t, filepath.Join(dst, "Music", "Artist A - Blue", coreart.ArtName), coreart.ArtSize)
	mustSidecar(t, filepath.Join(dst, "Music", "Artist A - Blue", coreart.ThumbName), coreart.ThumbSize)

	// The album with no embedded cover is a warning, not a failure.
	if rep.ArtMissing != 1 {
		t.Errorf("ArtMissing = %d, want 1 (Artist B - Red has no cover)", rep.ArtMissing)
	}
	if !anyContains(rep.Warnings, "no embedded cover") {
		t.Errorf("warnings do not mention the coverless album: %q", rep.Warnings)
	}

	if len(albums) != 2 {
		t.Errorf("progress: %d album events, want 2 (%q)", len(albums), albums)
	}

	// Timestamps are preserved, which is what makes the second run cheap.
	si, err := os.Stat(filepath.Join(src, "Blue - Artist A", "Alpha.flac"))
	if err != nil {
		t.Fatal(err)
	}
	di, err := os.Stat(filepath.Join(dst, "Music", "Artist A - Blue", "01. Alpha.flac"))
	if err != nil {
		t.Fatal(err)
	}
	if d := si.ModTime().Sub(di.ModTime()); d > time.Second || d < -time.Second {
		t.Errorf("copy mtime is %s off the source; the skip rule depends on it", d)
	}
}

// TestIndexBytesAreCidxEncode: the file on the device is exactly what the
// index builder would have produced — no second encoder lives in this package.
func TestIndexBytesAreCidxEncode(t *testing.T) {
	src, dst := srcTree(t), t.TempDir()
	o := opts(src, dst)
	scan, err := ScanSource(o)
	if err != nil {
		t.Fatal(err)
	}
	p, err := MakePlan(o, scan)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := Execute(context.Background(), p, o); err != nil {
		t.Fatal(err)
	}
	want := cidx.Encode(cidx.RecordsFromScan(scan))
	got, err := os.ReadFile(filepath.Join(dst, devicefs.MusicDir, devicefs.IndexName))
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got, want) {
		t.Fatalf("CORELIB.IDX is %d bytes, cidx.Encode(RecordsFromScan) is %d; they must be identical", len(got), len(want))
	}
	recs, err := cidx.Decode(got)
	if err != nil {
		t.Fatalf("the index we wrote does not decode: %v", err)
	}
	if len(recs) != 3 {
		t.Errorf("index holds %d records, want 3", len(recs))
	}
}

// TestSecondRunIsQuiet: a sync of an unchanged tree copies nothing and
// rewrites nothing — not the playlist, not the index, not CORECFG.DAT. A tool
// that rewrites 30 GB because it cannot tell it already did is not usable.
func TestSecondRunIsQuiet(t *testing.T) {
	src, dst := srcTree(t), t.TempDir()
	o := opts(src, dst)
	syncOnce(t, o)

	before := snapshot(t, dst)
	p2, rep2 := syncOnce(t, o)

	if rep2.Copied != 0 || rep2.Skipped != 3 {
		t.Errorf("second run: copied %d, skipped %d; want 0/3", rep2.Copied, rep2.Skipped)
	}
	if len(rep2.Written) != 0 {
		t.Errorf("second run wrote %v; want nothing", rep2.Written)
	}
	if rep2.IndexWritten || !p2.Index.Unchanged {
		t.Error("the index was rewritten although its bytes are identical")
	}
	if rep2.ArtWritten != 0 || rep2.ArtSkipped != 1 {
		t.Errorf("art: %d written / %d kept, want 0/1 (the coverless album has none to keep)", rep2.ArtWritten, rep2.ArtSkipped)
	}
	if rep2.PlaylistsWritten != 0 || rep2.PlaylistsUnchanged != 1 {
		t.Errorf("playlists: %d written / %d unchanged, want 0/1", rep2.PlaylistsWritten, rep2.PlaylistsUnchanged)
	}
	if rep2.ConfigCreated || rep2.LogCreated {
		t.Error("a valid CORECFG.DAT / CORELOG.BIN was rewritten; saved settings must survive a sync")
	}
	if after := snapshot(t, dst); !sameSnapshot(before, after) {
		t.Errorf("the device changed on an idempotent run:\nbefore %v\nafter  %v", before, after)
	}
}

// The clock. A sync leaves the host's time in CORECFG.DAT for the device to
// take at its next boot — the iPod cannot be told the time any other way — and
// a dry run writes nothing at all, clock included.
func TestSyncStampsTheClock(t *testing.T) {
	src, dst := srcTree(t), t.TempDir()
	o := opts(src, dst)
	before := time.Now().Add(-time.Second)
	_, rep := syncOnce(t, o)

	if rep.ClockStamped.Before(before) {
		t.Errorf("report says the clock was stamped at %v, before the sync started", rep.ClockStamped)
	}
	b, err := os.ReadFile(filepath.Join(dst, devicefs.ConfigName))
	if err != nil {
		t.Fatal(err)
	}
	newest, ok := devicefs.ConfigFileValid(b)
	if !ok {
		t.Fatal("the config the sync created does not validate")
	}
	found := false
	for i := 0; i < devicefs.ConfigSlots; i++ {
		slot := b[i*devicefs.ConfigSlotBytes : (i+1)*devicefs.ConfigSlotBytes]
		seq, _, valid := devicefs.DecodeConfigSlot(slot)
		if !valid || seq != newest {
			continue
		}
		ts, ok := devicefs.DecodeConfigTime(slot)
		if !ok || ts.HostEpoch == 0 || !ts.Pending() {
			t.Errorf("the newest slot carries no pending stamp: %+v (ok=%v)", ts, ok)
		}
		found = true
	}
	if !found {
		t.Error("no slot holds the newest record")
	}

	// A dry run against a device that has never been synced writes nothing —
	// so there is nothing to stamp, and the report says so.
	dry := t.TempDir()
	od := opts(src, dry)
	od.DryRun = true
	p, repDry := syncOnce(t, od)
	if p.Clock {
		t.Error("a dry run planned a clock stamp")
	}
	if !repDry.ClockStamped.IsZero() {
		t.Error("a dry run stamped the clock")
	}
	if _, err := os.Stat(filepath.Join(dry, devicefs.ConfigName)); !os.IsNotExist(err) {
		t.Error("a dry run created CORECFG.DAT")
	}
}

// TestChangedFileIsCopiedAgain: size and time are the whole skip rule, so
// both halves of it have to work.
func TestChangedFileIsCopiedAgain(t *testing.T) {
	src, dst := srcTree(t), t.TempDir()
	o := opts(src, dst)
	syncOnce(t, o)

	// Same bytes, newer time: the source was re-encoded or re-tagged.
	beta := filepath.Join(src, "Blue - Artist A", "Beta.flac")
	future := time.Now().Add(2 * time.Hour)
	if err := os.Chtimes(beta, future, future); err != nil {
		t.Fatal(err)
	}
	_, rep := syncOnce(t, o)
	if rep.Copied != 1 || rep.Skipped != 2 {
		t.Fatalf("after touching one file: copied %d, skipped %d; want 1/2", rep.Copied, rep.Skipped)
	}

	// And a third run is quiet again: the copy carries the new time.
	_, rep = syncOnce(t, o)
	if rep.Copied != 0 {
		t.Errorf("the touched file copied again on the next run (%d)", rep.Copied)
	}
}

// TestVerifyCatchesWhatTimestampsMiss: a destination file with the same size
// and time but different bytes is invisible to the default rule and is exactly
// what --verify exists for.
func TestVerifyCatchesWhatTimestampsMiss(t *testing.T) {
	src, dst := srcTree(t), t.TempDir()
	o := opts(src, dst)
	syncOnce(t, o)

	victim := filepath.Join(dst, "Music", "Artist A - Blue", "01. Alpha.flac")
	fi, err := os.Stat(victim)
	if err != nil {
		t.Fatal(err)
	}
	b, err := os.ReadFile(victim)
	if err != nil {
		t.Fatal(err)
	}
	b[len(b)-1] ^= 0xFF // same length, different content
	if err := os.WriteFile(victim, b, 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.Chtimes(victim, fi.ModTime(), fi.ModTime()); err != nil {
		t.Fatal(err)
	}

	if _, rep := syncOnce(t, o); rep.Copied != 0 {
		t.Errorf("the default rule copied %d file(s); size and time still match", rep.Copied)
	}
	vo := o
	vo.Verify = true
	if _, rep := syncOnce(t, vo); rep.Copied != 1 {
		t.Fatalf("--verify copied %d file(s), want 1", rep.Copied)
	}
	got, err := os.ReadFile(victim)
	if err != nil {
		t.Fatal(err)
	}
	want, err := os.ReadFile(filepath.Join(src, "Blue - Artist A", "Alpha.flac"))
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got, want) {
		t.Error("--verify did not restore the corrupted file")
	}
}

// TestPlaylistIsRewrittenToDevicePaths: the source names host paths, the
// device only understands its own layout, and a line that maps to nothing is
// dropped with a warning rather than written as a track that is not there.
func TestPlaylistIsRewrittenToDevicePaths(t *testing.T) {
	src, dst := srcTree(t), t.TempDir()
	o := opts(src, dst)
	p, rep := syncOnce(t, o)

	if len(p.Playlists) != 1 {
		t.Fatalf("plan has %d playlists, want 1", len(p.Playlists))
	}
	pl := p.Playlists[0]
	if pl.Name != "mix.m3u8" {
		t.Errorf("device playlist name = %q, want mix.m3u8", pl.Name)
	}
	if len(pl.Dropped) != 1 || !strings.Contains(pl.Dropped[0], "Missing.flac") {
		t.Errorf("dropped = %q, want the one unknown line", pl.Dropped)
	}
	if !anyContains(rep.Warnings, "is not a track in the source tree") {
		t.Errorf("the unknown line produced no warning: %q", rep.Warnings)
	}

	got, err := os.ReadFile(filepath.Join(dst, "Music", "Playlists", "mix.m3u8"))
	if err != nil {
		t.Fatal(err)
	}
	want := "#EXTM3U\n" +
		"/Music/Artist A - Blue/01. Alpha.flac\n" +
		"/Music/Artist B - Red/01. Gamma.flac\n"
	if string(got) != want {
		t.Errorf("device playlist:\n got %q\nwant %q", got, want)
	}
}

// TestPruneNeedsYes: --prune alone lists and refuses. The refusal happens
// before any write, so the user reads the list with the device untouched.
func TestPruneNeedsYes(t *testing.T) {
	src, dst := srcTree(t), t.TempDir()
	o := opts(src, dst)
	syncOnce(t, o)

	orphan := filepath.Join(dst, "Music", "Ghost - Album")
	if err := os.MkdirAll(orphan, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(orphan, "01. Old.flac"), []byte("stale"), 0o644); err != nil {
		t.Fatal(err)
	}
	stray := filepath.Join(dst, "Music", "Artist A - Blue", "99. Removed.flac")
	if err := os.WriteFile(stray, []byte("stale"), 0o644); err != nil {
		t.Fatal(err)
	}

	po := o
	po.Prune = true
	p := planFor(t, po)
	if len(p.Prune) != 2 {
		t.Fatalf("plan lists %d prunable item(s) (%q), want 2", len(p.Prune), p.Prune)
	}
	if p.PruneBytes != 10 {
		t.Errorf("PruneBytes = %d, want 10", p.PruneBytes)
	}

	_, err := Execute(context.Background(), p, po)
	if !errors.Is(err, ErrPruneNeedsYes) {
		t.Fatalf("Execute with --prune and no --yes: err = %v, want ErrPruneNeedsYes", err)
	}
	if _, err := os.Stat(orphan); err != nil {
		t.Errorf("the orphan album was removed without --yes: %v", err)
	}
	if _, err := os.Stat(stray); err != nil {
		t.Errorf("the stray file was removed without --yes: %v", err)
	}

	// A plan with nothing to prune is not blocked by --prune.
	clean := opts(src, t.TempDir())
	clean.Prune = true
	if _, err := Execute(context.Background(), planFor(t, clean), clean); err != nil {
		t.Errorf("--prune with nothing to prune failed: %v", err)
	}
}

// TestPruneWithYesRemovesOrphans is the other half: with both flags, and only
// what the plan listed.
func TestPruneWithYesRemovesOrphans(t *testing.T) {
	src, dst := srcTree(t), t.TempDir()
	o := opts(src, dst)
	syncOnce(t, o)

	orphan := filepath.Join(dst, "Music", "Ghost - Album")
	if err := os.MkdirAll(orphan, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(orphan, "01. Old.flac"), []byte("stale"), 0o644); err != nil {
		t.Fatal(err)
	}
	stalePL := filepath.Join(dst, "Music", "Playlists", "gone.m3u8")
	if err := os.WriteFile(stalePL, []byte("#EXTM3U\n"), 0o644); err != nil {
		t.Fatal(err)
	}

	po := o
	po.Prune, po.Yes = true, true
	_, rep := syncOnce(t, po)
	if rep.Pruned != 2 {
		t.Errorf("pruned %d item(s), want 2", rep.Pruned)
	}
	if _, err := os.Stat(orphan); !os.IsNotExist(err) {
		t.Error("the orphan album survived --prune --yes")
	}
	if _, err := os.Stat(stalePL); !os.IsNotExist(err) {
		t.Error("the stale playlist survived --prune --yes")
	}

	// Everything that belongs is still there.
	want := []string{
		"CORECFG.DAT",
		"CORELOG.BIN",
		"Music/Artist A - Blue/01. Alpha.flac",
		"Music/Artist A - Blue/02. Beta.flac",
		"Music/Artist A - Blue/folder.art",
		"Music/Artist A - Blue/folder.thm",
		"Music/Artist B - Red/01. Gamma.flac",
		"Music/CORELIB.IDX",
		"Music/Playlists/mix.m3u8",
	}
	if got := walkFiles(t, dst); !equalStrings(got, want) {
		t.Errorf("after prune:\n got %q\nwant %q", got, want)
	}
}

// TestPruneKeepsSidecarsUnderNoArt: --no-art means "do not touch the art",
// which includes not deleting it as an unrecognised file.
func TestPruneKeepsSidecarsUnderNoArt(t *testing.T) {
	src, dst := srcTree(t), t.TempDir()
	syncOnce(t, opts(src, dst))

	o := opts(src, dst)
	o.NoArt, o.Prune, o.Yes = true, true, true
	p, rep := syncOnce(t, o)
	if len(p.Art) != 0 {
		t.Errorf("--no-art still planned %d art op(s)", len(p.Art))
	}
	if rep.Pruned != 0 {
		t.Fatalf("--no-art --prune --yes deleted %d item(s) (%q)", rep.Pruned, p.Prune)
	}
	for _, n := range []string{coreart.ArtName, coreart.ThumbName} {
		if _, err := os.Stat(filepath.Join(dst, "Music", "Artist A - Blue", n)); err != nil {
			t.Errorf("%s was removed under --no-art: %v", n, err)
		}
	}
}

// TestArtRefreshRewritesValidSidecars: the default keeps the art that is
// already on the device (so a first Go sync does not churn it); --art-refresh
// is how the user asks for the new renderer's output.
func TestArtRefreshRewritesValidSidecars(t *testing.T) {
	src, dst := srcTree(t), t.TempDir()
	syncOnce(t, opts(src, dst))

	o := opts(src, dst)
	o.ArtRefresh = true
	_, rep := syncOnce(t, o)
	if rep.ArtWritten != 1 {
		t.Errorf("--art-refresh rendered %d album(s), want 1", rep.ArtWritten)
	}
	mustSidecar(t, filepath.Join(dst, "Music", "Artist A - Blue", coreart.ThumbName), coreart.ThumbSize)
}

// TestInvalidSidecarIsRewritten: a truncated folder.thm is what a sync
// interrupted by a pulled cable leaves behind, and the firmware would latch
// the album as "no art" for the session. The next sync has to fix it.
func TestInvalidSidecarIsRewritten(t *testing.T) {
	src, dst := srcTree(t), t.TempDir()
	syncOnce(t, opts(src, dst))

	thumb := filepath.Join(dst, "Music", "Artist A - Blue", coreart.ThumbName)
	if err := os.WriteFile(thumb, []byte("CART\x01\x00"), 0o644); err != nil {
		t.Fatal(err)
	}
	_, rep := syncOnce(t, opts(src, dst))
	if rep.ArtWritten != 1 {
		t.Errorf("a truncated sidecar was not re-rendered (ArtWritten = %d)", rep.ArtWritten)
	}
	mustSidecar(t, thumb, coreart.ThumbSize)
}

// TestSidecarsFollowTheSourcePicture is plan §1 decision 11: embedding a cover
// in the source FLAC must reach the iPod on the next sync without a flag.
//
// The sidecars on the device stay structurally valid when the source picture
// changes — they are the OLD cover — so validity alone would keep them for
// ever. `core fix` and `core art --fetch` deliberately do not preserve the
// source's mtime, and this is the rule that reads it.
func TestSidecarsFollowTheSourcePicture(t *testing.T) {
	src, dst := srcTree(t), t.TempDir()
	syncOnce(t, opts(src, dst))

	// Nothing touched: the art of the album that HAS a cover stays put.
	// (Artist B - Red has no embedded picture at all, so its sidecars are
	// never written and it is "missing" on every run.)
	p := planFor(t, opts(src, dst))
	for _, a := range p.Art {
		if a.Album == "Artist A - Blue" && a.Write {
			t.Fatalf("an untouched tree re-rendered %s: %s", a.Album, a.Reason)
		}
	}

	// The cover in the album's art source is replaced (which is exactly what
	// flac.WritePicture does, mtime and all).
	artSrc := filepath.Join(src, "Blue - Artist A", "Alpha.flac")
	later := time.Now().Add(1 * time.Minute)
	if err := os.Chtimes(artSrc, later, later); err != nil {
		t.Fatal(err)
	}

	p = planFor(t, opts(src, dst))
	var got *ArtOp
	for i := range p.Art {
		if p.Art[i].Album == "Artist A - Blue" {
			got = &p.Art[i]
		}
	}
	if got == nil {
		t.Fatal("no art op for Artist A - Blue")
	}
	if !got.Write {
		t.Errorf("a newer art source did not re-render the sidecars: %s", got.Reason)
	}
	if !strings.Contains(got.Reason, "newer") {
		t.Errorf("reason = %q, want it to say the source is newer", got.Reason)
	}
	// And the reason is the mtime, not a coincidence: the album is only in
	// the list because its source moved forward.
	if got.SrcFLAC != artSrc {
		t.Errorf("art source = %s, want %s", got.SrcFLAC, artSrc)
	}
}

// TestDryRunWritesNothing — not the music, not the playlists, and not
// CORECFG.DAT either.
func TestDryRunWritesNothing(t *testing.T) {
	src, dst := srcTree(t), t.TempDir()
	o := opts(src, dst)
	o.DryRun = true

	p := planFor(t, o)
	rep, err := Execute(context.Background(), p, o)
	if err != nil {
		t.Fatalf("dry run: %v", err)
	}
	if len(rep.Written) != 0 {
		t.Errorf("a dry run wrote %v", rep.Written)
	}
	if got := walkFiles(t, dst); len(got) != 0 {
		t.Errorf("the destination is not empty after a dry run: %q", got)
	}
	// It still has to say what it would have done, byte totals included.
	if rep.Copied != 3 || rep.CopiedBytes != p.CopyBytes || p.CopyBytes == 0 {
		t.Errorf("dry-run report: copied %d (%d bytes), plan says %d bytes", rep.Copied, rep.CopiedBytes, p.CopyBytes)
	}
	if !rep.ConfigCreated || !rep.LogCreated || rep.IndexBytes != p.Index.Size {
		t.Errorf("dry-run report does not describe the device files and index: %+v", rep)
	}
}

// TestNoArtSkipsSidecarsEntirely.
func TestNoArtSkipsSidecarsEntirely(t *testing.T) {
	src, dst := srcTree(t), t.TempDir()
	o := opts(src, dst)
	o.NoArt = true
	p, rep := syncOnce(t, o)

	if len(p.Art) != 0 || rep.ArtWritten != 0 {
		t.Errorf("--no-art planned %d op(s) and wrote %d", len(p.Art), rep.ArtWritten)
	}
	for _, n := range []string{coreart.ArtName, coreart.ThumbName} {
		if _, err := os.Stat(filepath.Join(dst, "Music", "Artist A - Blue", n)); !os.IsNotExist(err) {
			t.Errorf("--no-art wrote %s anyway", n)
		}
	}
	if _, err := os.Stat(filepath.Join(dst, devicefs.MusicDir, devicefs.IndexName)); err != nil {
		t.Errorf("--no-art skipped the index too: %v", err)
	}
}

// TestCaseOnlyDifferenceIsRenamed: the bytes are already on the device under a
// name that differs only in case. Moving beats re-sending 30 MB.
func TestCaseOnlyDifferenceIsRenamed(t *testing.T) {
	src, dst := srcTree(t), t.TempDir()
	o := opts(src, dst)
	syncOnce(t, o)

	dir := filepath.Join(dst, "Music", "Artist A - Blue")
	from, to := filepath.Join(dir, "01. Alpha.flac"), filepath.Join(dir, "01. ALPHA.flac")
	if err := os.Rename(from, to); err != nil {
		t.Fatal(err)
	}
	p, rep := syncOnce(t, o)
	if len(p.Rename) != 1 || rep.Renamed != 1 {
		t.Fatalf("plan renamed %d, report renamed %d; want 1 and 1 (copies: %d)", len(p.Rename), rep.Renamed, rep.Copied)
	}
	if rep.Copied != 0 {
		t.Errorf("the file was re-copied (%d) instead of renamed", rep.Copied)
	}
	if _, err := os.Stat(from); err != nil {
		t.Errorf("the renamed file is not at its device name: %v", err)
	}
	if _, err := os.Stat(to); !os.IsNotExist(err) {
		t.Error("the old spelling is still on the device")
	}
}

// TestCopyFailureLeavesTheIndexAlone. The device keeps an index that still
// matches the files it still has, and the report says what was done.
func TestCopyFailureLeavesTheIndexAlone(t *testing.T) {
	src, dst := srcTree(t), t.TempDir()
	o := opts(src, dst)
	p := planFor(t, o)

	// The user moved a file out of the tree between the scan and the copy.
	if err := os.Remove(filepath.Join(src, "Red - Artist B", "Gamma.flac")); err != nil {
		t.Fatal(err)
	}
	rep, err := Execute(context.Background(), p, o)
	if err == nil {
		t.Fatal("Execute succeeded although a source file disappeared")
	}
	if rep == nil {
		t.Fatal("no report came back with the error; the user cannot tell what was done")
	}
	if rep.Copied != 2 {
		t.Errorf("report says %d copied; the first album's two tracks had already landed", rep.Copied)
	}
	if _, err := os.Stat(filepath.Join(dst, devicefs.MusicDir, devicefs.IndexName)); !os.IsNotExist(err) {
		t.Error("CORELIB.IDX was written although a copy failed")
	}
	for _, w := range rep.Written {
		if filepath.Base(w) == devicefs.IndexName {
			t.Errorf("the write log claims the index was written: %v", rep.Written)
		}
	}
}

// TestRefusesDestinationInsideSource — a sync that copies its own output.
func TestRefusesDestinationInsideSource(t *testing.T) {
	src := srcTree(t)
	inner := filepath.Join(src, "device")
	if err := os.MkdirAll(inner, 0o755); err != nil {
		t.Fatal(err)
	}
	for _, o := range []Options{
		{Src: src, Dst: inner},
		{Src: src, Dst: src},
		{Src: inner, Dst: src},
	} {
		if err := CheckPaths(o); err == nil {
			t.Errorf("CheckPaths(%s -> %s) allowed it", o.Src, o.Dst)
		}
	}
}

// TestRefusesDrvfsMount holds the rule that the app never writes an iPod
// through WSL's view of a Windows drive. The mount table is this host's, so
// the test skips where there is no such mount; devicefs's own mount_test.go
// covers the decision against a fake table.
func TestRefusesDrvfsMount(t *testing.T) {
	var target string
	for _, c := range []string{"/mnt/c", "/mnt/d", "/mnt/e"} {
		if devicefs.RefusesMount(c) != nil {
			target = c
			break
		}
	}
	if target == "" {
		t.Skip("no drvfs/9p mount on this host to refuse")
	}
	err := CheckPaths(Options{Src: t.TempDir(), Dst: filepath.Join(target, "ipod-sync-test")})
	if err == nil {
		t.Fatalf("CheckPaths allowed a sync to %s", target)
	}
	if !strings.Contains(err.Error(), "Windows") {
		t.Errorf("the refusal does not tell the user what to do instead: %v", err)
	}
}

// TestSkippedUppercaseExtensionIsWarnedAndNotCopied.
//
// library skips a ".FLAC" file (glob("*.flac") is case-sensitive, so the
// reference index never held it either). The copy list is built from what the
// scan INDEXED, so the file is not copied — and the warning the scan raised
// reaches the user through the plan, because a track that is silently not on
// the device is the worst outcome of the three.
func TestSkippedUppercaseExtensionIsWarnedAndNotCopied(t *testing.T) {
	src, dst := srcTree(t), t.TempDir()
	writeTrack(t, filepath.Join(src, "Blue - Artist A"), "Delta.FLAC",
		map[string]string{"title": "Delta", "artist": "Artist A", "album": "Blue"}, nil)

	p, rep := syncOnce(t, opts(src, dst))
	if p.Tracks != 3 {
		t.Errorf("plan has %d tracks, want 3 — the .FLAC file must not be in it", p.Tracks)
	}
	if !anyContains(rep.Warnings, "Delta.FLAC") {
		t.Errorf("no warning about the skipped .FLAC file: %q", rep.Warnings)
	}
	for _, f := range walkFiles(t, dst) {
		if strings.Contains(f, "Delta") {
			t.Errorf("the skipped file was copied anyway: %s", f)
		}
	}
	// And the index agrees with the tree: 3 records, 3 flacs.
	b, err := os.ReadFile(filepath.Join(dst, devicefs.MusicDir, devicefs.IndexName))
	if err != nil {
		t.Fatal(err)
	}
	recs, err := cidx.Decode(b)
	if err != nil {
		t.Fatal(err)
	}
	if len(recs) != 3 {
		t.Errorf("index holds %d records, the device holds 3 tracks", len(recs))
	}
}

// TestPlaylistDirOverride: --playlists points somewhere else entirely.
func TestPlaylistDirOverride(t *testing.T) {
	src, dst := srcTree(t), t.TempDir()
	alt := t.TempDir()
	// An absolute line, and one that needs the "album folder + file name"
	// fallback (a path from another machine).
	abs := filepath.Join(src, "Red - Artist B", "Gamma.flac")
	body := "#EXTM3U\n" + abs + "\n" + `C:\Music\Blue - Artist A\Beta.flac` + "\n"
	if err := os.WriteFile(filepath.Join(alt, "Road Trip.m3u"), []byte(body), 0o644); err != nil {
		t.Fatal(err)
	}

	o := opts(src, dst)
	o.Playlists = alt
	p, _ := syncOnce(t, o)
	if len(p.Playlists) != 1 {
		t.Fatalf("plan has %d playlists, want 1", len(p.Playlists))
	}
	if p.Playlists[0].Name != "Road Trip.m3u8" {
		t.Errorf("name = %q, want Road Trip.m3u8 (.m3u sources are rewritten as UTF-8 .m3u8)", p.Playlists[0].Name)
	}
	got, err := os.ReadFile(filepath.Join(dst, "Music", "Playlists", "Road Trip.m3u8"))
	if err != nil {
		t.Fatal(err)
	}
	want := "#EXTM3U\n/Music/Artist B - Red/01. Gamma.flac\n/Music/Artist A - Blue/02. Beta.flac\n"
	if string(got) != want {
		t.Errorf("playlist:\n got %q\nwant %q", got, want)
	}
	// The source playlist folder is untouched by the default lookup.
	if _, err := os.Stat(filepath.Join(dst, "Music", "Playlists", "mix.m3u8")); !os.IsNotExist(err) {
		t.Error("--playlists did not replace the default folder")
	}
}

// TestConfigAndLogAreNotResetWhenValid: CORECFG.DAT holds the user's saved
// settings. A sync that reset the volume and the resume point every time would
// be worse than no sync.
func TestConfigAndLogAreNotResetWhenValid(t *testing.T) {
	src, dst := srcTree(t), t.TempDir()
	syncOnce(t, opts(src, dst))

	cfg := filepath.Join(dst, devicefs.ConfigName)
	// Pretend the device saved settings: slot 1 at a later seq.
	b, err := os.ReadFile(cfg)
	if err != nil {
		t.Fatal(err)
	}
	slot := devicefs.EncodeConfigSlot(devicefs.Settings{Volume: 42, BacklightBright: 32, Clicker: 1}, 7)
	copy(b[devicefs.ConfigSlotBytes:], slot[:])
	if err := os.WriteFile(cfg, b, 0o644); err != nil {
		t.Fatal(err)
	}

	syncOnce(t, opts(src, dst))
	after, err := os.ReadFile(cfg)
	if err != nil {
		t.Fatal(err)
	}
	if len(after) != len(b) {
		t.Fatalf("CORECFG.DAT is now %d bytes, was %d", len(after), len(b))
	}
	// The device's own slot is untouched...
	if !bytes.Equal(b[devicefs.ConfigSlotBytes:], after[devicefs.ConfigSlotBytes:]) {
		t.Error("the device's saved slot was rewritten; its settings are gone")
	}
	seq, s, ok := devicefs.DecodeConfigSlot(after[devicefs.ConfigSlotBytes:])
	if !ok || seq != 7 || s.Volume != 42 {
		t.Errorf("saved slot came back as seq %d volume %d (ok=%v)", seq, s.Volume, ok)
	}
	// ...and the clock stamp the sync wrote into the OTHER slot carries those
	// same settings forward, so whichever slot the device loads it still has
	// the user's volume.
	seq, s, ok = devicefs.DecodeConfigSlot(after[:devicefs.ConfigSlotBytes])
	if !ok || seq != 8 || s.Volume != 42 {
		t.Errorf("the stamped slot came back as seq %d volume %d (ok=%v); want 8/42",
			seq, s.Volume, ok)
	}
	if ts, ok := devicefs.DecodeConfigTime(after[:devicefs.ConfigSlotBytes]); !ok ||
		ts.HostEpoch == 0 || !ts.Pending() {
		t.Errorf("the sync did not leave a pending clock stamp: %+v (ok=%v)", ts, ok)
	}
}

// TestContextCancellationStopsBeforeTheIndex.
func TestContextCancellationStopsBeforeTheIndex(t *testing.T) {
	src, dst := srcTree(t), t.TempDir()
	o := opts(src, dst)
	p := planFor(t, o)

	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	rep, err := Execute(ctx, p, o)
	if !errors.Is(err, context.Canceled) {
		t.Fatalf("err = %v, want context.Canceled", err)
	}
	if rep != nil && len(rep.Written) != 0 {
		t.Errorf("a cancelled sync wrote %v", rep.Written)
	}
	if _, err := os.Stat(filepath.Join(dst, devicefs.MusicDir, devicefs.IndexName)); !os.IsNotExist(err) {
		t.Error("a cancelled sync wrote the index")
	}
}

// TestGenreMapReachesTheIndex proves ScanSource wires --genre-map through,
// since the plan's index bytes are the only place it shows.
func TestGenreMapReachesTheIndex(t *testing.T) {
	src, dst := srcTree(t), t.TempDir()
	mapPath := filepath.Join(t.TempDir(), "genres.json")
	if err := os.WriteFile(mapPath, []byte(`{"Artist A": "Shoegaze"}`), 0o644); err != nil {
		t.Fatal(err)
	}
	o := opts(src, dst)
	o.GenreMap = mapPath
	p := planFor(t, o)
	recs, err := cidx.Decode(p.Index.Bytes)
	if err != nil {
		t.Fatal(err)
	}
	var seen bool
	for _, r := range recs {
		if r.Artist == "Artist A" {
			seen = true
			if r.Genre != "Shoegaze" {
				t.Errorf("genre = %q, want Shoegaze from the map", r.Genre)
			}
		}
	}
	if !seen {
		t.Fatal("no record for Artist A")
	}
}

// --- helpers ----------------------------------------------------------------

func mustSidecar(t *testing.T, path string, dim int) {
	t.Helper()
	b, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read %s: %v", path, err)
	}
	if !coreart.Valid(b, dim) {
		t.Errorf("%s is not a %dx%d CoreArt the firmware would load (%d bytes)", path, dim, dim, len(b))
	}
}

type fileState struct {
	rel  string
	size int64
	mod  time.Time
}

func snapshot(t *testing.T, root string) []fileState {
	t.Helper()
	var out []fileState
	err := filepath.Walk(root, func(p string, fi os.FileInfo, err error) error {
		if err != nil || fi.IsDir() {
			return err
		}
		rel, _ := filepath.Rel(root, p)
		out = append(out, fileState{filepath.ToSlash(rel), fi.Size(), fi.ModTime()})
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].rel < out[j].rel })
	return out
}

func sameSnapshot(a, b []fileState) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i].rel != b[i].rel || a[i].size != b[i].size {
			return false
		}
		// CORECFG.DAT is stamped with the host's clock on every real run
		// (internal/devicefs/clock.go), so its mtime moves by design. Its
		// SIZE must not, and what is inside it is checked by name in
		// TestConfigAndLogAreNotResetWhenValid — a stamp rewrites exactly one
		// slot and copies the other verbatim.
		if a[i].rel == devicefs.ConfigName {
			continue
		}
		if !a[i].mod.Equal(b[i].mod) {
			return false
		}
	}
	return true
}

func equalStrings(a, b []string) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

func anyContains(ss []string, sub string) bool {
	for _, s := range ss {
		if strings.Contains(s, sub) {
			return true
		}
	}
	return false
}

// TestPruneRefusesEmptySource: a --src that scans to nothing must not turn
// --prune --yes into "empty the device".
func TestPruneRefusesEmptySource(t *testing.T) {
	src, dst := srcTree(t), t.TempDir()
	syncOnce(t, opts(src, dst))

	empty := t.TempDir()
	po := opts(empty, dst)
	po.Prune, po.Yes = true, true
	p := planFor(t, po)
	if len(p.Prune) == 0 {
		t.Fatal("an empty source should list every album as prunable")
	}
	_, err := Execute(context.Background(), p, po)
	if !errors.Is(err, ErrPruneEmptySource) {
		t.Fatalf("Execute: err = %v, want ErrPruneEmptySource", err)
	}
	if _, err := os.Stat(filepath.Join(dst, "Music", "Artist A - Blue")); err != nil {
		t.Errorf("an album was removed against an empty source: %v", err)
	}
}

func TestUnderDir(t *testing.T) {
	music := filepath.Join("d", "Music")
	for _, c := range []struct {
		p    string
		want bool
	}{
		{filepath.Join("d", "Music", "X - Y"), true},
		{filepath.Join("d", "music", "X - Y", "01.flac"), true},
		{filepath.Join("d", "Music"), false},
		{filepath.Join("d", "CORECFG.DAT"), false},
		{filepath.Join("d", "Music2", "x"), false},
		{filepath.Join("e", "Music", "x"), false},
	} {
		if got := underDir(c.p, music); got != c.want {
			t.Errorf("underDir(%q) = %v, want %v", c.p, got, c.want)
		}
	}
}
