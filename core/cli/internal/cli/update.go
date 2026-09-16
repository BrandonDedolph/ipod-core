package cli

import (
	"bufio"
	"context"
	"fmt"
	"io"
	"runtime"
	"strings"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/disk"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flasher"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/fwpart"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/ghrelease"
	"github.com/spf13/cobra"
)

// `core update` is `core flash` with the image fetched instead of
// typed. Everything that decides whether a write is safe still lives in
// internal/flasher and is reached through the same Flash call
// internal/cli/flash.go makes — this file adds exactly two things in
// front of it: which version is on the device, and which version is on
// GitHub.
//
// The elevated child is deliberately NOT another `update`. It is
// re-invoked as `flash <cached file> --yes --no-relaunch`, so the
// process that runs with Administrator rights does no network I/O at
// all: it opens a file that the unelevated parent already downloaded,
// size-checked and checksum-verified, and writes it. A UAC child that
// could fetch its own bytes would move the trust decision into the
// process with the most power and the least supervision.

// Seams for the tests. Each is the real thing in production and a
// package variable only so `core update` can be driven end to end
// without a network, a GitHub account or an iPod on the bench.
var (
	newReleaseClient  = ghrelease.New
	flashImage        = flasher.Flash
	deviceVersionFunc = deviceFirmwareVersion
)

// notesLineLimit is how much of a release body is printed before the
// rest is elided. Release notes are written for humans and are usually
// short; the limit is there for the one that pastes a changelog.
const notesLineLimit = 40

func newUpdateCmd() *cobra.Command {
	var (
		check      bool
		tag        string
		yes        bool
		repo       string
		untested   bool
		noRelaunch bool
		backupDir  string
	)
	cmd := &cobra.Command{
		Use:   "update",
		Short: "Fetch the latest firmware release and flash it",
		Long: `Compares the version on the connected iPod with the latest release on
GitHub, prints the release notes, downloads the ` + "`.ipod`" + ` image into a
local cache, verifies its transport checksum, and then runs exactly the
same sequence as "core flash" — whole-partition backup first, typed
confirmation, read-back verification.

The device's version comes from a tagged marker inside the installed
image (see "core info"). Images before v0.1.3 do not carry one, so the
answer there is "unknown"; that is not an error and does not stop an
update.

An iPod that is still running Apple's firmware is an INSTALL, not an
update: "core install" keeps Apple's firmware under a name you can find
again and creates the files the firmware needs on the music volume.

--check prints the comparison and the release notes and stops, having
downloaded nothing and opened nothing for writing.

There is no signature checking here. This project has no release key.
What is verified is the .ipod transport checksum, which is an integrity
check: it catches a truncated or corrupted download, not a hostile one.`,
		Args: cobra.NoArgs,
		RunE: func(cmd *cobra.Command, args []string) error {
			return runUpdate(cmd, updateOptions{
				Check:      check,
				Tag:        strings.TrimSpace(tag),
				Yes:        yes,
				Repo:       strings.TrimSpace(repo),
				Untested:   untested,
				NoRelaunch: noRelaunch,
				BackupDir:  backupDir,
			})
		},
	}
	cmd.Flags().BoolVar(&check, "check", false,
		"Print the comparison and the release notes, then stop; downloads nothing")
	cmd.Flags().StringVar(&tag, "tag", "",
		"Update to this release instead of the latest (e.g. v0.1.3); also allows re-flashing "+
			"the version already installed")
	cmd.Flags().BoolVarP(&yes, "yes", "y", false,
		"Skip the typed confirmation (the backup and the read-back still happen)")
	cmd.Flags().StringVar(&repo, "repo", ghrelease.DefaultRepo,
		"GitHub repository to read releases from, as owner/name")
	cmd.Flags().BoolVar(&untested, "untested-hardware", false,
		"Write to an iPod outside the tested capacity window ("+disk.TestedModel+" only)")
	cmd.Flags().BoolVar(&noRelaunch, "no-relaunch", false,
		"Never start an elevated copy of this command; refuse and print the command instead")
	cmd.Flags().StringVar(&backupDir, "backup-dir", "",
		"Where to write the whole-partition backup (default: <user config dir>/core/backups)")
	return cmd
}

type updateOptions struct {
	Check      bool
	Tag        string
	Yes        bool
	Repo       string
	Untested   bool
	NoRelaunch bool
	BackupDir  string
}

func runUpdate(cmd *cobra.Command, o updateOptions) error {
	out := cmd.OutOrStdout()
	if o.Repo == "" {
		o.Repo = ghrelease.DefaultRepo
	}

	// The device first, and its failure is not fatal here. `update
	// --check` on a machine with nothing plugged in is a reasonable
	// thing to run, and the flash path below finds the device itself
	// and produces the actionable error if there is none.
	installed, deviceErr := deviceVersionFunc(cmd)

	rel, err := fetchRelease(cmd.Context(), o)
	if err != nil {
		return err
	}

	printComparison(out, installed, deviceErr, rel, o)

	asset, haveAsset := ghrelease.FirmwareAsset(rel)
	if haveAsset {
		fmt.Fprintf(out, "asset:  %s (%d bytes)\n", asset.Name, asset.Size)
	} else {
		fmt.Fprintf(out, "asset:  none — release %s has no firmware image attached\n", rel.Tag)
	}
	printNotes(out, rel.Notes)

	if o.Check {
		fmt.Fprintf(out, "\n--check: nothing was downloaded and nothing was written.\n")
		return nil
	}

	// Up to date. --tag is the explicit override: someone asking for a
	// specific version by name is asking for it to be written, which is
	// how a device that took a bad flash of the current version gets
	// the good one back.
	if installed != "" && installed == rel.Tag {
		if o.Tag == "" {
			fmt.Fprintf(out, "\nalready up to date: the device runs %s, which is the latest release.\n"+
				"Pass --tag %s to write it again anyway.\n", rel.Tag, rel.Tag)
			return nil
		}
		fmt.Fprintf(out, "\nthe device already runs %s; --tag was given, so it will be written again.\n",
			rel.Tag)
	}

	if !haveAsset {
		return fmt.Errorf("%w: %s carries %d asset(s), none of them core.ipod or core-%s.ipod",
			ghrelease.ErrNoAsset, rel.Tag, len(rel.Assets), rel.Tag)
	}

	path, err := fetchImage(cmd.Context(), out, rel, asset)
	if err != nil {
		return err
	}
	return flashCached(cmd, o, path)
}

func fetchRelease(ctx context.Context, o updateOptions) (ghrelease.Release, error) {
	c := newReleaseClient()
	if o.Tag != "" {
		rel, err := c.ByTag(ctx, o.Repo, o.Tag)
		if err != nil {
			return ghrelease.Release{}, fmt.Errorf("looking up release %s in %s: %w", o.Tag, o.Repo, err)
		}
		return rel, nil
	}
	rel, err := c.Latest(ctx, o.Repo)
	if err != nil {
		return ghrelease.Release{}, fmt.Errorf("looking up the latest release in %s: %w", o.Repo, err)
	}
	return rel, nil
}

// printComparison is the two-line header: what is installed, what is
// available, and — when the device could not answer — why.
func printComparison(out io.Writer, installed string, deviceErr error, rel ghrelease.Release, o updateOptions) {
	label := "latest"
	if o.Tag != "" {
		label = "requested"
	}
	shown := installed
	if shown == "" {
		shown = "unknown"
	}
	if deviceErr != nil {
		shown = "not read"
	}
	fmt.Fprintf(out, "device: %s · %s: %s\n", shown, label, rel.Tag)
	switch {
	case deviceErr != nil:
		fmt.Fprintf(out, "        %s\n", firstLine(deviceErr.Error()))
	case installed == "":
		fmt.Fprintf(out, "        no version marker in the installed image; "+
			"images before v0.1.3 carry none\n")
		fmt.Fprintf(out, "        (if this iPod has never had Core on it, the command is "+
			"`core install`; `core info` says which it is)\n")
	}
	if name := strings.TrimSpace(rel.Name); name != "" && name != rel.Tag {
		fmt.Fprintf(out, "release: %s — %s\n", rel.Tag, name)
	}
}

// printNotes prints the release body, bounded.
func printNotes(out io.Writer, notes string) {
	notes = strings.ReplaceAll(notes, "\r\n", "\n")
	notes = strings.TrimRight(notes, "\n")
	if strings.TrimSpace(notes) == "" {
		fmt.Fprintf(out, "\n(the release carries no notes)\n")
		return
	}
	lines := strings.Split(notes, "\n")
	fmt.Fprintf(out, "\n--- release notes ---\n")
	for i, line := range lines {
		if i == notesLineLimit {
			fmt.Fprintf(out, "…\n")
			break
		}
		fmt.Fprintln(out, line)
	}
	fmt.Fprintf(out, "--- end of release notes ---\n")
}

func firstLine(s string) string {
	if i := strings.IndexByte(s, '\n'); i >= 0 {
		return s[:i]
	}
	return s
}

// fetchImage returns the path of a verified local copy of the asset,
// downloading it only if the cache does not already hold one.
func fetchImage(ctx context.Context, out io.Writer, rel ghrelease.Release, a ghrelease.Asset) (string, error) {
	dst, err := ghrelease.CachePath(rel.Tag, a.Name)
	if err != nil {
		return "", err
	}
	if ghrelease.Cached(a, dst) {
		fmt.Fprintf(out, "\ncached: %s (already downloaded and verified)\n", dst)
		return dst, nil
	}
	fmt.Fprintf(out, "\ndownloading %s (%d bytes) …\n", a.Name, a.Size)
	if err := newReleaseClient().Download(ctx, a, dst); err != nil {
		return "", err
	}
	fmt.Fprintf(out, "verified: %s (.ipod transport checksum OK)\n", dst)
	return dst, nil
}

// flashCached hands the downloaded file to internal/flasher with the
// same Deps internal/cli/flash.go builds, with one difference that
// matters: ChildArgs describes a `flash` command, not this one.
func flashCached(cmd *cobra.Command, o updateOptions, path string) error {
	out := cmd.OutOrStdout()
	fmt.Fprintln(out)

	res, err := flashImage(cmd.Context(), flasher.Options{
		Image:      path,
		Device:     deviceFlag(cmd),
		Yes:        o.Yes,
		BackupDir:  o.BackupDir,
		Untested:   o.Untested,
		NoRelaunch: o.NoRelaunch,
	}, flasher.Deps{
		GOOS:      runtime.GOOS,
		ChildArgs: flashChildArgs(cmd, o, path),
		Out:       out,
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
		if res != nil && res.Relaunched && res.ChildExit != 0 {
			return &ExitError{Code: res.ChildExit, Err: err}
		}
		return err
	}
	return nil
}

// flashChildArgs is the command the elevated child runs. flasher's own
// childArgs appends --yes and --no-relaunch, so what is built here is
// everything else: the subcommand, the cached image, and the flags that
// change what gets written or where the backup goes.
//
// os.Args is deliberately not reused. The parent's arguments say
// `update`, and an elevated `update` would re-resolve the release and
// re-download it as Administrator — the exact thing this command is
// arranged to avoid.
func flashChildArgs(cmd *cobra.Command, o updateOptions, path string) []string {
	args := []string{"flash", path}
	if d := deviceFlag(cmd); d != "" {
		args = append(args, "--device", d)
	}
	if o.BackupDir != "" {
		args = append(args, "--backup-dir", o.BackupDir)
	}
	if o.Untested {
		args = append(args, "--untested-hardware")
	}
	return args
}

// deviceFirmwareVersion reads the installed version off the connected
// iPod. It returns "" with a nil error for an image that carries no
// marker (everything before v0.1.3), and an error only when the device
// could not be found, opened or parsed.
func deviceFirmwareVersion(cmd *cobra.Command) (string, error) {
	pod, h, err := openDevice(cmd)
	if err != nil {
		return "", err
	}
	defer h.Close()

	p := firmwarePartition(pod, h)
	d, err := fwpart.Parse(p)
	if err != nil {
		return "", fmt.Errorf("reading the firmware partition on %s: %w", pod.Disk.Path, err)
	}
	_, osos, ok := d.OSOS()
	if !ok {
		return "", fmt.Errorf("%s: %w", pod.Disk.Path, fwpart.ErrNoOSOS)
	}
	body, err := fwpart.ReadBody(p, osos)
	if err != nil {
		return "", err
	}
	v, _, found := fwpart.FindVersion(body)
	if !found {
		return "", nil
	}
	return v, nil
}
