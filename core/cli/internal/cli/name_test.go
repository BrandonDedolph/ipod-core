package cli

import (
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/disk"
)

// fakeVolume stands in for the two kernel32 calls. The command's own
// behaviour — what it writes, what it refuses, what it prints — is the
// thing under test, and none of it needs a real volume; the calls
// themselves are proved against C:\ in internal/disk and against the
// iPod by hand.
type fakeVolume struct {
	label   string
	serial  string
	sets    []string
	setErr  error
	getErr  error
	readsAs string // what the read-back returns, when it must differ
}

// install points the command at this fake and at a config file in a
// temp dir, and puts everything back afterwards.
func (f *fakeVolume) install(t *testing.T) string {
	t.Helper()
	cfg := filepath.Join(t.TempDir(), "core", "config.json")
	oldGet, oldSet, oldSerial, oldPath := nameLabelGet, nameLabelSet, nameDiskSerial, nameConfigPath
	t.Cleanup(func() {
		nameLabelGet, nameLabelSet, nameDiskSerial, nameConfigPath = oldGet, oldSet, oldSerial, oldPath
	})
	nameLabelGet = func(root string) (string, error) {
		if f.getErr != nil {
			return "", f.getErr
		}
		if f.readsAs != "" {
			return f.readsAs, nil
		}
		return f.label, nil
	}
	nameLabelSet = func(root, label string) error {
		if f.setErr != nil {
			return f.setErr
		}
		f.sets = append(f.sets, label)
		f.label = label
		return nil
	}
	nameDiskSerial = func(root string) (string, error) { return f.serial, nil }
	nameConfigPath = func() (string, error) { return cfg, nil }
	return cfg
}

func writeConfig(t *testing.T, path string, body string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(body), 0o644); err != nil {
		t.Fatal(err)
	}
}

func readConfig(t *testing.T, path string) map[string]any {
	t.Helper()
	b, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read %s: %v", path, err)
	}
	var m map[string]any
	if err := json.Unmarshal(b, &m); err != nil {
		t.Fatalf("parse %s: %v\n%s", path, err, b)
	}
	return m
}

// `core name D:` with no name writes nothing and prints the three
// facts: what FAT holds, which disk it is, and what the app calls it.
func TestNameShowsTheLabelTheSerialAndTheFriendlyName(t *testing.T) {
	f := &fakeVolume{label: "BRANDON'S I", serial: "000A2700168F1E3C"}
	cfg := f.install(t)
	writeConfig(t, cfg, `{"source":"C:\\Music","names":{"000A2700168F1E3C":"Brandon's iPod"}}`)

	out, _, err := runCore(t, "name", "D:")
	if err != nil {
		t.Fatalf("name D:: %v", err)
	}
	for _, want := range []string{`D:\`, "BRANDON'S I", "000A2700168F1E3C", "Brandon's iPod"} {
		if !strings.Contains(out, want) {
			t.Errorf("output does not mention %q:\n%s", want, out)
		}
	}
	if len(f.sets) != 0 {
		t.Errorf("a read wrote the label: %v", f.sets)
	}
}

// With no name remembered, the read still works and says how to set one
// rather than printing an empty field.
func TestNameShowsHowToSetOneWhenThereIsNoFriendlyName(t *testing.T) {
	f := &fakeVolume{label: "", serial: "S1"}
	f.install(t)
	out, _, err := runCore(t, "name", "D:")
	if err != nil {
		t.Fatalf("name D:: %v", err)
	}
	if !strings.Contains(out, "(no label)") || !strings.Contains(out, "core name D:") {
		t.Errorf("unhelpful output for an unnamed volume:\n%s", out)
	}
}

// The load-bearing one: the label written is LegalLabel of what the
// user typed, the truncation is said out loud, and the friendly name is
// filed under the disk serial without disturbing the rest of the file.
func TestNameSetWritesTheLegalLabelAndRemembersTheFriendlyName(t *testing.T) {
	f := &fakeVolume{serial: "000A2700168F1E3C"}
	cfg := f.install(t)
	writeConfig(t, cfg, `{"source":"C:\\Music","backup_dir":"C:\\b"}`)

	out, _, err := runCore(t, "name", "D:", "Brandon's iPod")
	if err != nil {
		t.Fatalf("name D: <name>: %v", err)
	}
	if len(f.sets) != 1 || f.sets[0] != "BRANDON'S I" {
		t.Fatalf("SetVolumeLabel calls = %v, want one call with BRANDON'S I", f.sets)
	}
	if !strings.Contains(out, "Windows will show it as BRANDON'S I") {
		t.Errorf("the truncation was not explained:\n%s", out)
	}
	if !strings.Contains(out, "read back and matches") {
		t.Errorf("the label was not verified by read-back:\n%s", out)
	}

	m := readConfig(t, cfg)
	if m["source"] != `C:\Music` || m["backup_dir"] != `C:\b` {
		t.Errorf("writing a name disturbed the rest of config.json: %v", m)
	}
	names, _ := m["names"].(map[string]any)
	if names["000A2700168F1E3C"] != "Brandon's iPod" {
		t.Errorf("names = %v, want the friendly name under the serial", m["names"])
	}
}

func TestNameLabelOnlyLeavesTheConfigAlone(t *testing.T) {
	f := &fakeVolume{serial: "S1"}
	cfg := f.install(t)

	out, _, err := runCore(t, "name", "D:", "Brandon's iPod", "--label-only")
	if err != nil {
		t.Fatalf("name --label-only: %v", err)
	}
	if len(f.sets) != 1 || f.sets[0] != "BRANDON'S I" {
		t.Fatalf("SetVolumeLabel calls = %v", f.sets)
	}
	if !strings.Contains(out, "--label-only") {
		t.Errorf("the output does not say the name was not stored:\n%s", out)
	}
	if _, err := os.Stat(cfg); !errors.Is(err, os.ErrNotExist) {
		t.Errorf("--label-only wrote %s", cfg)
	}
}

// An empty name is "this iPod has no name": the label is cleared and
// the config entry is removed rather than set to "".
func TestNameWithAnEmptyNameClearsBoth(t *testing.T) {
	f := &fakeVolume{label: "BRANDON'S I", serial: "S1"}
	cfg := f.install(t)
	writeConfig(t, cfg, `{"names":{"S1":"Brandon's iPod","S2":"Spare"}}`)

	if _, _, err := runCore(t, "name", "D:", ""); err != nil {
		t.Fatalf("name D: \"\": %v", err)
	}
	if len(f.sets) != 1 || f.sets[0] != "" {
		t.Fatalf("SetVolumeLabel calls = %q, want one call with the empty label", f.sets)
	}
	names, _ := readConfig(t, cfg)["names"].(map[string]any)
	if _, still := names["S1"]; still {
		t.Errorf("the friendly name survived the clear: %v", names)
	}
	if names["S2"] != "Spare" {
		t.Errorf("clearing one name removed another: %v", names)
	}
}

// A name with nothing legal in it must fail before the volume is
// touched: writing an empty label would silently un-name the iPod.
func TestNameRefusesANameWithNothingLegalInIt(t *testing.T) {
	f := &fakeVolume{serial: "S1"}
	f.install(t)
	_, _, err := runCore(t, "name", "D:", "***")
	if err == nil {
		t.Fatal("a name of *** was accepted")
	}
	if len(f.sets) != 0 {
		t.Errorf("the volume was written anyway: %v", f.sets)
	}
	if !strings.Contains(err.Error(), "FAT volume label") {
		t.Errorf("the refusal does not explain the rule: %v", err)
	}
}

// The read-back is the point of the write path: a label that did not
// take is a failure, not a success with a surprise in Explorer.
func TestNameFailsWhenTheLabelDoesNotReadBack(t *testing.T) {
	f := &fakeVolume{serial: "S1", readsAs: "SOMETHINGEL"}
	f.install(t)
	_, _, err := runCore(t, "name", "D:", "Brandon's iPod")
	if err == nil {
		t.Fatal("a label that read back differently was reported as success")
	}
	if !strings.Contains(err.Error(), "did not take") {
		t.Errorf("unclear failure: %v", err)
	}
}

// Without a disk serial there is nothing to key the friendly name to.
// The label still goes on, and the output says the other half did not.
func TestNameWithoutADiskSerialStillWritesTheLabel(t *testing.T) {
	f := &fakeVolume{}
	cfg := f.install(t)
	out, _, err := runCore(t, "name", "D:", "Brandon's iPod")
	if err != nil {
		t.Fatalf("name: %v", err)
	}
	if len(f.sets) != 1 {
		t.Fatalf("SetVolumeLabel calls = %v", f.sets)
	}
	if !strings.Contains(out, "NOT stored") {
		t.Errorf("the output hides that the name was not remembered:\n%s", out)
	}
	if _, err := os.Stat(cfg); !errors.Is(err, os.ErrNotExist) {
		t.Errorf("a name was filed under an empty serial in %s", cfg)
	}
}

// The `name` line `core info` prints, over the three resolutions.
func TestDeviceNameText(t *testing.T) {
	pod := disk.IPod{Disk: disk.Disk{Serial: "S1", Volumes: []string{"D:"}, MountPoints: []string{`D:\`}}}

	f := &fakeVolume{label: "BRANDON'S I", serial: "S1"}
	cfg := f.install(t)
	writeConfig(t, cfg, `{"names":{"S1":"Brandon's iPod"}}`)
	if got := deviceNameText(pod); got != "Brandon's iPod (BRANDON'S I)" {
		t.Errorf("with both: %q", got)
	}

	writeConfig(t, cfg, `{"names":{}}`)
	if got := deviceNameText(pod); got != "BRANDON'S I" {
		t.Errorf("with a label only: %q", got)
	}

	f.label = ""
	if got := deviceNameText(pod); !strings.Contains(got, "unnamed") {
		t.Errorf("with neither: %q", got)
	}

	nolabel := disk.IPod{Disk: disk.Disk{Serial: "S1"}}
	if got := deviceNameText(nolabel); !strings.Contains(got, "no mounted volume") {
		t.Errorf("with no volume: %q", got)
	}
}

// A corrupt config.json must not stop the command: the app overwrites
// it on its next save, and refusing to name an iPod because a settings
// file has a stray comma in it would be absurd.
func TestNameSurvivesACorruptConfig(t *testing.T) {
	f := &fakeVolume{label: "OLD", serial: "S1"}
	cfg := f.install(t)
	writeConfig(t, cfg, "{not json")

	if _, _, err := runCore(t, "name", "D:"); err != nil {
		t.Fatalf("read with a corrupt config: %v", err)
	}
	if _, _, err := runCore(t, "name", "D:", "Brandon's iPod"); err != nil {
		t.Fatalf("write with a corrupt config: %v", err)
	}
	names, _ := readConfig(t, cfg)["names"].(map[string]any)
	if names["S1"] != "Brandon's iPod" {
		t.Errorf("the name was not stored over the corrupt file: %v", names)
	}
}
