// Package cli wires the cobra command tree.
package cli

import (
	"fmt"
	"io"
	"os"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/disk"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/version"
	"github.com/spf13/cobra"
)

// Root returns the configured root command. Each subcommand lives in
// its own file in this package. Only commands with code behind them are
// registered: a command that exists in the help text and returns "not
// yet implemented" reads, to anyone who did not write it, as a command
// that works. The commands still to be written are listed in
// core/cli/README.md with the slice that lands them.
func Root() *cobra.Command {
	root := &cobra.Command{
		Use:           "core",
		Short:         "Custom iPod Video 5G/5.5G firmware — host CLI",
		Long:          longDescription,
		Version:       version.Full(),
		SilenceUsage:  true,
		SilenceErrors: true,
	}

	// Global device selector. internal/disk defines ErrMultipleDevices
	// ("select one with --device") and this is that way. Read it with
	// deviceFlag.
	root.PersistentFlags().String("device", "",
		"Target a specific iPod: OS block-device path (/dev/sdX, /dev/diskN, "+
			"\\\\.\\PhysicalDriveN) or serial number. Required when more than one is connected.")

	// The elevated child's flag. On Windows a "run as administrator"
	// process gets its OWN console window, which closes the moment it
	// exits — so disk.RelaunchElevated appends this flag and the parent
	// reads the file back. Hidden because nobody should type it: it is
	// a channel between two copies of this binary, not an option.
	root.PersistentFlags().String("elevated-log", "",
		"Tee all output to this file (used by the elevated child process on Windows)")
	_ = root.PersistentFlags().MarkHidden("elevated-log")

	root.PersistentPreRunE = func(cmd *cobra.Command, args []string) error {
		return openElevatedLog(cmd)
	}

	root.AddCommand(
		newBuildCmd(),
		newInfoCmd(),
		newBackupCmd(),
		newFirmwareCmd(),
		newIndexCmd(),
		newArtCmd(),
		newOrganizeCmd(),
		newFixCmd(),
		newSyncCmd(),
		newEjectCmd(),
		newNameCmd(),
		newInstallCmd(),
		newFlashCmd(),
		newUpdateCmd(),
		newDoctorCmd(),
	)

	return root
}

// deviceFlag returns the value of the global --device selector, or ""
// when the user didn't pass one. Defined here so no command grows its
// own private copy of the flag.
func deviceFlag(cmd *cobra.Command) string {
	v, err := cmd.Flags().GetString("device")
	if err != nil {
		return ""
	}
	return v
}

// openElevatedLog wires --elevated-log up, if it was passed.
//
// The writers are set on the ROOT command: cobra's OutOrStdout walks up
// to the parent when a command has no writer of its own, so one place
// covers every subcommand, and cmd/core's final "error: …" line (which
// prints through root.ErrOrStderr) lands in the log too. That last part
// is the whole point — the failure is the thing the parent needs to
// show, and it is the thing that would otherwise vanish with the
// child's console.
func openElevatedLog(cmd *cobra.Command) error {
	path, err := cmd.Flags().GetString("elevated-log")
	if err != nil || path == "" {
		return nil
	}
	f, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0o644)
	if err != nil {
		return fmt.Errorf("open the elevated log %s: %w", path, err)
	}
	// Deliberately not closed: it lives as long as the process, and
	// os.File writes are unbuffered, so there is nothing to lose.
	root := cmd.Root()
	root.SetOut(io.MultiWriter(os.Stdout, f))
	root.SetErr(io.MultiWriter(os.Stderr, f))
	return nil
}

// Elevated reports whether this process can open a raw device by right.
// Re-exported from internal/disk so command code does not have to
// import it for one predicate.
func Elevated() bool { return disk.IsElevated() }

const longDescription = `core is the host-side CLI for the custom iPod Video firmware project.

Available today:
  core info           Identify a connected iPod and describe its firmware partition
  core backup         Dump the whole firmware partition to a file
  core firmware       Pack / unpack / inspect images; read the OSOS body off a device
  core build          Cross-compile the firmware (make -C core <target>)
  core index          Build CORELIB.IDX from a source music tree
  core art            Bake folder.art / folder.thm sidecars
  core organize       Rename and re-folder a music tree from its tags
  core fix            List everything wrong with a library, and fix it
  core sync           Put a music tree on the iPod: files, art, playlists, index
  core eject          Flush and eject the volume so the cable can be pulled
  core name           Read or set the iPod's name (its FAT volume label)
  core install        Put Core on an iPod still running Apple's firmware
  core flash          Write a firmware image, or restore a partition backup
  core update         Fetch the latest firmware release and flash it
  core doctor         Read-only health check of a device and its library volume

The desktop application is a second binary, core-app, which ships beside
this one. It links the same packages; on Windows a flash from the app
runs THIS binary elevated, because a windowsgui process has no console
for a UAC child to write to.

info, backup, doctor and firmware read only ever READ the device; flash
and update are the only commands that write one, and both back the whole
partition up first. Reading a raw disk needs Administrator on Windows and
root on Linux/macOS; when the open is refused, the error prints the exact
elevated command to run.

Run "core <command> --help" for command-specific options.
`
