package app

import (
	"errors"
	"path/filepath"
	"strings"
	"testing"
)

// The header's whole rule, in one table. "Apple iPod" is the SCSI model
// string; it is not a name and must never be one.
func TestDeviceDisplayName(t *testing.T) {
	cases := []struct {
		name string
		dev  Device
		want string
	}{
		{"the friendly name wins", Device{Name: "Brandon's iPod", Label: "BRANDON'S I"}, "Brandon's iPod"},
		{"then the label, as stored", Device{Label: "BRANDON'S I"}, "BRANDON'S I"},
		{"then the fallback", Device{}, DefaultDeviceName},
		{"a blank name does not count", Device{Name: "   ", Label: "IPOD"}, "IPOD"},
		{"a blank label does not count", Device{Label: "   "}, DefaultDeviceName},
		{"the model is not a name", Device{Model: "iPod Video 5.5G 80 GB"}, DefaultDeviceName},
	}
	for _, c := range cases {
		if got := c.dev.DisplayName(); got != c.want {
			t.Errorf("%s: DisplayName = %q, want %q", c.name, got, c.want)
		}
		if strings.Contains(c.dev.DisplayName(), "Apple") {
			t.Errorf("%s: the name says Apple: %q", c.name, c.dev.DisplayName())
		}
	}
}

// newNamedTestUI is newTestUI with a config file, because the rename is
// half a device write and half a settings write and both halves matter.
func newNamedTestUI(t *testing.T, f *fakeBackend) (*UI, string) {
	t.Helper()
	path := filepath.Join(t.TempDir(), "config.json")
	u := NewUI(Options{Backend: f, ConfigPath: path})
	u.st.Device = Device{Found: true, Path: `\\.\PhysicalDrive2`, Volume: `D:\`,
		Serial: "000A2700168F1E3C", Tested: true, Model: "iPod Video 5.5G 80 GB"}
	return u, path
}

// The load-bearing one: Save calls the backend exactly once, with what
// was typed; the label that comes back is what the header shows beside
// the name; and the friendly name lands in config.json under the disk's
// serial, where the next launch will find it.
func TestRenameWritesTheVolumeAndRemembersTheName(t *testing.T) {
	f := &fakeBackend{}
	u, cfgPath := newNamedTestUI(t, f)

	u.editingName = true
	u.nameEd.SetText("Brandon's iPod")
	u.startRename(strings.TrimSpace(u.nameEd.Text()))
	settle(u)

	if len(f.renameCalls) != 1 {
		t.Fatalf("Rename calls = %+v, want exactly one", f.renameCalls)
	}
	if f.renameCalls[0].volume != `D:\` || f.renameCalls[0].name != "Brandon's iPod" {
		t.Errorf("Rename called with %+v", f.renameCalls[0])
	}
	if u.st.Device.Name != "Brandon's iPod" || u.st.Device.Label != "BRANDON'S I" {
		t.Errorf("after the rename: name=%q label=%q", u.st.Device.Name, u.st.Device.Label)
	}
	if got := u.st.Device.DisplayName(); got != "Brandon's iPod" {
		t.Errorf("the header shows %q", got)
	}
	cfg, err := LoadConfig(cfgPath)
	if err != nil {
		t.Fatalf("LoadConfig: %v", err)
	}
	if cfg.NameFor("000A2700168F1E3C") != "Brandon's iPod" {
		t.Errorf("config.json did not keep the name: %+v", cfg.Names)
	}
	if !strings.Contains(strings.Join(u.st.Log, "\n"), "BRANDON'S I") {
		t.Errorf("the log does not say what Windows will show:\n%s", strings.Join(u.st.Log, "\n"))
	}
}

// A refresh must not lose the name: the label comes off the volume, the
// friendly name comes out of config.json, and drain is where they meet.
func TestRefreshResolvesTheFriendlyNameFromTheConfig(t *testing.T) {
	f := &fakeBackend{dev: Device{Found: true, Serial: "S1", Volume: `D:\`, Label: "BRANDON'S I"}}
	u := NewUI(Options{Backend: f, Config: Config{Names: map[string]string{"S1": "Brandon's iPod"}}})
	u.startRefresh()
	settle(u)
	if u.st.Device.Name != "Brandon's iPod" || u.st.Device.Label != "BRANDON'S I" {
		t.Fatalf("after a refresh: name=%q label=%q", u.st.Device.Name, u.st.Device.Label)
	}
	if got := u.st.Device.DisplayName(); got != "Brandon's iPod" {
		t.Errorf("DisplayName = %q", got)
	}
}

// An unknown iPod shows its label, not the SCSI string and not a blank.
func TestRefreshOfAnUnknownIPodShowsTheLabel(t *testing.T) {
	f := &fakeBackend{dev: Device{Found: true, Serial: "S9", Volume: `D:\`, Label: "IPOD"}}
	u := NewUI(Options{Backend: f, Config: Config{Names: map[string]string{"S1": "Brandon's iPod"}}})
	u.startRefresh()
	settle(u)
	if got := u.st.Device.DisplayName(); got != "IPOD" {
		t.Errorf("DisplayName = %q, want the volume label", got)
	}
}

// An empty name is "no name": the label is cleared and the config entry
// is removed rather than remembered as "".
func TestRenameToNothingClearsBothHalves(t *testing.T) {
	f := &fakeBackend{}
	u, cfgPath := newNamedTestUI(t, f)
	u.cfg.SetName("000A2700168F1E3C", "Brandon's iPod")
	u.st.Device.Name, u.st.Device.Label = "Brandon's iPod", "BRANDON'S I"

	u.startRename("")
	settle(u)

	if len(f.renameCalls) != 1 || f.renameCalls[0].name != "" {
		t.Fatalf("Rename calls = %+v", f.renameCalls)
	}
	if u.st.Device.Name != "" || u.st.Device.Label != "" {
		t.Errorf("after clearing: name=%q label=%q", u.st.Device.Name, u.st.Device.Label)
	}
	if got := u.st.Device.DisplayName(); got != DefaultDeviceName {
		t.Errorf("DisplayName = %q, want the fallback", got)
	}
	cfg, err := LoadConfig(cfgPath)
	if err != nil {
		t.Fatalf("LoadConfig: %v", err)
	}
	if _, still := cfg.Names["000A2700168F1E3C"]; still {
		t.Errorf("the cleared name is still in the config: %+v", cfg.Names)
	}
}

// A failed rename leaves the model alone — the header must not show a
// name the volume refused.
func TestRenameFailureKeepsTheOldName(t *testing.T) {
	f := &fakeBackend{renameErr: errors.New("the label read back as IPOD, not BRANDON'S I")}
	u, _ := newNamedTestUI(t, f)
	u.st.Device.Name, u.st.Device.Label = "Old", "OLD"

	u.startRename("Brandon's iPod")
	settle(u)

	if u.st.Device.Name != "Old" || u.st.Device.Label != "OLD" {
		t.Errorf("a failed rename changed the model: name=%q label=%q", u.st.Device.Name, u.st.Device.Label)
	}
	if u.st.Job == nil || !u.st.Job.Failed {
		t.Errorf("the job did not fail: %+v", u.st.Job)
	}
}

// Renaming needs a volume. Without one the job never starts: a rename
// of "" would be a rename of whatever drive the OS thinks that is.
func TestRenameWithoutAVolumeRefuses(t *testing.T) {
	f := &fakeBackend{}
	u := NewUI(Options{Backend: f})
	u.st.Device = Device{Found: true}
	u.startRename("Brandon's iPod")
	settle(u)
	if len(f.renameCalls) != 0 {
		t.Fatalf("Rename was called with no volume: %+v", f.renameCalls)
	}
	if !strings.Contains(strings.Join(u.st.Log, "\n"), "Refresh") {
		t.Errorf("the log does not say what to do:\n%s", strings.Join(u.st.Log, "\n"))
	}
}

// The edit is taken away while a job runs: a rename during a flash
// would be a rename of a volume the flasher has dismounted.
func TestTheNameEditIsClosedWhileAJobRuns(t *testing.T) {
	f := &fakeBackend{block: make(chan struct{})}
	u, _ := newNamedTestUI(t, f)
	u.st.Source = "/src"
	u.sourceEd.SetText("/src")

	u.startSync(JobSync)
	u.editingName = true
	u.nameEd.SetText("Brandon's iPod")

	gtx := newTestContext(MinWidth, MinHeight)
	u.nameEvents(gtx, u.st.Busy())
	if u.editingName {
		t.Error("the name field stayed open while a job was running")
	}
	close(f.block)
	settle(u)
	if len(f.renameCalls) != 0 {
		t.Errorf("a rename ran during a job: %+v", f.renameCalls)
	}
}

// Cancel puts the field back to what the device says, and calls
// nothing.
func TestCancelDropsTheEdit(t *testing.T) {
	f := &fakeBackend{}
	u, _ := newNamedTestUI(t, f)
	u.st.Device.Name = "Brandon's iPod"
	u.editingName = true
	u.nameEd.SetText("Something else")

	gtx := newTestContext(MinWidth, MinHeight)
	u.nameCancelBtn.Click()
	u.nameEvents(gtx, false)

	if u.editingName {
		t.Error("Cancel left the field open")
	}
	if got := u.nameEd.Text(); got != "Brandon's iPod" {
		t.Errorf("Cancel left %q in the field", got)
	}
	if len(f.renameCalls) != 0 {
		t.Errorf("Cancel called the backend: %+v", f.renameCalls)
	}
}

// Save is one click and one call: the frame after it must not start a
// second rename.
func TestSaveCallsTheBackendOnce(t *testing.T) {
	f := &fakeBackend{}
	u, _ := newNamedTestUI(t, f)
	u.editingName = true
	u.nameEd.SetText("Brandon's iPod")

	gtx := newTestContext(MinWidth, MinHeight)
	u.nameSaveBtn.Click()
	u.nameEvents(gtx, false)
	settle(u)
	u.nameEvents(gtx, false)
	u.nameEvents(gtx, false)
	settle(u)

	if len(f.renameCalls) != 1 {
		t.Fatalf("Rename calls = %+v, want exactly one", f.renameCalls)
	}
	if u.editingName {
		t.Error("Save left the field open")
	}
}

// The sentence under the field, which is the whole reason the edit is
// not just a text box.
func TestNamePreview(t *testing.T) {
	u := NewUI(Options{Backend: &fakeBackend{}})
	cases := map[string]string{
		"Brandon's iPod": "Windows will show it as BRANDON'S I",
		"IPOD":           "Windows will show it as IPOD.",
		"":               "clears it",
		"***":            "Nothing in that name fits",
	}
	for typed, want := range cases {
		u.nameEd.SetText(typed)
		if got := u.namePreview(); !strings.Contains(got, want) {
			t.Errorf("namePreview(%q) = %q, want it to mention %q", typed, got, want)
		}
	}
}

// The card must lay out in both states, at the size that matters.
func TestTheNameRowLaysOutOpenAndClosed(t *testing.T) {
	for _, editing := range []bool{false, true} {
		u := NewUI(Options{Backend: &fakeBackend{}})
		u.SetState(DemoState())
		u.editingName = editing
		gtx := newTestContext(MinWidth, MinHeight)
		if got := u.Layout(gtx).Size; got.X != MinWidth || got.Y != MinHeight {
			t.Errorf("editing=%v laid out %v", editing, got)
		}
	}
}
