// SPDX-License-Identifier: Apache-2.0

package devicefs

import (
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"testing"
)

// The hazard this test exists for: the record EnsureConfig writes is the one
// the firmware loads forever after, because a valid record on disk always
// beats settings_defaults(). A value that disagrees with the C is therefore
// not a difference of opinion — it is a setting the device can never have.
// ResumeOnStartup already cost one debugging session on hardware that way
// (2026-07-27, tools/make_config.py:89-95).
//
// So: read settings_defaults() out of core/ui/settings.c and compare every
// field this package models. Skips outside the repo.
func TestDefaultSettingsMatchesFirmware(t *testing.T) {
	repo := repoRoot(t)
	if repo == "" {
		t.Skip("not inside the ipod_theme tree (set CORE_REPO)")
	}
	src, err := os.ReadFile(filepath.Join(repo, "core", "ui", "settings.c"))
	if err != nil {
		t.Skipf("core/ui/settings.c unreadable: %v", err)
	}

	body := regexp.MustCompile(`(?s)void\s+settings_defaults\s*\([^)]*\)\s*\{(.*?)\n\}`).
		FindSubmatch(src)
	if body == nil {
		t.Fatal("settings_defaults() not found in core/ui/settings.c — has it been renamed?")
	}
	assign := regexp.MustCompile(`s->(\w+)\s*=\s*([A-Za-z0-9_+-]+)\s*;`)
	got := map[string]string{}
	for _, m := range assign.FindAllSubmatch(body[1], -1) {
		got[string(m[1])] = string(m[2])
	}
	if len(got) == 0 {
		t.Fatal("settings_defaults() parsed to no assignments")
	}

	d := DefaultSettings()
	want := []struct {
		field string
		val   int
	}{
		{"shuffle", int(d.Shuffle)},
		{"repeat", int(d.Repeat)},
		{"resume_on_startup", int(d.ResumeOnStartup)},
		{"crossfade", int(d.Crossfade)},
		{"volume", int(d.Volume)},
		{"bass", int(d.Bass)},
		{"treble", int(d.Treble)},
		{"balance", int(d.Balance)},
		{"backlight_secs", int(d.BacklightSecs)},
		{"backlight_bright", int(d.BacklightBright)},
		{"theme", int(d.Theme)},
		{"clicker", int(d.Clicker)},
		{"volume_limit", int(d.VolumeLimit)},
		{"eq", int(d.EQ)},
	}
	for _, w := range want {
		raw, ok := got[w.field]
		if !ok {
			t.Errorf("settings_defaults() no longer assigns s->%s", w.field)
			continue
		}
		n, err := cValue(raw)
		if err != nil {
			t.Errorf("s->%s = %s: %v", w.field, raw, err)
			continue
		}
		if n != w.val {
			t.Errorf("DefaultSettings().%s = %d, but settings_defaults() sets s->%s = %s (%d)",
				w.field, w.val, w.field, raw, n)
		}
	}

	// The resume locator and queue context must be zero in the C too: this
	// package writes them as zeros, so a non-zero default there would mean
	// a freshly created file disagrees with a freshly reset one.
	for _, f := range []string{
		"resume_hash", "resume_secs", "resume_total", "resume_flags",
		"resume_qidx", "resume_seed", "resume_order_seed", "resume_order_keep",
		"resume_ctx_hash",
	} {
		raw, ok := got[f]
		if !ok {
			continue // a field this build does not have; not this test's business
		}
		if n, err := cValue(raw); err != nil || n != 0 {
			t.Errorf("settings_defaults() sets s->%s = %s; this package writes 0", f, raw)
		}
	}
	if raw, ok := got["resume_kind"]; ok && raw != "RESUME_KIND_NONE" {
		if n, err := cValue(raw); err != nil || n != 0 {
			t.Errorf("settings_defaults() sets s->resume_kind = %s; this package writes 0", raw)
		}
	}
}

// cValue resolves the right-hand side of one settings_defaults() assignment:
// an integer literal, or one of the enum names the firmware uses there.
func cValue(s string) (int, error) {
	switch s {
	case "REPEAT_OFF", "RESUME_KIND_NONE", "EQ_OFF", "SHUFFLE_OFF":
		return 0, nil
	case "REPEAT_ALL", "SHUFFLE_SONGS":
		return 1, nil
	case "REPEAT_ONE", "SHUFFLE_ALBUMS":
		return 2, nil
	}
	return strconv.Atoi(s)
}
