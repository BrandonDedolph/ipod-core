// Package doctor is the read-only health check: everything the
// firmware depends on, asked of a connected iPod and of the FAT volume
// it mounts, as a list of OK / WARN / FAIL / SKIP lines plus the
// numbers behind them.
//
// It is a package and not the body of `core doctor` because two front
// ends need the same answers. The CLI prints the lines; core-app's
// Device card shows the same facts as a card and needs them as fields
// (song count, index size, whether the config and the log are valid)
// rather than as formatted text. Splitting those apart would have meant
// two implementations of "is this library within the caps", which is
// exactly the check that must not have two answers.
//
// Nothing here writes, opens for writing, or repairs. "Silently" is why
// it exists: library_load_index() stops at LIB_MAX_SONGS with no error
// and no log; config_mount() refuses a short CORECFG.DAT and the
// settings simply never persist; evlog_mount() turns the log off when
// the header disagrees with the file length. Every one of those is
// invisible on the device and obvious from the host.
package doctor

import (
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/cidx"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/devicefs"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/disk"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/fwpart"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/library"
)

// State is the verdict on one line. Only Fail is meant to change an
// exit code: a Warn is something to know about (untested hardware, an
// image with no version marker), not something that is wrong.
type State string

// The four verdicts.
const (
	OK   State = "OK"
	Warn State = "WARN"
	Fail State = "FAIL"
	Skip State = "SKIP"
)

// Check is one line: its verdict, the short name in the left column,
// and the sentence. Text is already formatted — the caller decides the
// column widths, not the wording.
type Check struct {
	State State
	Name  string
	Text  string
}

// Counts tallies a slice of checks by state.
func Counts(checks []Check) map[State]int {
	m := map[State]int{}
	for _, c := range checks {
		m[c.State]++
	}
	return m
}

// report accumulates checks and, on the way past, the structured
// numbers the GUI wants. Building both in one pass is deliberate: a
// second pass that re-read the index to count albums would be a second
// place for the cap arithmetic to be wrong.
type report struct{ checks []Check }

func (r *report) line(state State, name, format string, args ...any) {
	r.checks = append(r.checks, Check{State: state, Name: name, Text: fmt.Sprintf(format, args...)})
}

// --- the device half --------------------------------------------------

// DeviceDeps are the two OS calls the device half makes. They are
// injected because both callers decorate their errors differently: the
// CLI adds the "put it in disk mode" paragraph and the elevated command
// line, and the GUI puts those in its own card.
type DeviceDeps struct {
	// Select resolves the --device selector to one iPod.
	Select func() (disk.IPod, error)
	// Open opens that device read-only.
	Open func(path string) (disk.Handle, error)
	// Missing is the state used when there is no device: Fail for a
	// plain run, Skip when a volume was named explicitly, because
	// checking a library with the iPod unplugged is a thing worth
	// being able to do.
	Missing State
}

func (d *DeviceDeps) setDefaults() {
	if d.Select == nil {
		d.Select = func() (disk.IPod, error) { return disk.SelectIPod("") }
	}
	if d.Open == nil {
		d.Open = func(path string) (disk.Handle, error) { return disk.Open(path, false) }
	}
	if d.Missing == "" {
		d.Missing = Fail
	}
}

// DeviceReport is the device half: the iPod that was found, the facts
// read off its firmware partition, and the lines.
type DeviceReport struct {
	Pod   disk.IPod
	Found bool
	// Firmware is fwpart.VersionText of the installed OSOS body, or ""
	// when it could not be read.
	Firmware string
	// OSOSOK is the installed image's checksum verdict; OSOSNote is
	// the sentence behind it.
	OSOSOK   bool
	OSOSNote string
	// Err is the one-line reason there is no device, when Found is
	// false.
	Err    string
	Checks []Check
}

// CheckDevice runs the device half.
func CheckDevice(d DeviceDeps) DeviceReport {
	d.setDefaults()
	r := &report{}
	var rep DeviceReport

	pod, err := d.Select()
	if err != nil {
		rep.Err = FirstLine(err.Error())
		r.line(d.Missing, "device", "%s", rep.Err)
		r.line(Skip, "hardware", "no device to classify")
		r.line(Skip, "directory", "no device to read")
		r.line(Skip, "osos", "no device to read")
		r.line(Skip, "firmware", "no device to read")
		rep.Checks = r.checks
		return rep
	}
	rep.Pod, rep.Found = pod, true
	r.line(OK, "device", "%s — %s, %s", pod.Disk.Path,
		strings.TrimSpace(pod.Disk.Vendor+" "+pod.Disk.Model), disk.HumanSize(pod.Disk.SizeBytes))
	if pod.Tested {
		r.line(OK, "hardware", "%s — tested", pod.Model)
	} else {
		r.line(Warn, "hardware", "%s — UNTESTED: %s", pod.Model, pod.UntestedReason)
	}

	h, err := d.Open(pod.Disk.Path)
	if err != nil {
		rep.Err = FirstLine(err.Error())
		r.line(d.Missing, "directory", "%s", rep.Err)
		r.line(Skip, "osos", "the device could not be opened")
		r.line(Skip, "firmware", "the device could not be opened")
		rep.Checks = r.checks
		return rep
	}
	defer h.Close()

	p := FirmwarePartition(pod, h)
	dir, err := fwpart.Parse(p)
	if err != nil {
		r.line(Fail, "directory", "%v", err)
		r.line(Skip, "osos", "the directory did not parse")
		r.line(Skip, "firmware", "the directory did not parse")
		rep.Checks = r.checks
		return rep
	}
	r.line(OK, "directory", "version %d at %#x, %d images", dir.Version, dir.Start, len(dir.Entries))

	idx, osos, found := dir.OSOS()
	if !found {
		rep.OSOSNote = fwpart.ErrNoOSOS.Error()
		r.line(Fail, "osos", "%v", fwpart.ErrNoOSOS)
		r.line(Skip, "firmware", "there is no OSOS image to read a version from")
		rep.Checks = r.checks
		return rep
	}
	if err := fwpart.VerifyEntry(p, osos); err != nil {
		rep.OSOSNote = fmt.Sprintf("%d bytes, checksum %#08x does NOT verify: %v",
			osos.Length, osos.Checksum, err)
		r.line(Fail, "osos", "%s", rep.OSOSNote)
	} else {
		rep.OSOSOK = true
		rep.OSOSNote = fmt.Sprintf("%d bytes of %d capacity, checksum %#08x verifies",
			osos.Length, dir.Capacity(idx), osos.Checksum)
		r.line(OK, "osos", "%s", rep.OSOSNote)
	}

	body, err := fwpart.ReadBody(p, osos)
	if err != nil {
		r.line(Fail, "firmware", "the OSOS body could not be read: %v", err)
		rep.Checks = r.checks
		return rep
	}
	// An image with no marker is every image before v0.1.3, including
	// whatever is installed right now: a WARN, because the honest
	// answer is "cannot tell", and not a FAIL, because there is nothing
	// wrong with the image.
	state := Warn
	if _, _, found := fwpart.FindVersion(body); found && fwpart.CountVersionMarkers(body) == 1 {
		state = OK
	}
	rep.Firmware = fwpart.VersionText(body)
	r.line(state, "firmware", "%s", rep.Firmware)
	rep.Checks = r.checks
	return rep
}

// FirmwarePartition wraps the firmware partition of an open device as
// something fwpart can read: offsets relative to the start of partition
// 0, bounded by its length, so nothing in fwpart can address a byte of
// the music partition even by arithmetic error.
func FirmwarePartition(pod disk.IPod, h disk.Handle) fwpart.Partition {
	return fwpart.Partition{
		R:    io.NewSectionReader(h, pod.FWPartStart, pod.FWPartLen),
		Size: pod.FWPartLen,
	}
}

// VolumeOf picks the mounted FAT volume off the device the OS reported.
// MountPoints is what a check wants (a path it can open); Volumes is
// the fallback, because on Windows the volume name IS the path ("D:").
func VolumeOf(pod disk.IPod, ok bool) string {
	if !ok {
		return ""
	}
	for _, mp := range pod.Disk.MountPoints {
		if strings.TrimSpace(mp) != "" {
			return mp
		}
	}
	for _, v := range pod.Disk.Volumes {
		if len(v) == 2 && v[1] == ':' { // a Windows drive letter is a path
			return v + `\`
		}
	}
	return ""
}

// --- the volume half ---------------------------------------------------

// VolumeReport is the volume half: the lines, and the numbers the GUI
// puts on its Device card.
type VolumeReport struct {
	Volume string
	// Present is true when Music/ exists — everything else here is
	// meaningless without it.
	Present bool
	// Songs, Albums and Genres are derived from the index records, the
	// same way library_load_index() would.
	Songs, Albums, Genres int
	IndexBytes            int64
	IndexOK               bool
	ConfigValid, LogValid bool
	// ClockStamped is the host stamp in CORECFG.DAT (zero when there is
	// none); ClockPending says the device has not booted since it was
	// written, so the iPod's own clock is not yet that time.
	ClockStamped time.Time
	ClockPending bool
	// OTGValid is COREOTG.DAT, the On-The-Go live list; OTGEntries is how
	// many tracks the newest slot holds.
	OTGValid   bool
	OTGEntries int
	Playlists  []string
	// Note is the one-line reason the volume half stopped early, when
	// it did.
	Note   string
	Checks []Check
}

// CheckVolume runs every volume check. An empty volume is not an error:
// it yields a single SKIP saying how to name one.
func CheckVolume(volume string) VolumeReport {
	r := &report{}
	rep := VolumeReport{Volume: volume}

	if volume == "" {
		rep.Note = "no mounted volume found; pass --volume <path> to check one"
		r.line(Skip, "volume", "%s", rep.Note)
		rep.Checks = r.checks
		return rep
	}

	st, err := os.Stat(volume)
	if err != nil || !st.IsDir() {
		rep.Note = fmt.Sprintf("%s is not a directory this process can read", volume)
		r.line(Fail, "volume", "%s", rep.Note)
		rep.Checks = r.checks
		return rep
	}

	music := filepath.Join(volume, devicefs.MusicDir)
	if st, err := os.Stat(music); err != nil || !st.IsDir() {
		rep.Note = fmt.Sprintf("%s/ is missing — run `core sync`", devicefs.MusicDir)
		r.line(Fail, "music", "%s/ is missing — the firmware's library root "+
			"(run `core sync`)", devicefs.MusicDir)
		rep.Checks = r.checks
		return rep
	}
	rep.Present = true
	r.line(OK, "music", "%s/ present", devicefs.MusicDir)

	checkIndex(r, &rep, filepath.Join(music, devicefs.IndexName))
	checkConfig(r, &rep, filepath.Join(volume, devicefs.ConfigName))
	checkLog(r, &rep, filepath.Join(volume, devicefs.LogName))
	checkOTG(r, &rep, filepath.Join(volume, devicefs.OTGName))
	checkPlaylists(r, &rep, filepath.Join(music, devicefs.PlaylistDir))

	rep.Checks = r.checks
	return rep
}

// CheckIndex, CheckConfig, CheckLog and CheckPlaylists are the four
// volume checks on their own, for a caller that has one file and not a
// whole volume.
func CheckIndex(path string) []Check {
	r, rep := &report{}, &VolumeReport{}
	checkIndex(r, rep, path)
	return r.checks
}

// CheckConfig checks one CORECFG.DAT.
func CheckConfig(path string) []Check {
	r, rep := &report{}, &VolumeReport{}
	checkConfig(r, rep, path)
	return r.checks
}

// CheckLog checks one CORELOG.BIN.
func CheckLog(path string) []Check {
	r, rep := &report{}, &VolumeReport{}
	checkLog(r, rep, path)
	return r.checks
}

// CheckOTG checks one COREOTG.DAT.
func CheckOTG(path string) []Check {
	r, rep := &report{}, &VolumeReport{}
	checkOTG(r, rep, path)
	return r.checks
}

// CheckPlaylists checks one Playlists/ directory.
func CheckPlaylists(dir string) []Check {
	r, rep := &report{}, &VolumeReport{}
	checkPlaylists(r, rep, dir)
	return r.checks
}

// checkIndex applies exactly library_load_index()'s own acceptance test
// (that is what cidx.Decode is), and then the three caps, which the
// device does not check at all — it just stops reading.
func checkIndex(r *report, rep *VolumeReport, path string) {
	b, err := os.ReadFile(path)
	if errors.Is(err, os.ErrNotExist) {
		r.line(Fail, "index", "%s/%s is missing; the device would fall back to a "+
			"slow tag scan (run `core index` or `core sync`)", devicefs.MusicDir, devicefs.IndexName)
		return
	}
	if err != nil {
		r.line(Fail, "index", "%v", err)
		return
	}
	rep.IndexBytes = int64(len(b))
	recs, err := cidx.Decode(b)
	if err != nil {
		r.line(Fail, "index", "%s does not validate: %v", devicefs.IndexName, err)
		r.line(Skip, "caps", "the index did not decode")
		return
	}
	rep.IndexOK = true
	r.line(OK, "index", "%s: %d bytes, %d records, header and CRC OK",
		devicefs.IndexName, len(b), len(recs))

	albums, genres := map[string]struct{}{}, map[string]struct{}{}
	for _, rec := range recs {
		albums[rec.Folder] = struct{}{}
		if g := strings.TrimSpace(rec.Genre); g != "" {
			genres[g] = struct{}{}
		}
	}
	rep.Songs, rep.Albums, rep.Genres = len(recs), len(albums), len(genres)
	capLine(r, "songs", len(recs), library.MaxSongs)
	capLine(r, "albums", len(albums), library.MaxAlbums)
	capLine(r, "genres", len(genres), library.MaxGenres)
}

// capLine reports one cap. Over it is a FAIL and not a warning: past a
// cap the device drops records with no error and no log, so the library
// on screen is quietly not the library on disk.
func capLine(r *report, what string, n, max int) {
	switch {
	case n > max:
		r.line(Fail, what, "%d of a %d maximum — the device silently drops the rest",
			n, max)
	case n > max*9/10:
		r.line(Warn, what, "%d of a %d maximum", n, max)
	default:
		r.line(OK, what, "%d of a %d maximum", n, max)
	}
}

func checkConfig(r *report, rep *VolumeReport, path string) {
	b, err := readHeadFile(path, devicefs.ConfigMinBytes)
	if errors.Is(err, os.ErrNotExist) {
		r.line(Fail, "config", "%s is missing; the firmware cannot create it, so settings "+
			"would never persist (run `core sync`)", devicefs.ConfigName)
		return
	}
	if err != nil {
		r.line(Fail, "config", "%v", err)
		return
	}
	seq, ok := devicefs.ConfigFileValid(b)
	if !ok {
		r.line(Fail, "config", "%s holds no valid record (or is shorter than the %d bytes "+
			"config_mount() requires)", devicefs.ConfigName, devicefs.ConfigMinBytes)
		return
	}
	rep.ConfigValid = true
	r.line(OK, "config", "%s valid, newest record at seq %d", devicefs.ConfigName, seq)
	checkClock(r, rep, b, seq)
}

// checkClock reads the time block out of the newest record: what the host
// stamped and whether the device has acted on it.
//
// PENDING IS A WARNING, not a failure. The iPod cannot be told the time over
// the cable — the stamp waits in CORECFG.DAT for the next boot — so "stamped
// but not applied" is the normal state of a device that has not been switched
// on since the last sync, and it is also exactly what a user sees when they
// wonder why the clock is wrong. Saying which of the two it is beats making
// them guess.
func checkClock(r *report, rep *VolumeReport, head []byte, newest uint32) {
	for i := 0; i < devicefs.ConfigSlots; i++ {
		off := i * devicefs.ConfigSlotBytes
		slot := head[off : off+devicefs.ConfigSlotBytes]
		seq, _, ok := devicefs.DecodeConfigSlot(slot)
		if !ok || seq != newest {
			continue
		}
		ts, ok := devicefs.DecodeConfigTime(slot)
		if !ok || ts.HostEpoch == 0 {
			r.line(Warn, "clock", "never stamped — the iPod's clock is whatever it "+
				"was set to by hand (`core sync` or `core eject` sets it)")
			return
		}
		rep.ClockStamped = time.Unix(int64(ts.HostEpoch), 0).UTC()
		rep.ClockPending = ts.Pending()
		when := rep.ClockStamped.Format("2006-01-02 15:04") + " UTC"
		zone := offsetText(int(ts.HostOffMin))
		if ts.Pending() {
			r.line(Warn, "clock", "host stamp %s (%s) is pending — the device applies "+
				"it at its next boot", when, zone)
			return
		}
		r.line(OK, "clock", "host stamp %s (%s), applied by the device", when, zone)
		return
	}
}

// offsetText renders a minutes offset the way a user reads a time zone.
func offsetText(min int) string {
	sign := "+"
	if min < 0 {
		sign, min = "-", -min
	}
	return fmt.Sprintf("UTC%s%02d:%02d", sign, min/60, min%60)
}

// checkLog applies evlog_mount()'s two tests together: the header has
// to validate AND its block count has to agree with the file's actual
// length, or the log stays off and nothing says so.
func checkLog(r *report, rep *VolumeReport, path string) {
	st, err := os.Stat(path)
	if errors.Is(err, os.ErrNotExist) {
		r.line(Warn, "log", "%s is missing; the event log stays off (run `core sync`)",
			devicefs.LogName)
		return
	}
	if err != nil {
		r.line(Fail, "log", "%v", err)
		return
	}
	head, err := readHeadFile(path, devicefs.LogBlockBytes)
	if err != nil {
		r.line(Fail, "log", "%v", err)
		return
	}
	blocks, fileID, ok := devicefs.DecodeLogHeader(head)
	if !ok {
		r.line(Fail, "log", "%s has no valid CLOG header", devicefs.LogName)
		return
	}
	if int64(blocks)*devicefs.LogBlockBytes != st.Size() {
		r.line(Fail, "log", "%s says %d blocks (%d bytes) and the file is %d bytes — "+
			"evlog_mount() refuses that and the log stays off, silently",
			devicefs.LogName, blocks, int64(blocks)*devicefs.LogBlockBytes, st.Size())
		return
	}
	rep.LogValid = true
	r.line(OK, "log", "%s valid, %d blocks of %d bytes, file id %#08x",
		devicefs.LogName, blocks, devicefs.LogBlockBytes, fileID)
}

// checkOTG applies otg_store_mount()'s own acceptance test: the file has to
// be at least two slots long and one of them has to decode, or the On-The-Go
// list works for the session and persists NOTHING — with no error on screen
// and nothing in the log to say so.
func checkOTG(r *report, rep *VolumeReport, path string) {
	b, err := readHeadFile(path, devicefs.OTGMinBytes)
	if errors.Is(err, os.ErrNotExist) {
		r.line(Warn, "on-the-go", "%s is missing; the On-The-Go list works for "+
			"a session and is lost at the next boot (run `core sync`)", devicefs.OTGName)
		return
	}
	if err != nil {
		r.line(Fail, "on-the-go", "%v", err)
		return
	}
	seq, ok := devicefs.OTGFileValid(b)
	if !ok {
		r.line(Fail, "on-the-go", "%s holds no valid slot (or is shorter than the "+
			"%d bytes otg_store_mount() requires)", devicefs.OTGName, devicefs.OTGMinBytes)
		return
	}
	n := 0
	for i := 0; i < devicefs.OTGSlots; i++ {
		off := i * devicefs.OTGSlotBytes
		if s, _, entries, valid := devicefs.DecodeOTGSlot(b[off:]); valid && s == seq {
			n = len(entries)
		}
	}
	rep.OTGValid, rep.OTGEntries = true, n
	r.line(OK, "on-the-go", "%s valid, newest slot at seq %d, %d track(s)",
		devicefs.OTGName, seq, n)
}

func checkPlaylists(r *report, rep *VolumeReport, dir string) {
	entries, err := os.ReadDir(dir)
	if errors.Is(err, os.ErrNotExist) {
		r.line(OK, "playlists", "%s/%s/ absent — no saved playlists",
			devicefs.MusicDir, devicefs.PlaylistDir)
		return
	}
	if err != nil {
		r.line(Fail, "playlists", "%v", err)
		return
	}
	var names []string
	for _, e := range entries {
		if e.IsDir() {
			continue
		}
		switch strings.ToLower(filepath.Ext(e.Name())) {
		case ".m3u8", ".m3u":
			names = append(names, e.Name())
		}
	}
	sort.Strings(names)
	rep.Playlists = names
	if len(names) == 0 {
		r.line(OK, "playlists", "%s/%s/ present, empty", devicefs.MusicDir, devicefs.PlaylistDir)
	} else {
		r.line(OK, "playlists", "%d in %s/%s/ (%s)", len(names),
			devicefs.MusicDir, devicefs.PlaylistDir, strings.Join(names, ", "))
	}

	// The five On-The-Go slots are device files sitting in the same folder.
	// They are reported separately because "5 playlists" would otherwise
	// mean five the user never made, and because a DAMAGED one is a torn
	// save the device will refuse to open and nothing else would say so.
	for n := 1; n <= devicefs.OTGPlaylistSlots; n++ {
		name := devicefs.OTGSlotName(n)
		info, err := devicefs.OTGSlotFileState(filepath.Join(dir, name))
		label := fmt.Sprintf("otg slot %d", n)
		switch {
		case err != nil:
			// An unreadable slot file is the same dead end as a foreign one:
			// the device cannot look inside it, so it will never write to it.
			r.line(Fail, label, "%s: %v — the device will never write to a slot "+
				"it cannot read. If the file is not one of yours, delete "+
				"%s/%s/%s and run `core sync` to put an empty one back",
				name, err, devicefs.MusicDir, devicefs.PlaylistDir, name)
		case info.State == devicefs.OTGSlotAbsent:
			r.line(Warn, label, "%s is missing; Save has one fewer slot "+
				"(run `core sync`)", name)
		case info.State == devicefs.OTGSlotForeign:
			// Two things look identical here and only the user can tell them
			// apart: a playlist they made, and a slot whose save was
			// interrupted inside its very last write. Neither will ever be
			// written to again, so say what the way out is.
			r.line(Warn, label, "%s carries no On-The-Go header, so the device "+
				"lists and plays it and NEVER writes to it. If you did not put "+
				"it there (an interrupted save can leave one looking like "+
				"this), delete %s/%s/%s and run `core sync` to put an empty "+
				"one back", name, devicefs.MusicDir, devicefs.PlaylistDir, name)
		case info.State == devicefs.OTGSlotDamaged:
			r.line(Fail, label, "%s is a torn save (header gen %d, trailer gen %d, "+
				"%d entry line(s) against a count of %d); the device opens it to "+
				"\"Playlist damaged\" with Delete Playlist under it. The slot is NOT "+
				"free until that Delete rewrites it — Save only ever writes into an "+
				"empty slot",
				name, info.Gen, info.TrailerGen, info.Lines, info.Count)
		case info.Size < devicefs.OTGSlotFileMin ||
			info.Size%devicefs.OTGSlotSizeGrain != 0:
			r.line(Fail, label, "%s is %d bytes; the firmware only writes a slot "+
				"file of at least %d bytes and a multiple of %d, so Save will "+
				"skip it", name, info.Size, devicefs.OTGSlotFileMin,
				devicefs.OTGSlotSizeGrain)
		case info.State == devicefs.OTGSlotEmpty:
			r.line(OK, label, "%s empty (hidden from the Playlists list, free "+
				"for Save)", name)
		default:
			r.line(OK, label, "%s: %d track(s)", name, info.Count)
		}
	}
}

// readHeadFile reads up to n bytes from the head of a file. A file
// shorter than n comes back short rather than as an error — the
// validators decide what a short file means, and they already do.
func readHeadFile(path string, n int) ([]byte, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	b := make([]byte, n)
	got, err := io.ReadFull(f, b)
	if err != nil && !errors.Is(err, io.ErrUnexpectedEOF) && !errors.Is(err, io.EOF) {
		return nil, err
	}
	return b[:got], nil
}

// FirstLine is the first line of a multi-line error, which is what a
// one-line check has room for.
func FirstLine(s string) string {
	if i := strings.IndexByte(s, '\n'); i >= 0 {
		return s[:i]
	}
	return s
}
