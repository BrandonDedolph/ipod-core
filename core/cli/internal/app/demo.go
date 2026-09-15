package app

import "strings"

// The canned states.
//
// They are not test fixtures that happen to be exported: `core-app
// --screenshot x.png --state demo` is how this layout is reviewed from
// a shell with no display, and how the snapshot test has something with
// long strings, big numbers and a running job in it to overflow. Every
// number here is a real one from the bench device and the 928-track MC
// library, because a mock with "Artist 1" and 3 songs is a mock that
// never finds the line that wraps.

// ErrGPUUnavailable is what the window and the headless renderer return
// when this build has no GPU backend. On Linux that is a build without
// `-tags novulkan` on a machine with no Vulkan headers; see
// core/cli/README.md.
const gpuAdvice = "build with -tags novulkan (see core/cli/README.md)"

// DemoState is a machine with the iPod attached, a library on it, a
// release checked and a sync half done.
func DemoState() State {
	st := State{
		Device: Device{
			Found:      true,
			Path:       `\\.\PhysicalDrive2`,
			Model:      "iPod Video 5.5G 80 GB",
			Serial:     "000A2700168F1E3C",
			Size:       80_026_361_856,
			SectorSize: 2048,
			Tested:     true,
			Volume:     `D:\`,
			Firmware:   "v0.1.2 (build 2026-09-14T22:41:07Z g928727d)",
			OSOSOK:     true,
			OSOSNote:   "237,640 bytes of 7,618,560 capacity, checksum 0x01589d64 verifies",
			Elevated:   true,
		},
		Library: Library{
			Present:     true,
			Songs:       928,
			Albums:      101,
			Genres:      17,
			IndexBytes:  237_584,
			ConfigValid: true,
			LogValid:    true,
		},
		Release: Release{
			Checked: true,
			Tag:     "v0.1.3",
			Asset:   "core.ipod",
			Notes:   "Hold banner, About in ink, version marker in the image.",
		},
		Source:    `C:\Users\brandon-home\Music\MC`,
		FlashFile: `C:\Users\brandon-home\ipod-bringup\core.ipod`,
		CLIPath:   `C:\Users\brandon-home\ipod-bringup\core.exe`,
		Job:       &JobStatus{Kind: JobSync, Text: "Talking Heads - Remain in Light", Pct: 0.62},
	}
	for _, line := range strings.Split(strings.TrimSpace(demoLog), "\n") {
		st.Log = append(st.Log, line)
	}
	return st
}

// EmptyState is the first run: nothing plugged in, nothing configured.
func EmptyState() State {
	st := State{
		Device: Device{
			Err: "disk: no iPod found",
		},
		CLIPath: "",
	}
	st.Logf("core-app (devel)")
	st.Logf("core CLI: not found beside this app — Flash and Update will print the command instead")
	st.Logf("press Refresh once the iPod is in disk mode (Select + Play) and plugged in")
	return st
}

const demoLog = `
core-app v0.1.2
core CLI: C:\Users\brandon-home\ipod-bringup\core.exe
--- refresh ---
14:02:03  device: \\.\PhysicalDrive2 — Apple iPod, 80.0 GB (74.5 GiB)
14:02:03  sector 2048 bytes (reported by the OS, confirmed: the Apple preamble is at sector 63)
14:02:03  firmware: v0.1.2 (build 2026-09-14T22:41:07Z g928727d)
14:02:03  OSOS 237,640 bytes, checksum 0x01589d64 verifies
14:02:04  library: 928 songs, 101 albums, index 237,584 B, CORECFG.DAT and CORELOG.BIN valid
--- check for updates ---
14:02:19  latest release: v0.1.3 (core.ipod)
--- sync ---
14:03:41  scanned 101 albums, 928 tracks
14:03:41  plan: 101 albums, 63 to copy, 865 already there, 0 to rename, 0 orphan(s)
14:03:41  copy
14:03:44  [58/101] Steely Dan - Aja — 0 copied, 7 skipped, 0 renamed, art skip
14:03:47  [59/101] Stevie Wonder - Innervisions — 9 copied, 0 skipped, 0 renamed, art ok
14:03:51  [60/101] Talk Talk - Spirit of Eden — 0 copied, 6 skipped, 0 renamed, art skip
14:03:55  [61/101] Talking Heads - Fear of Music — 11 copied, 0 skipped, 0 renamed, art ok
14:04:02  [62/101] Talking Heads - Remain in Light — 8 copied, 0 skipped, 0 renamed, art ok
`
