// SPDX-License-Identifier: Apache-2.0

package eject

import (
	"fmt"
	"io"
	"time"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/devicefs"
)

// Eject stamps the iPod's clock and then hands the volume back to the OS.
//
// THE STAMP GOES FIRST, and eject is where it belongs. The device cannot be
// told the time over the cable (internal/devicefs/clock.go), so the host
// leaves the time in CORECFG.DAT for the next boot to pick up — which means
// the error the user sees on the device is the age of the stamp. Ejecting is
// the last thing anyone does before unplugging, and the boot that reads the
// stamp is usually seconds later, so this is the freshest stamp we can write.
// It is also the only moment we know the volume is still mounted and quiet.
//
// A failed stamp never fails the eject: the whole point of this command is
// getting the bytes safely off the host before a cable is pulled, and a wrong
// clock is a nuisance while an interrupted flush is a corrupt filesystem.
// Targets that are not mount points (`core eject /dev/sdb1`) have no
// filesystem here to patch and are skipped with one printed line.
//
// The per-OS bodies are ejectVolume in eject_<goos>.go. Putting the stamp in
// this shared wrapper rather than in each of them is deliberate: a new
// platform gets the clock for free instead of forgetting it.
func Eject(w io.Writer, target string) error {
	stampClock(w, target)
	return ejectVolume(w, target)
}

func stampClock(w io.Writer, target string) {
	stamped, did, err := devicefs.StampConfigTimeIfVolume(target, time.Now())
	switch {
	case err != nil:
		fmt.Fprintf(w, "clock not stamped: %v\n", err)
	case !did:
		fmt.Fprintf(w, "clock not stamped: %s is not a mount point (give one to set the iPod's clock)\n", target)
	default:
		fmt.Fprintf(w, "clock:     stamped %s (%s)\n",
			stamped.When.Format("2006-01-02 15:04"), offsetText(stamped.OffMin))
	}
}

// offsetText renders a minutes offset the way a user reads a time zone:
// "+02:00", "-08:00", "+05:30".
func offsetText(min int) string {
	sign := "+"
	if min < 0 {
		sign, min = "-", -min
	}
	return fmt.Sprintf("UTC%s%02d:%02d", sign, min/60, min%60)
}
