// SPDX-License-Identifier: Apache-2.0

package eject

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/devicefs"
)

// Eject is the last thing anyone does before unplugging, which makes it the
// freshest clock the device can be given — and the desktop app's Eject button
// runs this same wrapper (internal/app/backend.go), so this file is where that
// button's stamp is covered. ejectVolume itself is per-OS and talks to udisks /
// IOCTLs / diskutil, so the tests below drive the half that is ours.

func TestStampClockWritesAndNarrates(t *testing.T) {
	dir := t.TempDir()
	if _, err := devicefs.EnsureConfig(dir); err != nil {
		t.Fatal(err)
	}
	before, err := os.ReadFile(filepath.Join(dir, devicefs.ConfigName))
	if err != nil {
		t.Fatal(err)
	}

	var out bytes.Buffer
	stampClock(&out, dir)

	if !strings.Contains(out.String(), "clock:") || !strings.Contains(out.String(), "UTC") {
		t.Errorf("the eject did not say what it did with the clock:\n%s", out.String())
	}
	after, err := os.ReadFile(filepath.Join(dir, devicefs.ConfigName))
	if err != nil {
		t.Fatal(err)
	}
	if len(after) != len(before) {
		t.Fatalf("the file changed size: %d -> %d", len(before), len(after))
	}
	if !bytes.Equal(before[:devicefs.ConfigSlotBytes], after[:devicefs.ConfigSlotBytes]) {
		t.Error("the stamp rewrote the record the device would load")
	}
	ts, ok := devicefs.DecodeConfigTime(after[devicefs.ConfigSlotBytes:])
	if !ok || !ts.Pending() {
		t.Errorf("no pending stamp in the other slot: %+v (ok=%v)", ts, ok)
	}
	if d := time.Since(time.Unix(int64(ts.HostEpoch), 0)); d > time.Minute || d < -time.Minute {
		t.Errorf("the stamp is %v away from now", d)
	}
}

// A device node is not a mount point: there is no filesystem here to patch, and
// the user is told rather than left wondering why their clock is wrong.
func TestStampClockSkipsANonVolume(t *testing.T) {
	var out bytes.Buffer
	stampClock(&out, filepath.Join(t.TempDir(), "sdb1"))
	if !strings.Contains(out.String(), "not a mount point") {
		t.Errorf("a non-volume target was not explained:\n%s", out.String())
	}
}

// A volume with no CORECFG.DAT (never synced) must not stop an eject: the
// whole point of the command is getting the bytes off safely.
func TestStampClockOnAnUnpreparedVolume(t *testing.T) {
	var out bytes.Buffer
	stampClock(&out, t.TempDir())
	if !strings.Contains(out.String(), "clock not stamped") {
		t.Errorf("a volume with no config did not explain itself:\n%s", out.String())
	}
}
