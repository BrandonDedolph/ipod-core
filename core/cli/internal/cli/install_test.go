package cli

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/devicefs"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/disk"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flasher"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/fwpart"
	"github.com/spf13/cobra"
)

// Nothing here opens a disk or makes a network request: the device is
// the stub behind installDeviceFunc, the flasher is the stub behind
// flashImage, and the "FAT volume" is a temp directory that the real
// devicefs writes real files into.

// stubInstallDevice replaces the device read with a fixed answer and
// returns the temp directory standing in for the mounted volume.
func stubInstallDevice(t *testing.T, inst fwpart.Installed) string {
	t.Helper()
	volume := t.TempDir()
	prev := installDeviceFunc
	installDeviceFunc = func(*cobra.Command) (installDevice, error) {
		return installDevice{
			Pod: disk.IPod{
				Disk: disk.Disk{
					Path: `\\.\PhysicalDrive9`, Serial: "TESTSERIAL",
					Volumes: []string{"D:"}, MountPoints: []string{volume},
				},
				Model: disk.TestedModel, Tested: true,
			},
			Installed: inst,
		}, nil
	}
	t.Cleanup(func() { installDeviceFunc = prev })
	return volume
}

// captureFlashResult replaces the flasher and hands back the Result the
// test wants, recording what it was called with.
func captureFlashResult(t *testing.T, res *flasher.Result) *flashCall {
	t.Helper()
	got := &flashCall{}
	prev := flashImage
	flashImage = func(ctx context.Context, o flasher.Options, d flasher.Deps) (*flasher.Result, error) {
		got.Called, got.Opts, got.Deps = true, o, d
		return res, nil
	}
	t.Cleanup(func() { flashImage = prev })
	return got
}

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

// The refusal that makes `install` a different command from `flash`: a
// device that is already ours is an UPDATE, and saying so is more
// useful than writing the image.
func TestInstallRefusesADeviceAlreadyRunningCore(t *testing.T) {
	for _, inst := range []fwpart.Installed{
		coreInstalled(),
		{Kind: fwpart.CoreOld, Description: "Core before v0.1.3 (367608 bytes, entry 0x0, no version marker)"},
	} {
		t.Run(string(inst.Kind), func(t *testing.T) {
			stubInstallDevice(t, inst)
			flash := captureFlashResult(t, &flasher.Result{Verified: true})
			path := writeTemp(t, t.TempDir(), "core.ipod", ipodFile(t, plausibleImage(8192)))

			_, _, err := runCore(t, "install", path, "--yes")
			if err == nil {
				t.Fatal("install onto a device already running Core succeeded")
			}
			msg := err.Error()
			if !strings.Contains(msg, "already") || !strings.Contains(msg, "core update") {
				t.Errorf("the refusal does not say what to run instead:\n%s", msg)
			}
			if flash.Called {
				t.Error("the refusal still reached the flasher")
			}
		})
	}
}

func TestInstallForceWritesOverCoreAnyway(t *testing.T) {
	stubInstallDevice(t, coreInstalled())
	flash := captureFlashResult(t, &flasher.Result{Verified: true, Device: `\\.\PhysicalDrive9`})
	path := writeTemp(t, t.TempDir(), "core.ipod", ipodFile(t, plausibleImage(8192)))

	out, _, err := runCore(t, "install", path, "--yes", "--force")
	if err != nil {
		t.Fatalf("install --force: %v", err)
	}
	if !flash.Called {
		t.Fatal("--force did not reach the flasher")
	}
	if !strings.Contains(out, "--force") {
		t.Errorf("the output does not say it is overwriting a Core image:\n%s", out)
	}
	args := strings.Join(flash.Deps.ChildArgs, " ")
	if !strings.Contains(args, "--force") {
		t.Errorf("the elevated child would be refused: %v", flash.Deps.ChildArgs)
	}
}

// The whole sequence over the stub device: classify, flash with the
// install flag, then create on the volume what the firmware cannot
// create for itself.
func TestInstallCreatesTheDeviceFilesAndMusicFolder(t *testing.T) {
	volume := stubInstallDevice(t, appleInstalled())
	backup := filepath.Join(t.TempDir(), "apple-TESTSERIAL-2026-09-15.bin")
	flash := captureFlashResult(t, &flasher.Result{
		Verified: true, Device: `\\.\PhysicalDrive9`, BackupPath: backup,
		Installed: appleInstalled(),
	})
	path := writeTemp(t, t.TempDir(), "core.ipod", ipodFile(t, plausibleImage(8192)))

	out, _, err := runCore(t, "install", path, "--yes", "--backup-dir", filepath.Dir(backup))
	if err != nil {
		t.Fatalf("install: %v", err)
	}

	if !flash.Called {
		t.Fatal("install never reached the flash step")
	}
	if !flash.Opts.Install {
		t.Error("the flasher was not told this is an install")
	}
	if flash.Opts.Image != path || !flash.Opts.Yes || flash.Opts.DryRun {
		t.Errorf("flasher options: %+v", flash.Opts)
	}

	// The elevated child runs `install` — it has the volume step to do
	// — and is handed the file, never a release to fetch.
	wantArgs := "install " + path + " --backup-dir " + filepath.Dir(backup)
	if got := strings.Join(flash.Deps.ChildArgs, " "); got != wantArgs {
		t.Errorf("the elevated child would run\n  %s\nwant\n  %s", got, wantArgs)
	}

	// The three things the firmware cannot make for itself.
	for _, name := range []string{devicefs.ConfigName, devicefs.LogName} {
		st, err := os.Stat(filepath.Join(volume, name))
		if err != nil {
			t.Errorf("%s was not created: %v", name, err)
			continue
		}
		if st.Size() == 0 {
			t.Errorf("%s is empty", name)
		}
	}
	music, err := os.Stat(filepath.Join(volume, devicefs.MusicDir))
	if err != nil || !music.IsDir() {
		t.Errorf("%s\\ was not created: %v", devicefs.MusicDir, err)
	}
	if ents, _ := os.ReadDir(filepath.Join(volume, devicefs.MusicDir)); len(ents) != 0 {
		t.Errorf("%s\\ is not empty", devicefs.MusicDir)
	}

	// The device files must be the ones the firmware will accept, not
	// just files with the right names.
	head, err := os.ReadFile(filepath.Join(volume, devicefs.ConfigName))
	if err != nil {
		t.Fatal(err)
	}
	if _, ok := devicefs.ConfigFileValid(head); !ok {
		t.Error("the CORECFG.DAT written does not validate")
	}

	for _, want := range []string{
		"installed: Apple firmware (7.6 MB, entry 0x736000)",
		devicefs.ConfigName,
		devicefs.LogName,
		"created",
		backup,
		"core flash --from-backup " + backup,
		"Select+Play",
	} {
		if !strings.Contains(out, want) {
			t.Errorf("the output does not mention %q:\n%s", want, out)
		}
	}
}

// A second install over the same volume leaves the files alone: the
// config is the user's settings and the log is their history, and
// neither is something an install may reset.
func TestInstallKeepsExistingDeviceFiles(t *testing.T) {
	volume := stubInstallDevice(t, appleInstalled())
	if _, err := devicefs.EnsureConfig(volume); err != nil {
		t.Fatal(err)
	}
	cfg := filepath.Join(volume, devicefs.ConfigName)
	before, err := os.ReadFile(cfg)
	if err != nil {
		t.Fatal(err)
	}
	captureFlashResult(t, &flasher.Result{Verified: true, Installed: appleInstalled()})
	path := writeTemp(t, t.TempDir(), "core.ipod", ipodFile(t, plausibleImage(8192)))

	out, _, err := runCore(t, "install", path, "--yes")
	if err != nil {
		t.Fatalf("install: %v", err)
	}
	after, err := os.ReadFile(cfg)
	if err != nil {
		t.Fatal(err)
	}
	if string(before) != string(after) {
		t.Error("the install rewrote an existing, valid CORECFG.DAT")
	}
	if !strings.Contains(out, "already there and valid") {
		t.Errorf("the output does not say the file was kept:\n%s", out)
	}
}

// --dry-run writes nothing anywhere — not the device (the flasher's
// job) and not the volume (this command's).
func TestInstallDryRunTouchesTheVolumeNot(t *testing.T) {
	volume := stubInstallDevice(t, appleInstalled())
	flash := captureFlashResult(t, &flasher.Result{DryRun: true})
	path := writeTemp(t, t.TempDir(), "core.ipod", ipodFile(t, plausibleImage(8192)))

	out, _, err := runCore(t, "install", path, "--dry-run")
	if err != nil {
		t.Fatalf("install --dry-run: %v", err)
	}
	if !flash.Opts.DryRun {
		t.Error("--dry-run did not reach the flasher")
	}
	ents, err := os.ReadDir(volume)
	if err != nil {
		t.Fatal(err)
	}
	if len(ents) != 0 {
		t.Errorf("--dry-run created %d entries on the volume", len(ents))
	}
	if !strings.Contains(out, "--dry-run") {
		t.Errorf("the output does not say nothing was written:\n%s", out)
	}
}

// With no file argument the latest release is fetched, verified and
// flashed — the same client, cache and checksum check `core update`
// uses, and the same rule that the elevated child never downloads.
func TestInstallFetchesTheLatestRelease(t *testing.T) {
	body := ipodFile(t, plausibleImage(8192))
	g := newFakeGitHub(t)
	g.publish(releaseV013(g, body))
	isolateCache(t)
	stubInstallDevice(t, appleInstalled())
	flash := captureFlashResult(t, &flasher.Result{Verified: true, Installed: appleInstalled()})

	out, _, err := runCore(t, "install", "--yes", "--repo", "owner/name")
	if err != nil {
		t.Fatalf("install: %v", err)
	}
	if !flash.Called {
		t.Fatal("install never reached the flash step")
	}
	if !strings.HasSuffix(flash.Opts.Image, "core-v0.1.3.ipod") {
		t.Errorf("flasher got image %q, want the downloaded release asset", flash.Opts.Image)
	}
	got, err := os.ReadFile(flash.Opts.Image)
	if err != nil || string(got) != string(body) {
		t.Errorf("the cached file is not the release asset (%v)", err)
	}
	for _, a := range flash.Deps.ChildArgs {
		if a == "--repo" || a == "--tag" {
			t.Errorf("the elevated child would do network work: %v", flash.Deps.ChildArgs)
		}
	}
	if !strings.Contains(out, "v0.1.3") || !strings.Contains(out, "verified:") {
		t.Errorf("the output does not report the release and its verification:\n%s", out)
	}
	if g.hits() != 1 {
		t.Errorf("%d downloads for one install", g.hits())
	}
}

func TestInstallHasItsFlags(t *testing.T) {
	cmd := findCmd(t, "install")
	for _, flag := range []string{"yes", "dry-run", "force", "backup-dir",
		"untested-hardware", "no-relaunch", "repo", "tag"} {
		if cmd.Flags().Lookup(flag) == nil {
			t.Errorf("core install has no --%s", flag)
		}
	}
	if cmd.InheritedFlags().Lookup("device") == nil {
		t.Error("core install cannot see the global --device flag")
	}
}

// With nothing plugged in, `install` must fail the way every other
// device command does: with the thing to try next.
func TestInstallWithoutADeviceExplainsItself(t *testing.T) {
	_, _, err := runCore(t, "install", "--yes", "--dry-run")
	if err == nil {
		t.Skip("this machine has something that identifies as an iPod attached")
	}
	msg := err.Error()
	actionable := strings.Contains(msg, "disk mode") ||
		strings.Contains(msg, "sudo ") ||
		strings.Contains(msg, "RunAs") ||
		strings.Contains(msg, "Administrator")
	if !actionable {
		t.Errorf("install failed without telling the user what to do:\n%s", msg)
	}
}
