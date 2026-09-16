package cli

import (
	"encoding/binary"
	"fmt"
	"hash/crc32"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/cidx"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/devicefs"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/library"
)

// doctor's device half needs an iPod and its volume half does not, so
// every test here passes --volume and asserts on the volume lines. The
// device lines are asserted only for their SKIP/FAIL shape: this runs
// on a developer machine that may or may not have something attached,
// and a test that depended on which would be a flake.

// syntheticVolume builds a volume the firmware would accept: Music/,
// a valid CORELIB.IDX, and the two pre-allocated files written by the
// same code `core sync` writes them with.
func syntheticVolume(t *testing.T, records int, playlists []string) string {
	t.Helper()
	vol := t.TempDir()
	music := filepath.Join(vol, devicefs.MusicDir)
	if err := os.MkdirAll(music, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(music, devicefs.IndexName),
		cidx.Encode(sampleRecords(records)), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := devicefs.EnsureConfig(vol); err != nil {
		t.Fatalf("EnsureConfig: %v", err)
	}
	// 8 blocks instead of the device's 2048: EnsureLog validates the
	// count against the file length either way, and a 4 MiB write per
	// test case is a cost with no coverage behind it.
	if _, err := devicefs.EnsureLog(vol, 8*devicefs.LogBlockBytes); err != nil {
		t.Fatalf("EnsureLog: %v", err)
	}
	if playlists != nil {
		dir := filepath.Join(music, devicefs.PlaylistDir)
		if err := os.MkdirAll(dir, 0o755); err != nil {
			t.Fatal(err)
		}
		for _, name := range playlists {
			if err := os.WriteFile(filepath.Join(dir, name),
				[]byte("#EXTM3U\n/Music/A - B/01. C.flac\n"), 0o644); err != nil {
				t.Fatal(err)
			}
		}
	}
	return vol
}

// sampleRecords spreads n songs over 4 albums and 3 genres, so the
// album and genre counts doctor derives from the records are not just
// the record count again.
func sampleRecords(n int) []cidx.Record {
	recs := make([]cidx.Record, n)
	genres := []string{"Rock", "Jazz", "Electronic"}
	for i := range recs {
		recs[i] = cidx.Record{
			DurationS: uint32(180 + i),
			Track:     uint16(i%20 + 1),
			Disc:      1,
			Folder:    fmt.Sprintf("Artist %d - Album %d", i%4, i%4),
			File:      fmt.Sprintf("%02d. Track %d.flac", i%20+1, i),
			Title:     fmt.Sprintf("Track %d", i),
			Artist:    fmt.Sprintf("Artist %d", i%4),
			Genre:     genres[i%len(genres)],
		}
	}
	return recs
}

// deviceLinesSkipped reports whether the run found no device, which is
// the normal case on a machine with no iPod plugged in.
func deviceLinesSkipped(out string) bool {
	return strings.Contains(out, "SKIP  device")
}

func TestDoctorOnAValidVolume(t *testing.T) {
	vol := syntheticVolume(t, 928, []string{"Favourites.m3u8", "Recent.m3u"})

	out, _, err := runCore(t, "doctor", "--volume", vol)
	if !deviceLinesSkipped(out) {
		t.Skipf("this machine has something that identifies as an iPod attached:\n%s", out)
	}
	if err != nil {
		t.Fatalf("doctor on a valid volume failed: %v\n%s", err, out)
	}
	for _, want := range []string{
		"OK    music        Music/ present",
		"OK    index        CORELIB.IDX:",
		"928 records, header and CRC OK",
		fmt.Sprintf("OK    songs        928 of a %d maximum", library.MaxSongs),
		fmt.Sprintf("OK    albums       4 of a %d maximum", library.MaxAlbums),
		fmt.Sprintf("OK    genres       3 of a %d maximum", library.MaxGenres),
		"OK    config       CORECFG.DAT valid, newest record at seq 1",
		"OK    log          CORELOG.BIN valid, 8 blocks of 2048 bytes",
		"OK    playlists    2 in Music/Playlists/ (Favourites.m3u8, Recent.m3u)",
		"0 FAIL",
	} {
		if !strings.Contains(out, want) {
			t.Errorf("doctor did not print %q:\n%s", want, out)
		}
	}
}

// The device half is SKIP, not FAIL, when a volume was named: checking
// a library with the iPod unplugged is a thing worth being able to do.
func TestDoctorSkipsTheDeviceHalfWithAVolume(t *testing.T) {
	vol := syntheticVolume(t, 10, nil)
	out, _, err := runCore(t, "doctor", "--volume", vol)
	if !deviceLinesSkipped(out) {
		t.Skip("this machine has something that identifies as an iPod attached")
	}
	if err != nil {
		t.Fatalf("doctor with no device and a volume failed: %v\n%s", err, out)
	}
	for _, name := range []string{"hardware", "directory", "osos", "firmware"} {
		if !strings.Contains(out, "SKIP  "+name) {
			t.Errorf("the %s check is not a SKIP:\n%s", name, out)
		}
	}
	if strings.Contains(out, "FAIL  ") || !strings.Contains(out, "0 FAIL") {
		t.Errorf("a device-less run reported a failure:\n%s", out)
	}
	if !strings.Contains(out, "skipped") {
		t.Errorf("the summary does not count the skips:\n%s", out)
	}
	// An absent Playlists/ is not a problem; a device with no saved
	// playlists is the normal state.
	if !strings.Contains(out, "OK    playlists    Music/Playlists/ absent") {
		t.Errorf("an absent playlist folder was not reported as fine:\n%s", out)
	}
}

// A corrupt index is the failure that is invisible on the device: the
// firmware just falls back to a slow tag scan, or loads nothing.
func TestDoctorFailsOnACorruptIndex(t *testing.T) {
	vol := syntheticVolume(t, 50, nil)
	path := filepath.Join(vol, devicefs.MusicDir, devicefs.IndexName)
	b, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	b[len(b)-1] ^= 0xFF // one flipped bit inside the CRC's coverage
	if err := os.WriteFile(path, b, 0o644); err != nil {
		t.Fatal(err)
	}

	out, _, err := runCore(t, "doctor", "--volume", vol)
	if err == nil {
		t.Fatalf("doctor accepted a corrupt index:\n%s", out)
	}
	if !strings.Contains(err.Error(), "check(s) failed") {
		t.Errorf("the failure is not the summary error: %v", err)
	}
	if !strings.Contains(out, "FAIL  index") || !strings.Contains(out, "CRC-32") {
		t.Errorf("doctor did not name the CRC as the problem:\n%s", out)
	}
	if !strings.Contains(out, "SKIP  caps") {
		t.Errorf("the caps were checked against an index that did not decode:\n%s", out)
	}
}

func TestDoctorFailsOnAMissingIndexAndConfig(t *testing.T) {
	vol := t.TempDir()
	if err := os.MkdirAll(filepath.Join(vol, devicefs.MusicDir), 0o755); err != nil {
		t.Fatal(err)
	}
	out, _, err := runCore(t, "doctor", "--volume", vol)
	if err == nil {
		t.Fatalf("doctor passed a volume with nothing on it:\n%s", out)
	}
	for _, want := range []string{
		"FAIL  index        Music/CORELIB.IDX is missing",
		"FAIL  config       CORECFG.DAT is missing",
		"WARN  log          CORELOG.BIN is missing",
	} {
		if !strings.Contains(out, want) {
			t.Errorf("doctor did not print %q:\n%s", want, out)
		}
	}
}

// Music/ absent is the one that stops the rest: everything else this
// checks lives under it or is meaningless without it.
func TestDoctorFailsWithoutMusic(t *testing.T) {
	out, _, err := runCore(t, "doctor", "--volume", t.TempDir())
	if err == nil {
		t.Fatalf("doctor passed a volume with no Music/:\n%s", out)
	}
	if !strings.Contains(out, "FAIL  music") || !strings.Contains(out, "core sync") {
		t.Errorf("doctor did not explain the missing library root:\n%s", out)
	}
}

// A log whose header disagrees with the file length is evlog_mount()'s
// silent-off case, and the whole reason this check exists.
func TestDoctorFailsOnALogThatDisagreesWithItsLength(t *testing.T) {
	vol := syntheticVolume(t, 10, nil)
	path := filepath.Join(vol, devicefs.LogName)
	b, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, b[:len(b)-devicefs.LogBlockBytes], 0o644); err != nil {
		t.Fatal(err)
	}
	out, _, err := runCore(t, "doctor", "--volume", vol)
	if err == nil {
		t.Fatalf("doctor accepted a truncated log:\n%s", out)
	}
	if !strings.Contains(out, "FAIL  log") || !strings.Contains(out, "stays off, silently") {
		t.Errorf("doctor did not explain the log mismatch:\n%s", out)
	}
}

// Past LIB_MAX_SONGS the device drops records with no error and no log.
// That is a FAIL, not a warning: the library on screen is quietly not
// the library on disk.
func TestDoctorFailsPastTheSongCap(t *testing.T) {
	vol := syntheticVolume(t, library.MaxSongs+1, nil)
	out, _, err := runCore(t, "doctor", "--volume", vol)
	if err == nil {
		t.Fatalf("doctor accepted a library past the cap:\n%s", out)
	}
	want := fmt.Sprintf("FAIL  songs        %d of a %d maximum", library.MaxSongs+1, library.MaxSongs)
	if !strings.Contains(out, want) {
		t.Errorf("doctor did not report the cap:\n%s", out)
	}
	if !strings.Contains(out, "silently drops") {
		t.Errorf("doctor did not say what going past the cap costs:\n%s", out)
	}
}

func TestDoctorHasItsFlags(t *testing.T) {
	cmd := findCmd(t, "doctor")
	if cmd.Flags().Lookup("volume") == nil {
		t.Error("core doctor has no --volume")
	}
	if cmd.InheritedFlags().Lookup("device") == nil {
		t.Error("core doctor cannot see the global --device flag")
	}
}

// The clock line. A wrong clock on the iPod has exactly three causes and
// doctor has to tell them apart: nothing ever stamped it, a stamp is waiting
// for the device to boot, or the device has already taken the stamp and the
// clock is simply what it is.
func TestDoctorReportsTheClock(t *testing.T) {
	vol := syntheticVolume(t, 10, nil)

	out, _, err := runCore(t, "doctor", "--volume", vol)
	if err != nil {
		t.Fatalf("doctor: %v\n%s", err, out)
	}
	if !strings.Contains(out, "WARN  clock        never stamped") {
		t.Errorf("a volume that has never been stamped does not say so:\n%s", out)
	}

	// After a sync-style stamp: pending, because the device has not booted.
	now := time.Unix(1789555320, 0).In(time.FixedZone("CEST", 2*3600))
	if _, err := devicefs.StampConfigTime(vol, now); err != nil {
		t.Fatal(err)
	}
	out, _, err = runCore(t, "doctor", "--volume", vol)
	if err != nil {
		t.Fatalf("doctor: %v\n%s", err, out)
	}
	if !strings.Contains(out, "WARN  clock") || !strings.Contains(out, "pending") ||
		!strings.Contains(out, "2026-09-16 10:42 UTC (UTC+02:00)") {
		t.Errorf("a pending stamp is not reported with its time and zone:\n%s", out)
	}

	// Now pretend the device booted and marked it: applied_epoch := host_epoch.
	cfg := filepath.Join(vol, devicefs.ConfigName)
	b, err := os.ReadFile(cfg)
	if err != nil {
		t.Fatal(err)
	}
	newest, _ := devicefs.ConfigFileValid(b)
	for i := 0; i < devicefs.ConfigSlots; i++ {
		off := i * devicefs.ConfigSlotBytes
		slot := b[off : off+devicefs.ConfigSlotBytes]
		if seq, _, ok := devicefs.DecodeConfigSlot(slot); !ok || seq != newest {
			continue
		}
		// payload offset 48 = host_epoch, 56 = applied_epoch (the record
		// layout in core/kernel/config.c).
		copy(slot[12+56:12+60], slot[12+48:12+52])
		binary.LittleEndian.PutUint32(slot[1020:], crc32.ChecksumIEEE(slot[:1020]))
	}
	if err := os.WriteFile(cfg, b, 0o644); err != nil {
		t.Fatal(err)
	}
	out, _, err = runCore(t, "doctor", "--volume", vol)
	if err != nil {
		t.Fatalf("doctor: %v\n%s", err, out)
	}
	if !strings.Contains(out, "OK    clock") || !strings.Contains(out, "applied by the device") {
		t.Errorf("an applied stamp is not reported as done:\n%s", out)
	}
}
