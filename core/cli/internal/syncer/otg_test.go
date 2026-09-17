// SPDX-License-Identifier: Apache-2.0

package syncer

import (
	"context"
	"fmt"
	"hash/crc32"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/devicefs"
)

// The On-The-Go files are the only ones on the volume the DEVICE writes and
// the host only creates. Three things follow, and all three are how a user
// loses a list they built if they are wrong: they are created exactly once, a
// prune never touches them, and a source playlist can never land on one.

func TestOTGFilesAreCreatedOnce(t *testing.T) {
	src, dst := srcTree(t), t.TempDir()
	o := opts(src, dst)

	p, rep := syncOnce(t, o)
	if !p.OTG || len(p.OTGSlots) != devicefs.OTGPlaylistSlots {
		t.Fatalf("plan: otg %v, %d slot(s) to create; want true / %d",
			p.OTG, len(p.OTGSlots), devicefs.OTGPlaylistSlots)
	}
	if !rep.OTGCreated || rep.OTGSlotsCreated != devicefs.OTGPlaylistSlots {
		t.Fatalf("report: otg %v, %d slots; want true / %d",
			rep.OTGCreated, rep.OTGSlotsCreated, devicefs.OTGPlaylistSlots)
	}
	otg, err := os.ReadFile(filepath.Join(dst, devicefs.OTGName))
	if err != nil {
		t.Fatal(err)
	}
	if seq, ok := devicefs.OTGFileValid(otg); !ok || seq != 1 {
		t.Errorf("COREOTG.DAT is seq %d valid=%v, want 1/true", seq, ok)
	}

	// A second run creates nothing — and, crucially, does not reset the list
	// the device has been keeping in the meantime.
	saved, err := devicefs.EncodeOTGSlot(
		[]devicefs.OTGEntry{{FolderHash: 1, FileHash: 2}}, 44, 3)
	if err != nil {
		t.Fatal(err)
	}
	copy(otg[devicefs.OTGSlotBytes:], saved[:])
	if err := os.WriteFile(filepath.Join(dst, devicefs.OTGName), otg, 0o644); err != nil {
		t.Fatal(err)
	}
	p, rep = syncOnce(t, o)
	if p.OTG || len(p.OTGSlots) != 0 {
		t.Errorf("a second plan wants to create otg=%v slots=%v", p.OTG, p.OTGSlots)
	}
	if rep.OTGCreated || rep.OTGSlotsCreated != 0 {
		t.Errorf("a second sync created otg=%v %d slot(s)", rep.OTGCreated, rep.OTGSlotsCreated)
	}
	again, err := os.ReadFile(filepath.Join(dst, devicefs.OTGName))
	if err != nil {
		t.Fatal(err)
	}
	if string(again) != string(otg) {
		t.Error("the second sync rewrote COREOTG.DAT and lost the device's list")
	}
}

func TestOTGDryRunWritesNothing(t *testing.T) {
	src, dst := srcTree(t), t.TempDir()
	o := opts(src, dst)
	o.DryRun = true

	p := planFor(t, o)
	if !p.OTG || len(p.OTGSlots) != devicefs.OTGPlaylistSlots {
		t.Fatalf("the dry-run plan does not list the On-The-Go files: %v %v",
			p.OTG, p.OTGSlots)
	}
	rep, err := Execute(context.Background(), p, o)
	if err != nil {
		t.Fatal(err)
	}
	if !rep.OTGCreated || rep.OTGSlotsCreated != devicefs.OTGPlaylistSlots {
		t.Errorf("the dry run should REPORT what it would create: %v %d",
			rep.OTGCreated, rep.OTGSlotsCreated)
	}
	if got := walkFiles(t, dst); len(got) != 0 {
		t.Errorf("a dry run wrote %v", got)
	}
}

func TestPruneKeepsOTGSlots(t *testing.T) {
	src, dst := srcTree(t), t.TempDir()
	o := opts(src, dst)
	syncOnce(t, o)

	// Slot 3 holds a list the user saved on the device; slot 1 is still
	// empty. Neither is in any source tree, and neither may be pruned.
	pldir := filepath.Join(dst, devicefs.MusicDir, devicefs.PlaylistDir)
	used := deviceSavedSlot(t, 4, "/Music/Artist A - Blue/01. Alpha.flac")
	slot3 := filepath.Join(pldir, devicefs.OTGSlotName(3))
	if err := os.WriteFile(slot3, used, 0o644); err != nil {
		t.Fatal(err)
	}

	stale := filepath.Join(pldir, "gone.m3u8")
	if err := os.WriteFile(stale, []byte("#EXTM3U\n"), 0o644); err != nil {
		t.Fatal(err)
	}

	po := o
	po.Prune, po.Yes = true, true
	p, rep := syncOnce(t, po)

	for _, victim := range p.Prune {
		if devicefs.OTGSlotIndex(filepath.Base(victim)) > 0 {
			t.Errorf("the plan would prune a device slot: %s", victim)
		}
	}
	if rep.Pruned != 1 {
		t.Errorf("pruned %d item(s), want just the stale playlist", rep.Pruned)
	}
	if _, err := os.Stat(stale); !os.IsNotExist(err) {
		t.Error("the stale playlist survived --prune --yes")
	}
	for n := 1; n <= devicefs.OTGPlaylistSlots; n++ {
		if _, err := os.Stat(filepath.Join(pldir, devicefs.OTGSlotName(n))); err != nil {
			t.Errorf("%s was pruned: %v", devicefs.OTGSlotName(n), err)
		}
	}
	back, err := os.ReadFile(slot3)
	if err != nil {
		t.Fatal(err)
	}
	if string(back) != string(used) {
		t.Error("the saved On-The-Go list was rewritten by a sync")
	}
}

func TestSourcePlaylistOnSlotNameIsRefused(t *testing.T) {
	src, dst := srcTree(t), t.TempDir()
	pl := filepath.Join(src, devicefs.PlaylistDir)
	body := "#EXTM3U\nRed - Artist B/Gamma.flac\n"
	if err := os.WriteFile(filepath.Join(pl, "On-The-Go 2.m3u8"), []byte(body), 0o644); err != nil {
		t.Fatal(err)
	}
	o := opts(src, dst)
	p, rep := syncOnce(t, o)

	for _, op := range p.Playlists {
		if devicefs.OTGSlotIndex(op.Name) > 0 {
			t.Fatalf("a source playlist was planned onto device slot %s", op.Name)
		}
	}
	var warned bool
	for _, w := range rep.Warnings {
		if strings.Contains(w, "On-The-Go 2.m3u8") && strings.Contains(w, "slot") {
			warned = true
		}
	}
	if !warned {
		t.Errorf("no warning about the refused playlist:\n%s",
			strings.Join(rep.Warnings, "\n"))
	}

	// The slot on the device is the empty one the sync created, not the
	// user's tracks.
	info, err := devicefs.OTGSlotFileState(filepath.Join(dst, devicefs.MusicDir,
		devicefs.PlaylistDir, devicefs.OTGSlotName(2)))
	if err != nil {
		t.Fatal(err)
	}
	if info.State != devicefs.OTGSlotEmpty {
		t.Errorf("slot 2 is %q, want %q", info.State, devicefs.OTGSlotEmpty)
	}
}

// deviceSavedSlot is what the FIRMWARE leaves in a slot after a Save: the
// header, the entry lines, the trailer carrying the same gen, and newlines to
// the last byte. The host never writes one — it only reads them — so the
// bytes are built here rather than borrowed from devicefs, which is also
// what makes this a real fixture and not a restatement of the code under
// test. core/library/otg_slot.h is the format.
func deviceSavedSlot(t *testing.T, gen int, entries ...string) []byte {
	t.Helper()
	var body strings.Builder
	for _, e := range entries {
		body.WriteString(e)
		body.WriteByte('\n')
	}
	out := "#EXTM3U\n" +
		fmt.Sprintf("#CORE-OTG v1 count=%05d crc=%08X gen=%05d\n",
			len(entries), crc32.ChecksumIEEE([]byte(body.String())), gen) +
		body.String() +
		fmt.Sprintf("#CORE-OTG-END gen=%05d\n", gen)
	b := make([]byte, devicefs.OTGSlotFileBytes)
	if copy(b, out) != len(out) {
		t.Fatalf("the fixture does not fit in %d bytes", devicefs.OTGSlotFileBytes)
	}
	for i := len(out); i < len(b); i++ {
		b[i] = '\n'
	}
	return b
}
