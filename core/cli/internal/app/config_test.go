package app

import (
	"os"
	"path/filepath"
	"testing"
)

func TestConfigRoundTrip(t *testing.T) {
	path := filepath.Join(t.TempDir(), "sub", "config.json")
	want := Config{Source: `C:\Users\you\Music\MC`, BackupDir: `D:\backups`}
	if err := SaveConfig(path, want); err != nil {
		t.Fatalf("SaveConfig: %v", err)
	}
	got, err := LoadConfig(path)
	if err != nil {
		t.Fatalf("LoadConfig: %v", err)
	}
	if got != want {
		t.Errorf("round trip gave %+v, want %+v", got, want)
	}
	// No leftover temp file: an interrupted save must not leave
	// something that looks like a config.
	if _, err := os.Stat(path + ".tmp"); !os.IsNotExist(err) {
		t.Error("the temp file survived the save")
	}
}

// A missing file is the first run, and a corrupt one is a file the user
// cannot fix from inside a GUI that refuses to open. Both start empty.
func TestLoadConfigMissingAndCorrupt(t *testing.T) {
	dir := t.TempDir()
	got, err := LoadConfig(filepath.Join(dir, "nope.json"))
	if err != nil || got != (Config{}) {
		t.Errorf("a missing config gave %+v, %v", got, err)
	}

	bad := filepath.Join(dir, "bad.json")
	if err := os.WriteFile(bad, []byte("{not json"), 0o644); err != nil {
		t.Fatal(err)
	}
	got, err = LoadConfig(bad)
	if err != nil || got != (Config{}) {
		t.Errorf("a corrupt config gave %+v, %v", got, err)
	}
}

func TestConfigPathIsUnderTheUserConfigDir(t *testing.T) {
	p, err := ConfigPath()
	if err != nil {
		t.Skipf("no user config dir on this machine: %v", err)
	}
	if filepath.Base(p) != "config.json" || filepath.Base(filepath.Dir(p)) != "core" {
		t.Errorf("ConfigPath() = %q, want <user config>/core/config.json", p)
	}
}

func TestSaveConfigWithNoPathIsAnError(t *testing.T) {
	if err := SaveConfig("", Config{}); err == nil {
		t.Error("SaveConfig(\"\") succeeded")
	}
}
