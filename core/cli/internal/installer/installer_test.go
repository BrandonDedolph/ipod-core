package installer

import (
	"bytes"
	"context"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/devicefs"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/disk"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flasher"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/fwpart"
)

// Nothing here opens a disk: the device is a literal, the flasher is a
// stub that records what it was told, and the "FAT volume" is a temp
// directory that the real devicefs writes real files into.

func appleInstalled() fwpart.Installed {
	return fwpart.Installed{
		Kind:        fwpart.Other,
		Description: "Apple firmware (7.6 MB, entry 0x736000)",
	}
}

func coreInstalled() fwpart.Installed {
	return fwpart.Installed{
		Kind: fwpart.Core, Version: "v0.1.3", BuildID: "v0.1.3",
		Description: "Core v0.1.3 (build v0.1.3)",
	}
}

func pod() disk.IPod {
	return disk.IPod{
		Disk: disk.Disk{
			Path: `\\.\PhysicalDrive9`, Serial: "TESTSERIAL",
			Volumes: []string{"D:"},
		},
		Model: disk.TestedModel, Tested: true,
	}
}

// harness is one install's worth of fakes: a device whose
// classification changes after the write (as a real one's does), a
// recording flasher, and a directory standing in for the volume.
type harness struct {
	before, after fwpart.Installed
	inspects      int
	flashRes      *flasher.Result
	flashErr      error
	called        bool
	opts          flasher.Options
	deps          flasher.Deps
	volume        string
	out           bytes.Buffer
	resolved      string
	resolveCalls  int
}

func newHarness(t *testing.T, before fwpart.Installed) *harness {
	t.Helper()
	return &harness{
		before:   before,
		after:    coreInstalled(),
		flashRes: &flasher.Result{Verified: true, Device: `\\.\PhysicalDrive9`, Installed: before},
		volume:   t.TempDir(),
	}
}

func (h *harness) build() Deps {
	return Deps{
		Inspect: func(context.Context) (Device, error) {
			h.inspects++
			inst := h.before
			if h.inspects > 1 {
				inst = h.after
			}
			return Device{Pod: pod(), Installed: inst}, nil
		},
		ResolveImage: func(context.Context) (string, error) {
			h.resolveCalls++
			return h.resolved, nil
		},
		Flash: func(_ context.Context, o flasher.Options, d flasher.Deps) (*flasher.Result, error) {
			h.called, h.opts, h.deps = true, o, d
			return h.flashRes, h.flashErr
		},
		Volume:    func(disk.IPod) string { return h.volume },
		Out:       &h.out,
		GOOS:      "windows",
		ChildArgs: func(image string) []string { return []string{"install", image} },
	}
}

// The whole sequence over the stub device: classify, flash with the
// install flag, then create on the volume what the firmware cannot
// create for itself.
func TestInstallCreatesTheDeviceFilesAndMusicFolder(t *testing.T) {
	h := newHarness(t, appleInstalled())
	backup := filepath.Join(t.TempDir(), "apple-TESTSERIAL-2026-09-15.bin")
	h.flashRes.BackupPath = backup

	res, err := Install(context.Background(), Options{Image: `C:\img\core.ipod`, Yes: true}, h.build())
	if err != nil {
		t.Fatalf("Install: %v", err)
	}
	if !h.called {
		t.Fatal("the sequence never reached the flash step")
	}
	if !h.opts.Install {
		t.Error("the flasher was not told this is an install")
	}
	if h.opts.Image != `C:\img\core.ipod` || !h.opts.Yes || h.opts.DryRun {
		t.Errorf("flasher options: %+v", h.opts)
	}
	if h.resolveCalls != 0 {
		t.Error("an image was named on the command line and a release was resolved anyway")
	}

	// The three things the firmware cannot make for itself.
	for _, name := range []string{devicefs.ConfigName, devicefs.LogName} {
		st, err := os.Stat(filepath.Join(h.volume, name))
		if err != nil {
			t.Errorf("%s was not created: %v", name, err)
			continue
		}
		if st.Size() == 0 {
			t.Errorf("%s is empty", name)
		}
	}
	music, err := os.Stat(filepath.Join(h.volume, devicefs.MusicDir))
	if err != nil || !music.IsDir() {
		t.Errorf("%s\\ was not created: %v", devicefs.MusicDir, err)
	}
	head, err := os.ReadFile(filepath.Join(h.volume, devicefs.ConfigName))
	if err != nil {
		t.Fatal(err)
	}
	if _, ok := devicefs.ConfigFileValid(head); !ok {
		t.Error("the CORECFG.DAT written does not validate")
	}

	if res.Before.Kind != fwpart.Other || res.After.Kind != fwpart.Core {
		t.Errorf("before/after = %v / %v", res.Before.Kind, res.After.Kind)
	}
	if res.Installed().Kind != fwpart.Core {
		t.Errorf("Installed() = %v, want the re-read", res.Installed().Kind)
	}
	if !res.ConfigCreated || !res.LogCreated || !res.MusicCreated {
		t.Errorf("the result does not record what it created: %+v", res)
	}
	if res.Volume != h.volume {
		t.Errorf("Result.Volume = %q", res.Volume)
	}

	out := h.out.String()
	for _, want := range []string{
		"installed: Apple firmware (7.6 MB, entry 0x736000)",
		devicefs.ConfigName, devicefs.LogName, "created",
		"firmware:  Core v0.1.3 (build v0.1.3) (was: Apple firmware (7.6 MB, entry 0x736000))",
		backup,
		"core flash --from-backup " + backup,
		"the only copy there is",
		"Select+Play",
	} {
		if !strings.Contains(out, want) {
			t.Errorf("the narration does not mention %q:\n%s", want, out)
		}
	}
}

// The refusal that makes `install` a different thing from `flash`: a
// device that is already ours is an UPDATE, and saying so is more
// useful than writing the image.
func TestInstallRefusesADeviceAlreadyRunningCore(t *testing.T) {
	for _, inst := range []fwpart.Installed{
		coreInstalled(),
		{Kind: fwpart.CoreOld, Description: "Core before v0.1.3 (367608 bytes, entry 0x0, no version marker)"},
	} {
		t.Run(string(inst.Kind), func(t *testing.T) {
			h := newHarness(t, inst)
			_, err := Install(context.Background(), Options{Image: "core.ipod", Yes: true}, h.build())
			if err == nil {
				t.Fatal("install onto a device already running Core succeeded")
			}
			if !errors.Is(err, ErrAlreadyCore) {
				t.Errorf("the refusal is not ErrAlreadyCore: %v", err)
			}
			msg := err.Error()
			if !strings.Contains(msg, "already") || !strings.Contains(msg, "core update") {
				t.Errorf("the refusal does not say what to run instead:\n%s", msg)
			}
			if h.called {
				t.Error("the refusal still reached the flasher")
			}
		})
	}
}

func TestInstallForceWritesOverCoreAnyway(t *testing.T) {
	h := newHarness(t, coreInstalled())
	if _, err := Install(context.Background(), Options{Image: "core.ipod", Yes: true, Force: true}, h.build()); err != nil {
		t.Fatalf("Install --force: %v", err)
	}
	if !h.called {
		t.Fatal("--force did not reach the flasher")
	}
	if !strings.Contains(h.out.String(), "--force") {
		t.Errorf("the narration does not say it is overwriting a Core image:\n%s", h.out.String())
	}
}

// --dry-run writes nothing anywhere — not the device (the flasher's
// job) and not the volume (this package's).
func TestInstallDryRunTouchesTheVolumeNot(t *testing.T) {
	h := newHarness(t, appleInstalled())
	h.flashRes = &flasher.Result{DryRun: true}

	if _, err := Install(context.Background(), Options{Image: "core.ipod", DryRun: true}, h.build()); err != nil {
		t.Fatalf("Install --dry-run: %v", err)
	}
	if !h.opts.DryRun {
		t.Error("DryRun did not reach the flasher")
	}
	ents, err := os.ReadDir(h.volume)
	if err != nil {
		t.Fatal(err)
	}
	if len(ents) != 0 {
		t.Errorf("a dry run created %d entries on the volume", len(ents))
	}
	if !strings.Contains(h.out.String(), "--dry-run") {
		t.Errorf("the narration does not say nothing was written:\n%s", h.out.String())
	}
	if h.inspects != 1 {
		t.Errorf("a dry run read the device %d times", h.inspects)
	}
}

// A second install over the same volume leaves the files alone: the
// config is the user's settings and the log is their history, and
// neither is something an install may reset.
func TestInstallKeepsExistingDeviceFiles(t *testing.T) {
	h := newHarness(t, appleInstalled())
	if _, err := devicefs.EnsureConfig(h.volume); err != nil {
		t.Fatal(err)
	}
	cfg := filepath.Join(h.volume, devicefs.ConfigName)
	before, err := os.ReadFile(cfg)
	if err != nil {
		t.Fatal(err)
	}
	res, err := Install(context.Background(), Options{Image: "core.ipod", Yes: true}, h.build())
	if err != nil {
		t.Fatalf("Install: %v", err)
	}
	after, err := os.ReadFile(cfg)
	if err != nil {
		t.Fatal(err)
	}
	// The install stamps the clock, which rewrites the OTHER slot and nothing
	// else: the record the device saved — slot 0 here — must come back byte
	// for byte, and the file must not have changed size.
	if len(after) != len(before) {
		t.Fatalf("CORECFG.DAT is now %d bytes, was %d", len(after), len(before))
	}
	if string(before[:devicefs.ConfigSlotBytes]) != string(after[:devicefs.ConfigSlotBytes]) {
		t.Error("the install rewrote the valid record in CORECFG.DAT")
	}
	if ts, ok := devicefs.DecodeConfigTime(after[devicefs.ConfigSlotBytes:]); !ok ||
		ts.HostEpoch == 0 || !ts.Pending() {
		t.Error("the install did not leave a pending clock stamp in the other slot")
	}
	if res.ConfigCreated {
		t.Error("the result claims it created a file that was already there")
	}
	if !strings.Contains(h.out.String(), "already there and valid") {
		t.Errorf("the narration does not say the file was kept:\n%s", h.out.String())
	}
}

// With no image the caller's resolver runs — once, before the write —
// and the path it produced is what an elevated child is told to write.
// A child handed a release tag instead would make a network request as
// Administrator.
func TestInstallResolvesTheImageAndHandsTheChildThePath(t *testing.T) {
	h := newHarness(t, appleInstalled())
	h.resolved = `C:\cache\core-v0.1.3.ipod`

	res, err := Install(context.Background(), Options{Yes: true}, h.build())
	if err != nil {
		t.Fatalf("Install: %v", err)
	}
	if h.resolveCalls != 1 {
		t.Errorf("the image was resolved %d times", h.resolveCalls)
	}
	if h.opts.Image != h.resolved || res.Image != h.resolved {
		t.Errorf("flasher got %q, result says %q", h.opts.Image, res.Image)
	}
	want := []string{"install", h.resolved}
	if got := h.deps.ChildArgs; len(got) != 2 || got[0] != want[0] || got[1] != want[1] {
		t.Errorf("the elevated child would run %q, want %q", got, want)
	}
}

// A relaunch means the elevated child ran the whole sequence, volume
// step included. Doing it again here would be a second process writing
// the same files, and the "was:" line would compare the device against
// itself.
func TestInstallAfterARelaunchDoesNotRedoTheVolumeStep(t *testing.T) {
	h := newHarness(t, appleInstalled())
	h.flashRes = &flasher.Result{Relaunched: true, ChildExit: 0}

	if _, err := Install(context.Background(), Options{Image: "core.ipod"}, h.build()); err != nil {
		t.Fatalf("Install: %v", err)
	}
	ents, err := os.ReadDir(h.volume)
	if err != nil {
		t.Fatal(err)
	}
	if len(ents) != 0 {
		t.Errorf("the parent recreated %d volume entries after the child had done it", len(ents))
	}
}

// An unmounted volume is not a failed install: the write is done and
// the device files can still be made. It has to say so, with the two
// commands that finish the job.
func TestInstallWithNoMountedVolumeSaysWhatToRun(t *testing.T) {
	h := newHarness(t, appleInstalled())
	h.volume = ""

	res, err := Install(context.Background(), Options{Image: "core.ipod", Yes: true}, h.build())
	if err != nil {
		t.Fatalf("Install: %v", err)
	}
	if res.Volume != "" {
		t.Errorf("Result.Volume = %q with nothing mounted", res.Volume)
	}
	out := h.out.String()
	if !strings.Contains(out, "not mounted") || !strings.Contains(out, "core sync") {
		t.Errorf("the narration does not say how to finish:\n%s", out)
	}
}

func TestInstallWithoutInspectRefuses(t *testing.T) {
	if _, err := Install(context.Background(), Options{Image: "x"}, Deps{}); err == nil {
		t.Fatal("Install with no way to read the device succeeded")
	}
}

// The flasher's error comes back with the Result, because a caller has
// to be able to tell "the elevated child failed with exit 2" from "this
// process failed".
func TestInstallReturnsTheResultWithTheFlashError(t *testing.T) {
	h := newHarness(t, appleInstalled())
	h.flashRes = &flasher.Result{Relaunched: true, ChildExit: 2}
	h.flashErr = errors.New("the elevated child failed")

	res, err := Install(context.Background(), Options{Image: "core.ipod"}, h.build())
	if err == nil {
		t.Fatal("a failed flash reported success")
	}
	if res == nil || res.Flash == nil || res.Flash.ChildExit != 2 {
		t.Fatalf("the result does not carry the child's exit code: %+v", res)
	}
}
