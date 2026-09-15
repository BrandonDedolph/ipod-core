// SPDX-License-Identifier: Apache-2.0

// Package devicefs writes and validates the files the firmware expects to
// find on the iPod's FAT32 data partition.
//
// The firmware's FAT32 driver is read-only apart from one hole: it overwrites
// the data sectors of files that already exist (core/kernel/config.c for
// settings, core/kernel/evlog.c for the log). It can neither create, grow,
// move nor delete anything. So every file on the volume — the music tree, the
// index, the playlists, and the two pre-allocated device files CORECFG.DAT and
// CORELOG.BIN — is created by the host, and this package is the host half of
// those two on-disk formats.
//
// The Python reference implementations are tools/make_config.py and
// tools/make_log.py; the device halves are core/kernel/config.c and
// core/kernel/evlog.c. All four must agree byte for byte, and the tests in
// this package hold that line (see config_test.go and log_test.go, which
// re-run the Python tools when python3 is on PATH).
package devicefs

// Names and layout the firmware hard-codes.
//
//	<volume root>/CORECFG.DAT          core/kernel/config.h:51
//	<volume root>/CORELOG.BIN          core/kernel/evlog.h:91
//	<volume root>/Music/               the library root (core/kernel/main.c:2211)
//	<volume root>/Music/CORELIB.IDX    the index (core/kernel/main.c:1753)
//	<volume root>/Music/Playlists/     *.m3u8 / *.m3u  (core/library/playlist.h:55)
const (
	ConfigName  = "CORECFG.DAT"
	LogName     = "CORELOG.BIN"
	MusicDir    = "Music"
	IndexName   = "CORELIB.IDX"
	PlaylistDir = "Playlists"
)
