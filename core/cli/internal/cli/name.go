package cli

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"strings"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/disk"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/doctor"
	"github.com/spf13/cobra"
)

// `core name` is the iPod's name.
//
// There is exactly one name on the device a host and the firmware could
// both read, and it is the FAT32 volume label of the music partition —
// eleven bytes, upper-case, ASCII, in the root directory's volume entry.
// That is not a name anybody would choose to type: `Brandon's iPod`
// only fits as `BRANDON'S I`. So the name exists in two halves. The
// label is what the device and Explorer show; the friendly name is kept
// beside it in the app's own config.json, keyed by the disk's serial,
// and is what core-app puts in its header. This command writes both and
// prints both, so the truncation is a thing the user is told about
// rather than a thing they discover in Explorer.
//
// The seams below are package variables and not an interface because
// there is one implementation and one fake: the tests need a volume
// that does not exist on this machine, and nothing else ever wants to
// substitute these.
var (
	nameLabelGet   = disk.VolumeLabel
	nameLabelSet   = disk.SetVolumeLabel
	nameDiskSerial = disk.VolumeDiskSerial
	nameConfigPath = defaultNameConfigPath
)

func newNameCmd() *cobra.Command {
	var labelOnly bool
	cmd := &cobra.Command{
		Use:   "name <drive-or-mount> [<name>]",
		Short: "Read or set the iPod's name (its FAT volume label)",
		Long: `Reads or sets the name of the iPod's music volume.

With no name, it prints what is there: the volume label as stored, the
serial of the disk it is on, and the friendly name core-app remembers
for that serial, if any.

With a name, it writes two things:

  the label     LegalLabel(<name>) — upper-cased, ASCII only, at most
                11 bytes, because that is the whole of a FAT volume
                entry. "Brandon's iPod" becomes "BRANDON'S I", and the
                command says so before it is applied. The label is read
                back and compared.
  the name      <name> verbatim, in <user config dir>/core/config.json
                under the disk's serial, which is what core-app shows in
                its header. --label-only skips this half.

An empty name ("") clears both.

Writing a label needs the same rights as any other volume operation
(Administrator on Windows), and the volume must not be locked — never
rename during a flash, which dismounts the drive letter.`,
		Args:         cobra.RangeArgs(1, 2),
		SilenceUsage: true,
		RunE: func(cmd *cobra.Command, args []string) error {
			vol := strings.TrimSpace(args[0])
			if vol == "" {
				return errors.New("name the volume (a drive letter, a mount point)")
			}
			if len(args) == 1 {
				return runNameShow(cmd.OutOrStdout(), vol)
			}
			return runNameSet(cmd.OutOrStdout(), vol, args[1], labelOnly)
		},
	}
	cmd.Flags().BoolVar(&labelOnly, "label-only", false,
		"Write the volume label only; do not remember the friendly name")
	return cmd
}

// runNameShow is the read-only half.
func runNameShow(out io.Writer, vol string) error {
	root := disk.VolumeRoot(vol)
	label, err := nameLabelGet(root)
	if err != nil {
		return fmt.Errorf("reading the volume label of %s: %w", root, err)
	}
	serial, _ := nameDiskSerial(root)
	friendly := lookupFriendlyName(serial)

	fmt.Fprintf(out, "volume      %s\n", root)
	fmt.Fprintf(out, "label       %s\n", labelText(label))
	if serial != "" {
		fmt.Fprintf(out, "serial      %s\n", serial)
	}
	if friendly != "" {
		fmt.Fprintf(out, "name        %s\n", friendly)
	} else {
		fmt.Fprintf(out, "name        (none — `core name %s \"Brandon's iPod\"` sets one)\n", vol)
	}
	return nil
}

// runNameSet writes the label, reads it back, and files the friendly
// name under the disk's serial.
func runNameSet(out io.Writer, vol, friendly string, labelOnly bool) error {
	root := disk.VolumeRoot(vol)
	friendly = strings.TrimSpace(friendly)
	label := disk.LegalLabel(friendly)
	if friendly != "" && label == "" {
		return fmt.Errorf("%q has nothing a FAT volume label can hold "+
			"(A-Z, 0-9, space and !#$%%&'()-@^_`{}~, 11 bytes)", friendly)
	}

	if err := nameLabelSet(root, label); err != nil {
		return fmt.Errorf("setting the volume label of %s: %w", root, err)
	}
	fmt.Fprintf(out, "volume      %s\n", root)
	fmt.Fprintf(out, "label       %s\n", labelText(label))
	if label != friendly {
		fmt.Fprintf(out, "            (Windows will show it as %s — a FAT label is 11 upper-case ASCII bytes)\n",
			labelText(label))
	}

	// Read back. A label that did not take is the failure this command
	// exists to catch: SetVolumeLabelW on a locked or read-only volume
	// can succeed and change nothing a user would ever notice.
	if got, err := nameLabelGet(root); err != nil {
		fmt.Fprintf(out, "verify      could not read the label back: %v\n", err)
	} else if got != label {
		return fmt.Errorf("the label read back as %q, not %q — the volume did not take it",
			got, label)
	} else {
		fmt.Fprintln(out, "verify      read back and matches")
	}

	if labelOnly {
		fmt.Fprintln(out, "name        (not stored — --label-only)")
		return nil
	}
	serial, serr := nameDiskSerial(root)
	if serial == "" {
		fmt.Fprintf(out, "name        %s (NOT stored: this volume reports no disk serial to file it under", friendly)
		if serr != nil {
			fmt.Fprintf(out, ": %v", serr)
		}
		fmt.Fprintln(out, ")")
		return nil
	}
	path, err := nameConfigPath()
	if err != nil {
		return err
	}
	if err := storeFriendlyName(path, serial, friendly); err != nil {
		return err
	}
	fmt.Fprintf(out, "serial      %s\n", serial)
	if friendly == "" {
		fmt.Fprintf(out, "name        (cleared in %s)\n", path)
	} else {
		fmt.Fprintf(out, "name        %s (in %s)\n", friendly, path)
	}
	return nil
}

func labelText(label string) string {
	if strings.TrimSpace(label) == "" {
		return "(no label)"
	}
	return label
}

// deviceNameText is the `name` line `core info` prints: the friendly
// name with the label it actually wrote in brackets. The two can differ
// and the honest answer shows both.
func deviceNameText(pod disk.IPod) string {
	vol := doctor.VolumeOf(pod, true)
	if vol == "" {
		return "(no mounted volume — the name lives on the music partition)"
	}
	root := disk.VolumeRoot(vol)
	label, err := nameLabelGet(root)
	if errors.Is(err, disk.ErrUnsupported) {
		// Reading a FAT label is implemented on Windows only, which is
		// the host the iPod is plugged into. Saying that is better than
		// printing a platform error where a name goes.
		return "(the iPod's name is read from the volume label, which core reads on Windows)"
	}
	if err != nil {
		return fmt.Sprintf("(could not read the label of %s: %v)", root, err)
	}
	friendly := lookupFriendlyName(pod.Disk.Serial)
	switch {
	case friendly != "" && label != "":
		return friendly + " (" + label + ")"
	case friendly != "":
		return friendly + " (the volume itself has no label)"
	case label != "":
		return label
	default:
		return "(unnamed — `core name " + vol + " \"Brandon's iPod\"` names it)"
	}
}

// --- the friendly name on disk ------------------------------------------

// The friendly name lives in core-app's own config.json, under
// `names: {"<disk serial>": "Brandon's iPod"}`. The CLI reads and
// writes that file directly rather than importing internal/app, which
// would link Gio into `core`; the round-trip below goes through
// map[string]json.RawMessage so every other key in the file — source,
// backup_dir, whatever the app grows next — survives a write from here
// untouched and undecoded.

func defaultNameConfigPath() (string, error) {
	dir, err := os.UserConfigDir()
	if err != nil {
		return "", fmt.Errorf("locating the user config directory: %w", err)
	}
	return filepath.Join(dir, "core", "config.json"), nil
}

// lookupFriendlyName is best-effort by design: a missing or unreadable
// config file means "no name remembered", never an error that stops a
// command whose real work is on the device.
func lookupFriendlyName(serial string) string {
	if serial == "" {
		return ""
	}
	path, err := nameConfigPath()
	if err != nil {
		return ""
	}
	names, err := readFriendlyNames(path)
	if err != nil {
		return ""
	}
	return names[serial]
}

func readFriendlyNames(path string) (map[string]string, error) {
	raw, err := readConfigObject(path)
	if err != nil {
		return nil, err
	}
	names := map[string]string{}
	if v, ok := raw["names"]; ok {
		if err := json.Unmarshal(v, &names); err != nil {
			return map[string]string{}, nil
		}
	}
	return names, nil
}

func readConfigObject(path string) (map[string]json.RawMessage, error) {
	b, err := os.ReadFile(path)
	if errors.Is(err, fs.ErrNotExist) {
		return map[string]json.RawMessage{}, nil
	}
	if err != nil {
		return nil, err
	}
	raw := map[string]json.RawMessage{}
	if err := json.Unmarshal(b, &raw); err != nil {
		// A corrupt config is the app's problem to overwrite, not a
		// reason for `core name` to refuse to name anything.
		return map[string]json.RawMessage{}, nil
	}
	return raw, nil
}

// storeFriendlyName files one name under one serial. An empty name
// removes the entry rather than storing "".
func storeFriendlyName(path, serial, friendly string) error {
	raw, err := readConfigObject(path)
	if err != nil {
		return fmt.Errorf("reading %s: %w", path, err)
	}
	names := map[string]string{}
	if v, ok := raw["names"]; ok {
		_ = json.Unmarshal(v, &names)
	}
	if friendly == "" {
		delete(names, serial)
	} else {
		names[serial] = friendly
	}
	enc, err := json.Marshal(names)
	if err != nil {
		return err
	}
	raw["names"] = enc
	body, err := json.MarshalIndent(raw, "", "  ")
	if err != nil {
		return err
	}
	body = append(body, '\n')
	if dir := filepath.Dir(path); dir != "" {
		if err := os.MkdirAll(dir, 0o755); err != nil {
			return fmt.Errorf("create %s: %w", dir, err)
		}
	}
	// Temp + rename, the same way app.SaveConfig writes it: the app may
	// be running while this command writes, and a half-written
	// config.json reads as a first run and loses the source folder.
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, body, 0o644); err != nil {
		return err
	}
	if err := os.Rename(tmp, path); err != nil {
		_ = os.Remove(tmp)
		return err
	}
	return nil
}
