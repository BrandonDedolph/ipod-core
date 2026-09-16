package disk

import (
	"errors"
	"runtime"
	"strings"
	"testing"
)

// TestLegalLabel is the naming rule, written out. Every row here is
// quoted in LegalLabel's doc comment, which is where a reader looks
// first, so the two have to agree.
func TestLegalLabel(t *testing.T) {
	cases := []struct {
		name, in, want string
	}{
		{"the plan's example", "Brandon's iPod", "BRANDON'S I"},
		{"accents are dropped, not folded", "Musique été", "MUSIQUE T"},
		{"lower case is raised", "ipod", "IPOD"},
		{"a run of spaces collapses", "my  ipod", "MY IPOD"},
		{"leading and trailing space goes", "  ipod  ", "IPOD"},
		{"empty stays empty", "", ""},
		{"nothing legal survives", "***", ""},
		{"reserved punctuation is dropped", "a.b,c:d/e\\f|g+h=i<j>k[l]m\"n*o?p", "ABCDEFGHIJK"},
		{"allowed punctuation is kept", "!#$%&'()-@^", "!#$%&'()-@^"},
		{"digits are kept", "iPod 80GB", "IPOD 80GB"},
		{"exactly eleven is untouched", "ELEVENCHAR1", "ELEVENCHAR1"},
		{"twelve is cut to eleven", "ABCDEFGHIJKL", "ABCDEFGHIJK"},
		{"a cut that lands on a space trims it", "ABCDEFGHIJ KL", "ABCDEFGHIJ"},
		{"unicode is dropped whole", "音楽", ""},
		{"emoji too", "iPod 🎧", "IPOD"},
		{"tabs and newlines are dropped", "a\tb\nc", "ABC"},
		{"an all-space name is no name", "     ", ""},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			got := LegalLabel(c.in)
			if got != c.want {
				t.Errorf("LegalLabel(%q) = %q, want %q", c.in, got, c.want)
			}
			if len(got) > MaxLabel {
				t.Errorf("LegalLabel(%q) = %q, %d bytes; the field holds %d",
					c.in, got, len(got), MaxLabel)
			}
			// Whatever comes out must be storable, or the rule is
			// not a rule.
			if got != "" {
				if err := ValidLabel(got); err != nil {
					t.Errorf("LegalLabel(%q) = %q, which ValidLabel refuses: %v", c.in, got, err)
				}
			}
		})
	}
}

// LegalLabel is a normalisation, so running it twice must change
// nothing the second time — the app does exactly that when it shows the
// legal label before applying it and then applies it.
func TestLegalLabelIsIdempotent(t *testing.T) {
	for _, s := range []string{"Brandon's iPod", "Musique été", "my  ipod", "***", "", "iPod 80GB"} {
		once := LegalLabel(s)
		if twice := LegalLabel(once); twice != once {
			t.Errorf("LegalLabel(%q) = %q, but LegalLabel of that = %q", s, once, twice)
		}
	}
}

func TestValidLabel(t *testing.T) {
	if err := ValidLabel("BRANDON'S I"); err != nil {
		t.Errorf("ValidLabel of a legal label: %v", err)
	}
	if err := ValidLabel(""); !errors.Is(err, ErrLabelEmpty) {
		t.Errorf("ValidLabel(\"\") = %v, want ErrLabelEmpty", err)
	}
	if err := ValidLabel("   "); !errors.Is(err, ErrLabelEmpty) {
		t.Errorf("ValidLabel(spaces) = %v, want ErrLabelEmpty", err)
	}
	if err := ValidLabel("TWELVECHARS1"); err == nil {
		t.Error("ValidLabel accepted 12 bytes")
	}
	for _, bad := range []string{"my ipod", "IPOD.", "IPOD/D", "MUSIQUE ÉT"} {
		if err := ValidLabel(bad); err == nil {
			t.Errorf("ValidLabel(%q) accepted it", bad)
		}
	}
}

func TestVolumeRoot(t *testing.T) {
	cases := map[string]string{
		"D:":            `D:\`,
		`D:\`:           `D:\`,
		`C:\mnt\ipod`:   `C:\mnt\ipod\`,
		"/media/IPOD":   "/media/IPOD",
		"/media/IPOD/":  "/media/IPOD/",
		"":              "",
		`\\.\D:`:        `\\.\D:`,
		`C:\mnt\ipod\`:  `C:\mnt\ipod\`,
		"/Volumes/IPOD": "/Volumes/IPOD",
	}
	for in, want := range cases {
		if got := VolumeRoot(in); got != want {
			t.Errorf("VolumeRoot(%q) = %q, want %q", in, got, want)
		}
	}
}

// TestVolumeLabelOffWindows pins the stub: the app and the CLI must get
// a refusal they can print, not a panic or a lie.
func TestVolumeLabelUnsupportedOffWindows(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("this is the off-Windows stub")
	}
	if _, err := VolumeLabel(`D:\`); !errors.Is(err, ErrUnsupported) {
		t.Errorf("VolumeLabel off Windows = %v, want ErrUnsupported", err)
	}
	if err := SetVolumeLabel(`D:\`, "IPOD"); !errors.Is(err, ErrUnsupported) {
		t.Errorf("SetVolumeLabel off Windows = %v, want ErrUnsupported", err)
	}
	if _, err := VolumeDiskSerial(`D:\`); !errors.Is(err, ErrUnsupported) {
		t.Errorf("VolumeDiskSerial off Windows = %v, want ErrUnsupported", err)
	}
}

// TestVolumeLabelOfSystemDriveOnWindows is the one test that talks to
// the real API. It reads C:\ — a volume that exists on every Windows
// machine, and one this never writes to — and asserts only that the
// call succeeds and hands back something a FAT label could hold or an
// NTFS label plainly does. It never touches the iPod, and it never
// calls SetVolumeLabel: the write side is proved on the device by hand,
// because a test that renames a volume on the developer's machine is a
// test nobody should run twice.
func TestVolumeLabelOfSystemDriveOnWindows(t *testing.T) {
	if runtime.GOOS != "windows" {
		t.Skip("GetVolumeInformationW is Windows-only")
	}
	label, err := VolumeLabel(`C:\`)
	if err != nil {
		t.Fatalf("VolumeLabel(C:\\): %v", err)
	}
	t.Logf("C:\\ label = %q", label)
	if strings.ContainsAny(label, "\x00") {
		t.Errorf("the label came back with a NUL in it: %q", label)
	}
}
