package cli

import (
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flac"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/syncer"
)

// syncFixture is a two-album source tree with one playlist. The point of the
// tests in this file is the COMMAND — flags, exit codes, what lands on stdout —
// not the sync rules, which internal/syncer holds.
func syncFixture(t *testing.T) string {
	t.Helper()
	src := t.TempDir()
	write := func(dir, name, title string) {
		if err := os.MkdirAll(dir, 0o755); err != nil {
			t.Fatal(err)
		}
		raw := flac.BuildFile(
			flac.StreamInfo{SampleRate: 44100, Channels: 2, BitsPerSample: 16, TotalSamples: 44100},
			map[string]string{"title": title, "album": "Blue", "artist": "Artist A"}, nil)
		if err := os.WriteFile(filepath.Join(dir, name), raw, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	write(filepath.Join(src, "Blue - Artist A"), "Alpha.flac", "Alpha")
	write(filepath.Join(src, "Red - Artist B"), "Gamma.flac", "Gamma")

	pl := filepath.Join(src, "Playlists")
	if err := os.MkdirAll(pl, 0o755); err != nil {
		t.Fatal(err)
	}
	body := "#EXTM3U\n../Blue - Artist A/Alpha.flac\nGone - Artist Q/Nope.flac\n"
	if err := os.WriteFile(filepath.Join(pl, "mix.m3u8"), []byte(body), 0o644); err != nil {
		t.Fatal(err)
	}
	return src
}

// TestSyncDryRunWritesNothing: the flag has to mean it. A dry run that
// creates CORECFG.DAT "because that is harmless" is a dry run nobody can
// trust with the rest.
func TestSyncDryRunWritesNothing(t *testing.T) {
	src, dst := syncFixture(t), t.TempDir()
	out, _, err := runCore(t, "sync", "--src", src, "--dst", dst, "--dry-run", "--genre-map", "")
	if err != nil {
		t.Fatalf("sync --dry-run: %v", err)
	}
	for _, want := range []string{"2 album(s), 2 track(s)", "CORECFG.DAT", "CORELIB.IDX", "dry run"} {
		if !strings.Contains(out, want) {
			t.Errorf("dry-run output does not mention %q:\n%s", want, out)
		}
	}
	ents, err := os.ReadDir(dst)
	if err != nil {
		t.Fatal(err)
	}
	if len(ents) != 0 {
		t.Errorf("the destination is not empty after --dry-run: %v", ents)
	}
}

// TestSyncRunsAndIsIdempotent walks the command the way the user does: once to
// fill an empty device, once more to prove it does nothing the second time.
func TestSyncRunsAndIsIdempotent(t *testing.T) {
	src, dst := syncFixture(t), t.TempDir()
	out, errOut, err := runCore(t, "sync", "--src", src, "--dst", dst, "--genre-map", "")
	if err != nil {
		t.Fatalf("sync: %v\n%s", err, errOut)
	}
	if !strings.Contains(out, "copied 1") {
		t.Errorf("no per-album line in the output:\n%s", out)
	}
	if !strings.Contains(errOut, "warning:") || !strings.Contains(errOut, "Nope.flac") {
		t.Errorf("the dropped playlist line did not reach stderr:\n%s", errOut)
	}
	for _, rel := range []string{
		"CORECFG.DAT", "CORELOG.BIN",
		"Music/CORELIB.IDX",
		"Music/Artist A - Blue/01. Alpha.flac",
		"Music/Playlists/mix.m3u8",
	} {
		if _, err := os.Stat(filepath.Join(dst, filepath.FromSlash(rel))); err != nil {
			t.Errorf("%s: %v", rel, err)
		}
	}

	out, _, err = runCore(t, "sync", "--src", src, "--dst", dst, "--genre-map", "")
	if err != nil {
		t.Fatalf("second sync: %v", err)
	}
	if !strings.Contains(out, "copied 0 (0 B)") {
		t.Errorf("the second run did not report zero copies:\n%s", out)
	}
	if !strings.Contains(out, "CORELIB.IDX: unchanged") {
		t.Errorf("the second run rewrote the index:\n%s", out)
	}
}

// TestSyncPruneWithoutYesExitsNonZero: the list, then a non-zero exit, and
// nothing deleted.
func TestSyncPruneWithoutYesExitsNonZero(t *testing.T) {
	src, dst := syncFixture(t), t.TempDir()
	if _, _, err := runCore(t, "sync", "--src", src, "--dst", dst, "--genre-map", ""); err != nil {
		t.Fatal(err)
	}
	orphan := filepath.Join(dst, "Music", "Ghost - Album")
	if err := os.MkdirAll(orphan, 0o755); err != nil {
		t.Fatal(err)
	}

	_, errOut, err := runCore(t, "sync", "--src", src, "--dst", dst, "--genre-map", "", "--prune")
	if err == nil {
		t.Fatal("sync --prune exited 0 with something to prune; the user would think it had pruned")
	}
	if !errors.Is(err, syncer.ErrPruneNeedsYes) {
		t.Errorf("error = %v, want ErrPruneNeedsYes", err)
	}
	if !strings.Contains(errOut, "Ghost - Album") || !strings.Contains(errOut, "--prune --yes") {
		t.Errorf("stderr does not list the orphan and the flag to add:\n%s", errOut)
	}
	if _, err := os.Stat(orphan); err != nil {
		t.Errorf("the orphan was removed without --yes: %v", err)
	}

	if _, _, err := runCore(t, "sync", "--src", src, "--dst", dst, "--genre-map", "", "--prune", "--yes"); err != nil {
		t.Fatalf("sync --prune --yes: %v", err)
	}
	if _, err := os.Stat(orphan); !os.IsNotExist(err) {
		t.Error("the orphan survived --prune --yes")
	}
}

// TestSyncJSONIsMachineReadable — the UI slice (S10) reads this, and so does
// anyone scripting a nightly sync.
func TestSyncJSONIsMachineReadable(t *testing.T) {
	src, dst := syncFixture(t), t.TempDir()

	out, _, err := runCore(t, "sync", "--src", src, "--dst", dst, "--genre-map", "", "--dry-run", "--json")
	if err != nil {
		t.Fatal(err)
	}
	var plan syncer.Plan
	if err := json.Unmarshal([]byte(out), &plan); err != nil {
		t.Fatalf("--dry-run --json is not JSON: %v\n%s", err, out)
	}
	if plan.Tracks != 2 || len(plan.Copy) != 2 {
		t.Errorf("plan JSON: %d tracks, %d copies", plan.Tracks, len(plan.Copy))
	}

	out, _, err = runCore(t, "sync", "--src", src, "--dst", dst, "--genre-map", "", "--json")
	if err != nil {
		t.Fatal(err)
	}
	var rep syncer.Report
	if err := json.Unmarshal([]byte(out), &rep); err != nil {
		t.Fatalf("--json report is not JSON: %v\n%s", err, out)
	}
	if rep.Copied != 2 || !rep.IndexWritten {
		t.Errorf("report JSON: copied %d, index written %v", rep.Copied, rep.IndexWritten)
	}
}

// TestSyncRequiresBothPaths.
func TestSyncRequiresBothPaths(t *testing.T) {
	src := syncFixture(t)
	for _, args := range [][]string{
		{"sync"},
		{"sync", "--src", src},
		{"sync", "--dst", t.TempDir()},
	} {
		if _, _, err := runCore(t, args...); err == nil {
			t.Errorf("%v exited 0 without both paths", args)
		}
	}
}

// TestEjectRejectsNonsense: eject is the command the user runs immediately
// before pulling a cable, so "it printed nothing and exited 0" is the one
// outcome it must never have.
func TestEjectRejectsNonsense(t *testing.T) {
	_, _, err := runCore(t, "eject", filepath.Join(t.TempDir(), "not-a-volume"))
	if err == nil {
		t.Fatal("eject of a path that is not a volume exited 0")
	}
	if _, _, err := runCore(t, "eject"); err == nil {
		t.Fatal("eject with no argument exited 0")
	}
}
