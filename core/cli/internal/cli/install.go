package cli

import (
	"bufio"
	"context"
	"fmt"
	"runtime"
	"strings"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/disk"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/fwpart"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/ghrelease"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/installer"
	"github.com/spf13/cobra"
)

// `core install` is the one-command version of "put this firmware on a
// stock iPod": classify what is there, refuse to touch a device that is
// already running Core, keep Apple's firmware under a name a person can
// find, write the image through exactly the same flasher sequence as
// `core flash`, and then create the two files and the folder the
// firmware expects on the FAT volume.
//
// Nothing about the WRITE is different from `core flash` — same backup,
// same typed confirmation, same read-back, same entry-point policy. What
// is different is everything around it, and that is the whole reason the
// command exists: the person installing this for the first time should
// not have to know that the OSOS row has an entry point, that Apple's
// firmware is unobtainable once overwritten, or that the device will not
// boot without CORECFG.DAT.
//
// The sequence itself lives in internal/installer, because core-app runs
// exactly the same one from its Install screen. What is left here is the
// command line: the flags, the release download, and the exit code.

// installDeviceFunc is the seam the tests replace: find the iPod, read
// its firmware partition, say what is installed. It is a package
// variable for the same reason update.go's are — the whole command can
// then be driven through the real cobra tree with no disk attached.
var installDeviceFunc = inspectInstallDevice

func newInstallCmd() *cobra.Command {
	var (
		yes        bool
		dryRun     bool
		force      bool
		backupDir  string
		untested   bool
		noRelaunch bool
		repo       string
		tag        string
	)
	cmd := &cobra.Command{
		Use:   "install [core.ipod]",
		Short: "Put Core on an iPod that is still running Apple's firmware",
		Long: `Installs this firmware on a connected iPod for the first time.

With no file argument the latest GitHub release is downloaded and its
.ipod transport checksum verified, exactly as "core update" does.

The sequence:

  1. the iPod is found and its OSOS image is classified — Core, an old
     Core, or something else (Apple's firmware, on a device that has
     never been flashed)
  2. a device already running Core is refused, with "core update" as
     the thing to run instead; --force overrides
  3. the WHOLE firmware partition is backed up. When what is being
     overwritten is Apple's firmware the backup is named
     apple-<serial>-<date>.bin, because it is the only copy of that
     device's original firmware in existence — Apple does not
     distribute it and it cannot be taken from another iPod
  4. the image is written: the OSOS body, and the one directory row,
     with the entry point set to 0 (this image starts at its first
     byte; Apple's starts 7.5 MB in, and a row left saying so is a
     device that boots to nothing)
  5. the write is read back and compared
  6. on the music volume: CORECFG.DAT and CORELOG.BIN are created if
     they are missing or invalid, and Music\ is created if it is not
     there. The firmware can overwrite these files but cannot create
     them, so an install that skipped this step would boot with no
     settings and no log

If anything goes wrong the backup path and the command that puts the
old firmware back are printed. The floor under all of it is ROM disk
mode: Select+Menu to reset, then Select+Play at the Apple logo.`,
		Args: cobra.MaximumNArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			o := installOptions{
				Yes:        yes,
				DryRun:     dryRun,
				Force:      force,
				BackupDir:  backupDir,
				Untested:   untested,
				NoRelaunch: noRelaunch,
				Repo:       strings.TrimSpace(repo),
				Tag:        strings.TrimSpace(tag),
			}
			if len(args) == 1 {
				o.Image = args[0]
			}
			return runInstall(cmd, o)
		},
	}
	cmd.Flags().BoolVarP(&yes, "yes", "y", false,
		"Skip the typed confirmation (the backup and the read-back still happen)")
	cmd.Flags().BoolVar(&dryRun, "dry-run", false,
		"Print the plan and stop; nothing is opened for writing, no backup is taken and "+
			"the music volume is not touched")
	cmd.Flags().BoolVar(&force, "force", false,
		"Install even though the device is already running Core (normally: use \"core update\")")
	cmd.Flags().StringVar(&backupDir, "backup-dir", "",
		"Where to write the whole-partition backup (default: <user config dir>/core/backups)")
	cmd.Flags().BoolVar(&untested, "untested-hardware", false,
		"Write to an iPod outside the tested capacity window ("+disk.TestedModel+" only)")
	cmd.Flags().BoolVar(&noRelaunch, "no-relaunch", false,
		"Never start an elevated copy of this command; refuse and print the command instead")
	cmd.Flags().StringVar(&repo, "repo", ghrelease.DefaultRepo,
		"GitHub repository to read releases from, as owner/name")
	cmd.Flags().StringVar(&tag, "tag", "",
		"Install this release instead of the latest (e.g. v0.1.3)")
	return cmd
}

type installOptions struct {
	Image      string
	Yes        bool
	DryRun     bool
	Force      bool
	BackupDir  string
	Untested   bool
	NoRelaunch bool
	Repo       string
	Tag        string
}

// installDevice is what the classification step found. It is an alias
// rather than a second struct so the stub the tests install behind
// installDeviceFunc is the very value internal/installer consumes.
type installDevice = installer.Device

func runInstall(cmd *cobra.Command, o installOptions) error {
	out := cmd.OutOrStdout()
	if o.Repo == "" {
		o.Repo = ghrelease.DefaultRepo
	}

	res, err := installer.Install(cmd.Context(), installer.Options{
		Image:      o.Image,
		Device:     deviceFlag(cmd),
		Yes:        o.Yes,
		DryRun:     o.DryRun,
		Force:      o.Force,
		BackupDir:  o.BackupDir,
		Untested:   o.Untested,
		NoRelaunch: o.NoRelaunch,
	}, installer.Deps{
		Inspect:      func(context.Context) (installer.Device, error) { return installDeviceFunc(cmd) },
		ResolveImage: func(context.Context) (string, error) { return installReleaseImage(cmd, o) },
		Flash:        flashImage,
		Out:          out,
		GOOS:         runtime.GOOS,
		// The elevated child is given the image PATH the parent already
		// downloaded and verified, never a release tag, so the elevated
		// process makes no network request. It runs `install` and not
		// `flash` because the volume step is part of the install and the
		// child is the process that gets to the end of the sequence.
		ChildArgs: func(image string) []string { return installChildArgs(cmd, o, image) },
		Confirm: func(prompt string) (string, error) {
			fmt.Fprint(out, prompt)
			line, rerr := bufio.NewReader(cmd.InOrStdin()).ReadString('\n')
			if rerr != nil && line == "" {
				return "", rerr
			}
			return line, nil
		},
	})
	if err != nil {
		if res != nil && res.Flash != nil && res.Flash.Relaunched && res.Flash.ChildExit != 0 {
			return &ExitError{Code: res.Flash.ChildExit, Err: err}
		}
		return err
	}
	return nil
}

// installReleaseImage downloads the release image, reusing `core
// update`'s release client, cache and checksum verification verbatim.
func installReleaseImage(cmd *cobra.Command, o installOptions) (string, error) {
	out := cmd.OutOrStdout()
	rel, err := fetchRelease(cmd.Context(), updateOptions{Repo: o.Repo, Tag: o.Tag})
	if err != nil {
		return "", err
	}
	asset, ok := ghrelease.FirmwareAsset(rel)
	if !ok {
		return "", fmt.Errorf("%w: %s carries %d asset(s), none of them core.ipod or core-%s.ipod",
			ghrelease.ErrNoAsset, rel.Tag, len(rel.Assets), rel.Tag)
	}
	fmt.Fprintf(out, "release:   %s — %s (%d bytes)\n", rel.Tag, asset.Name, asset.Size)
	return fetchImage(cmd.Context(), out, rel, asset)
}

// installChildArgs is the command the elevated child runs on Windows.
//
// It is another `install` — not a `flash` — because the volume work in
// step 6 is part of the install, and the child is the process that gets
// to the end of the sequence. It is given the image PATH the parent
// already downloaded and verified, never a release tag, so the elevated
// process makes no network request (the rule `core update` follows, for
// the same reason).
func installChildArgs(cmd *cobra.Command, o installOptions, image string) []string {
	args := []string{"install", image}
	if d := deviceFlag(cmd); d != "" {
		args = append(args, "--device", d)
	}
	if o.BackupDir != "" {
		args = append(args, "--backup-dir", o.BackupDir)
	}
	if o.Untested {
		args = append(args, "--untested-hardware")
	}
	if o.Force {
		args = append(args, "--force")
	}
	return args
}

// inspectInstallDevice opens the connected iPod read-only and says what
// firmware is on it.
func inspectInstallDevice(cmd *cobra.Command) (installDevice, error) {
	pod, h, err := openDevice(cmd)
	if err != nil {
		return installDevice{}, err
	}
	defer h.Close()

	inst, _, err := fwpart.ClassifyPartition(firmwarePartition(pod, h))
	if err != nil {
		return installDevice{}, fmt.Errorf("reading the firmware partition on %s: %w",
			pod.Disk.Path, err)
	}
	return installDevice{Pod: pod, Installed: inst}, nil
}
