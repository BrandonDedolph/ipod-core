// SPDX-License-Identifier: Apache-2.0

package syncer

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/cidx"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/devicefs"
)

// TestMCPlanAgainstEmptyDevice runs the planner over the real library and
// checks it against the one thing that can contradict it: the index the
// reference tools built from the same tree (CORE_PARITY_IDX, the 928-record
// oracle). A synthetic tree cannot catch a rule that only fires on a folder
// with a curly apostrophe, a two-disc album or a 70-byte title; the real one
// has all three.
//
// Skips when CORE_PARITY_SRC is unset, like every other parity test here.
func TestMCPlanAgainstEmptyDevice(t *testing.T) {
	src := os.Getenv("CORE_PARITY_SRC")
	if src == "" {
		t.Skip("set CORE_PARITY_SRC to the MC library to run the real-tree plan")
	}
	if fi, err := os.Stat(src); err != nil || !fi.IsDir() {
		t.Skipf("CORE_PARITY_SRC=%s is not a folder", src)
	}

	o := opts(src, t.TempDir())
	scan, err := ScanSource(o)
	if err != nil {
		t.Fatalf("ScanSource(%s): %v", src, err)
	}
	p, err := MakePlan(o, scan)
	if err != nil {
		t.Fatalf("MakePlan: %v", err)
	}

	t.Logf("MC plan: %d albums, %d tracks, %d to copy (%d bytes), %d to skip, %d art op(s), %d playlist(s)",
		p.Albums, p.Tracks, len(p.Copy), p.CopyBytes, len(p.Skip), len(p.Art), len(p.Playlists))
	t.Logf("MC index: %d records, %d bytes -> %s", p.Index.Records, p.Index.Size, p.Index.Dst)
	if n := len(p.Warnings); n > 0 {
		t.Logf("%d warning(s); first few:", n)
		for i, w := range p.Warnings {
			if i == 5 {
				break
			}
			t.Logf("  %s", w)
		}
	}

	// The plan and the scan are the same set of files, or the index describes
	// a device the sync did not build.
	if p.Albums != len(scan.Albums) || p.Tracks != scan.SongCount() {
		t.Errorf("plan %d albums / %d tracks, scan %d / %d", p.Albums, p.Tracks, len(scan.Albums), scan.SongCount())
	}
	if p.Index.Records != p.Tracks {
		t.Errorf("index carries %d records for %d planned tracks", p.Index.Records, p.Tracks)
	}
	if p.Albums == 0 || p.Tracks == 0 {
		t.Fatal("the real library planned nothing")
	}

	// An empty destination: everything is a copy, nothing is a skip, nothing
	// is prunable, and both device files have to be created.
	if len(p.Copy) != p.Tracks || len(p.Skip) != 0 {
		t.Errorf("against an empty device: %d copies / %d skips for %d tracks", len(p.Copy), len(p.Skip), p.Tracks)
	}
	if len(p.Prune) != 0 {
		t.Errorf("an empty device has %d prunable item(s)", len(p.Prune))
	}
	if !p.Config || !p.Log {
		t.Errorf("an empty device: config %v, log %v; both must be created", p.Config, p.Log)
	}
	if p.CopyBytes <= 0 {
		t.Error("the plan copies zero bytes")
	}

	// Every destination path is unique, and none of them is longer than the
	// firmware's playlist path cap would allow to be referenced.
	seen := map[string]string{}
	for _, op := range p.Copy {
		if prev, dup := seen[op.Dst]; dup {
			t.Fatalf("two source files both land on %s (%s and %s)", op.Dst, prev, op.Src)
		}
		seen[op.Dst] = op.Src
		if n := len(devicefs.PlaylistEntry(op.Album, filepath.Base(op.Dst))); n > devicefs.PathMax {
			t.Errorf("%s is %d bytes as a playlist entry, over M3U_PATH_MAX (%d)", op.Device, n, devicefs.PathMax)
		}
	}

	// The oracle: the index the Python tools built from this tree.
	oracle := os.Getenv("CORE_PARITY_IDX")
	if oracle == "" {
		t.Log("CORE_PARITY_IDX is unset; skipping the record-count cross-check")
		return
	}
	b, err := os.ReadFile(oracle)
	if err != nil {
		t.Skipf("CORE_PARITY_IDX=%s: %v", oracle, err)
	}
	recs, err := cidx.Decode(b)
	if err != nil {
		t.Fatalf("the oracle index does not decode: %v", err)
	}
	t.Logf("oracle %s: %d records", oracle, len(recs))
	if len(recs) != p.Index.Records {
		t.Errorf("the plan indexes %d tracks, the oracle has %d — the tree changed, or a scan rule drifted",
			p.Index.Records, len(recs))
	}
}
