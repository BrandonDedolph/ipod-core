package app

import (
	"encoding/json"
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
)

// Config is what the app remembers between runs. It is deliberately
// two fields: everything else on screen is read from the device or from
// GitHub, and a settings file that caches facts about hardware is a
// settings file that will one day disagree with the hardware.
type Config struct {
	// Source is the music folder the Music card starts with.
	Source string `json:"source"`
	// BackupDir is where whole-partition backups go. Empty means
	// flasher.DefaultBackupDir, which is what the CLI uses — the two
	// must agree, or `core flash --from-backup` would look in the
	// wrong place for a backup the app took.
	BackupDir string `json:"backup_dir"`
}

// ConfigPath is <UserConfigDir>/core/config.json:
// %AppData%\core\config.json on Windows, ~/.config/core/config.json on
// Linux, ~/Library/Application Support/core/config.json on macOS. The
// same parent directory internal/ghrelease caches downloads in and
// internal/flasher writes backups to.
func ConfigPath() (string, error) {
	dir, err := os.UserConfigDir()
	if err != nil {
		return "", fmt.Errorf("app: locating the user config directory: %w", err)
	}
	return filepath.Join(dir, "core", "config.json"), nil
}

// LoadConfig reads the file. A missing file is not an error — it is the
// first run — and neither is a corrupt one: the app starts with empty
// fields and overwrites it on the next save, because refusing to open
// over a bad JSON file would leave the user with a GUI that cannot be
// used to fix it.
func LoadConfig(path string) (Config, error) {
	b, err := os.ReadFile(path)
	if errors.Is(err, fs.ErrNotExist) {
		return Config{}, nil
	}
	if err != nil {
		return Config{}, err
	}
	var c Config
	if err := json.Unmarshal(b, &c); err != nil {
		return Config{}, nil
	}
	return c, nil
}

// SaveConfig writes the file, creating its directory. It writes to a
// temp name and renames, so a crash mid-write does not leave a
// half-written file that LoadConfig would silently treat as a first
// run.
func SaveConfig(path string, c Config) error {
	if path == "" {
		return errors.New("app: no config path")
	}
	if dir := filepath.Dir(path); dir != "" {
		if err := os.MkdirAll(dir, 0o755); err != nil {
			return fmt.Errorf("app: create %s: %w", dir, err)
		}
	}
	b, err := json.MarshalIndent(c, "", "  ")
	if err != nil {
		return err
	}
	b = append(b, '\n')
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, b, 0o644); err != nil {
		return err
	}
	if err := os.Rename(tmp, path); err != nil {
		_ = os.Remove(tmp)
		return err
	}
	return nil
}
