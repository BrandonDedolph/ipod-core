package app

import (
	"errors"
	"strings"
	"testing"
)

// The picker commands are pure functions of GOOS and a PATH lookup, so
// all three platforms' wording is checked from here. A broken quoting
// or a wrong flag would otherwise only show up on the machine that has
// that desktop.

func look(have ...string) lookPath {
	set := map[string]bool{}
	for _, h := range have {
		set[h] = true
	}
	return func(name string) (string, error) {
		if set[name] {
			return "/usr/bin/" + name, nil
		}
		return "", errors.New("not found")
	}
}

func TestPickerCommandWindows(t *testing.T) {
	name, args, err := pickerCommand("windows", true, look())
	if err != nil {
		t.Fatal(err)
	}
	if name != "powershell.exe" {
		t.Errorf("name = %q", name)
	}
	joined := strings.Join(args, " ")
	if !strings.Contains(joined, "FolderBrowserDialog") || !strings.Contains(joined, "-NoProfile") {
		t.Errorf("the folder command is wrong: %q", joined)
	}
	_, args, _ = pickerCommand("windows", false, look())
	if !strings.Contains(strings.Join(args, " "), "OpenFileDialog") {
		t.Errorf("the file command is wrong: %q", args)
	}
}

func TestPickerCommandDarwin(t *testing.T) {
	name, args, err := pickerCommand("darwin", true, look())
	if err != nil {
		t.Fatal(err)
	}
	if name != "osascript" {
		t.Errorf("name = %q", name)
	}
	// POSIX path of: an alias is not something Go can open.
	if !strings.Contains(strings.Join(args, " "), "POSIX path of (choose folder") {
		t.Errorf("args = %q", args)
	}
}

func TestPickerCommandLinuxPrefersZenityThenKdialog(t *testing.T) {
	name, args, err := pickerCommand("linux", true, look("zenity", "kdialog"))
	if err != nil {
		t.Fatal(err)
	}
	if name != "/usr/bin/zenity" || !strings.Contains(strings.Join(args, " "), "--directory") {
		t.Errorf("zenity: %q %q", name, args)
	}

	name, args, err = pickerCommand("linux", true, look("kdialog"))
	if err != nil {
		t.Fatal(err)
	}
	if name != "/usr/bin/kdialog" || args[0] != "--getexistingdirectory" {
		t.Errorf("kdialog: %q %q", name, args)
	}

	name, _, err = pickerCommand("linux", false, look("kdialog"))
	if err != nil || name != "/usr/bin/kdialog" {
		t.Errorf("kdialog file: %q %v", name, err)
	}
}

// No picker is not an error the app hides: the Browse button goes grey
// and the card says to type the path.
func TestPickerCommandLinuxWithNeither(t *testing.T) {
	_, _, err := pickerCommand("linux", true, look())
	if !errors.Is(err, ErrNoPicker) {
		t.Fatalf("err = %v, want ErrNoPicker", err)
	}
	if !strings.Contains(err.Error(), "type the path") {
		t.Errorf("the error does not say what to do instead: %v", err)
	}
}
