package cli

import (
	"bufio"
	"errors"
	"fmt"
	"os"
	"runtime"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/disk"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flasher"
	"github.com/spf13/cobra"
)

// flash is deliberately thin. Everything that decides anything lives in
// internal/flasher, where it can be run end to end against an in-memory
// device; this file is flags, wiring, and the exit code.

// ExitError asks main to exit with a specific status instead of 1. It
// exists for one case: the elevated child's exit code has to reach the
// shell that ran the parent, or a script cannot tell a flash that
// failed inside the UAC child from one that succeeded.
type ExitError struct {
	Code int
	Err  error
}

func (e *ExitError) Error() string { return e.Err.Error() }
func (e *ExitError) Unwrap() error { return e.Err }

func newFlashCmd() *cobra.Command {
	var (
		fromBackup string
		dryRun     bool
		yes        bool
		backupDir  string
		untested   bool
		noRelaunch bool
	)
	cmd := &cobra.Command{
		Use:   "flash [core.ipod | core.bin]",
		Short: "Write a firmware image to the iPod, or restore a whole partition backup",
		Long: `Replaces the OSOS image on the connected iPod's firmware partition —
the body at devOffset + 0x800, zero-padded to the next 0x800 boundary,
and the single directory sector holding that entry's row, with the new
length and checksum and every other field kept. The preamble, the
partition table and Apple's other images are never written.

With --from-backup, writes a whole firmware partition back from a file
produced by "core backup" (or ipodpatcher -r). That is the recovery
path: it restores the preamble and every image, and it refuses a file
whose size does not match the device's partition.

The sequence is fixed and nothing skips it:

  1. the input is parsed — a .ipod file must pass its checksum, a raw
     .bin must pass the same plausibility rules as "firmware pack"
  2. the iPod is identified three ways (see "core info")
  3. untested hardware is refused without --untested-hardware
  4. the device is opened READ-ONLY and the current directory printed
  5. the plan is printed: offsets, old and new length and checksum,
     capacity, and the backup path
  6. --dry-run stops here, having opened nothing for writing
  7. you type the device path to confirm (--yes skips this)
  8. on Windows an elevated copy of this command is started, unless
     --no-relaunch
  9. the WHOLE partition is backed up, fsynced, and re-parsed from disk
 10. the writes are applied: body first, directory sector second
 11. the device is re-opened read-only and the write is read back

If anything after step 9 fails, the backup path and the exact recovery
commands are printed. The floor under all of it is ROM disk mode:
Select+Menu to reset, then Select+Play at the Apple logo.`,
		Args: cobra.MaximumNArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			o := flasher.Options{
				FromBackup: fromBackup,
				Device:     deviceFlag(cmd),
				DryRun:     dryRun,
				Yes:        yes,
				BackupDir:  backupDir,
				Untested:   untested,
				NoRelaunch: noRelaunch,
			}
			if len(args) == 1 {
				o.Image = args[0]
			}
			if o.Image == "" && o.FromBackup == "" {
				return errors.New("nothing to write: pass an image path, or --from-backup <fwpart.bin>")
			}
			if o.Image != "" && o.FromBackup != "" {
				return errors.New("pass either an image path or --from-backup, not both")
			}

			out := cmd.OutOrStdout()
			res, err := flasher.Flash(cmd.Context(), o, flasher.Deps{
				GOOS:      runtime.GOOS,
				ChildArgs: os.Args[1:],
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
		},
	}
	cmd.Flags().StringVar(&fromBackup, "from-backup", "",
		"Restore a whole firmware partition from this file instead of writing an image")
	cmd.Flags().BoolVar(&dryRun, "dry-run", false,
		"Print the plan and stop; nothing is opened for writing and no backup is taken")
	cmd.Flags().BoolVarP(&yes, "yes", "y", false,
		"Skip the typed confirmation (the backup and the read-back still happen)")
	cmd.Flags().StringVar(&backupDir, "backup-dir", "",
		"Where to write the whole-partition backup (default: <user config dir>/core/backups)")
	cmd.Flags().BoolVar(&untested, "untested-hardware", false,
		"Write to an iPod outside the tested capacity window ("+disk.TestedModel+" only)")
	cmd.Flags().BoolVar(&noRelaunch, "no-relaunch", false,
		"Never start an elevated copy of this command; refuse and print the command instead")
	return cmd
}
