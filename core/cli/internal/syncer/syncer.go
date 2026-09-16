// SPDX-License-Identifier: Apache-2.0

// Package syncer puts a source music tree onto the iPod's FAT volume in the
// layout the firmware reads, and nothing else.
//
// The firmware cannot create, grow, move or delete a file: every byte on the
// volume is put there by the host. So a sync is not "copy some files", it is
// the whole device state — the music tree, the two art sidecars per album, the
// playlists, CORECFG.DAT, CORELOG.BIN and the index — computed as a PLAN
// first and only then executed. The plan is what --dry-run prints and what the
// reviewer reads; nothing in Execute decides anything the plan did not already
// say.
//
// Two rules the rest of the package exists to keep:
//
//   - THE INDEX IS WRITTEN LAST. The device binds a record to a file by
//     hashing the folder and file names out of the index (see internal/cidx).
//     An index that names a track not yet copied orphans the record — bad, but
//     legible. An index written before a copy that then failed is a library
//     that lies about itself, and the user has no way to tell. So every other
//     write happens first, and a failed copy aborts before CORELIB.IDX is
//     touched: the device keeps the index it already had, which still matches
//     the files it already had.
//   - NOTHING IS DELETED WITHOUT --prune --yes. The plan always LISTS what is
//     on the device and not in the source (that is information the user wants),
//     but removing it takes both flags.
package syncer

import (
	"crypto/sha256"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/cidx"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/coreart"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/devicefs"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/library"
)

// MTimeSlack is how far apart a source's and a destination's modification
// times may be and still mean "the same file". FAT stores a timestamp to two
// seconds, so a byte-identical copy legitimately reads back up to two seconds
// off; anything tighter re-copies the whole library on every run.
const MTimeSlack = 2 * time.Second

// CopyBufferSize is the streaming buffer. One MiB is large enough that the USB
// bridge sees big writes (small ones are what make an iPod sync take an hour)
// and small enough to be irrelevant to host memory.
const CopyBufferSize = 1 << 20

// ErrPruneNeedsYes is returned by Execute when --prune was asked for, there is
// something to prune, and --yes was not given. Nothing is written when it is
// returned: the caller prints the list and the user re-runs.
var ErrPruneNeedsYes = errors.New("syncer: --prune needs --yes")

// ErrPruneEmptySource is returned by Execute when --prune --yes would act on
// a plan whose source scanned to zero tracks: every album on the device
// would be an orphan, which is a wrong --src, not a request to empty it.
var ErrPruneEmptySource = errors.New("syncer: refusing to prune against an empty source")

// Options is one sync invocation, as the flags spell it.
type Options struct {
	// Src is the source tree of "Album - Artist" folders; Dst is the VOLUME
	// ROOT of the device (D:\, /media/IPOD), not its Music folder.
	Src, Dst string

	// Prune removes everything under Music/ that the plan did not produce.
	// It needs Yes as well; without it Execute refuses and lists.
	Prune bool
	// Verify compares content hashes instead of size+mtime when deciding
	// whether a track is already on the device. Slow and thorough.
	Verify bool
	// DryRun makes Execute a no-op: not one byte is written, not even
	// CORECFG.DAT.
	DryRun bool
	// NoArt skips the sidecars entirely (existing ones are left alone and
	// are never pruned). ArtRefresh re-renders them even when the ones on
	// the device are valid.
	NoArt, ArtRefresh bool
	// Yes confirms the destructive half of Prune.
	Yes bool

	// GenreMap is the artist -> genre JSON passed to library.LoadGenreMap.
	GenreMap string
	// Playlists is the folder of .m3u8/.m3u sources; empty means
	// <Src>/Playlists.
	Playlists string

	// Progress, when set, is called with one Event per album and per phase.
	Progress func(Event)
}

// EventKind says what an Event is about.
type EventKind int

// The kinds of Event.
const (
	// EventPhase starts a phase: "copy", "playlists", "prune", "device
	// files", "index".
	EventPhase EventKind = iota
	// EventAlbum reports one finished album: this is the per-album line the
	// CLI prints.
	EventAlbum
	// EventWarning is something the user should read but that did not stop
	// the sync.
	EventWarning
	// EventDone is the last event: the sync finished.
	EventDone
)

// Event is one progress notification.
type Event struct {
	Kind  EventKind
	Phase string // EventPhase, EventDone

	Album                    string // EventAlbum
	Index, Total             int    // EventAlbum: album n of m
	Copied, Skipped, Renamed int    // EventAlbum
	Bytes                    int64  // EventAlbum: bytes copied
	// Art is "ok" (rendered), "skip" (already valid), "none" (the album
	// has no embedded cover), "off" (--no-art) or "" (not reached).
	Art string

	Message string // EventWarning
}

// FileOp is one track: where it comes from, where it goes, and why the plan
// put it in the list it is in.
type FileOp struct {
	Src     string    `json:"src"`
	Dst     string    `json:"dst"`
	Device  string    `json:"device"` // "Music/<folder>/<name>", for display
	Album   string    `json:"album"`  // the album's DeviceFolder
	Size    int64     `json:"size"`
	ModTime time.Time `json:"mtime"`
	Reason  string    `json:"reason"`
}

// ArtOp is one album's pair of sidecars.
type ArtOp struct {
	Album   string `json:"album"` // DeviceFolder
	Dir     string `json:"dir"`   // the destination album folder
	SrcFLAC string `json:"src"`   // the file the cover is read from
	Write   bool   `json:"write"` // false = the sidecars on the device are fine
	Reason  string `json:"reason"`
}

// PlaylistOp is one playlist, already mapped to device paths.
type PlaylistOp struct {
	Name      string   `json:"name"` // "Favourites.m3u8"
	Src       string   `json:"src"`
	Dst       string   `json:"dst"`
	Entries   []string `json:"entries"` // "/Music/<folder>/<file>"
	Dropped   []string `json:"dropped"` // lines that mapped to nothing
	Unchanged bool     `json:"unchanged"`
}

// IndexOp is CORELIB.IDX.
type IndexOp struct {
	Dst       string `json:"dst"`
	Bytes     []byte `json:"-"`
	Size      int    `json:"size"`
	Records   int    `json:"records"`
	Unchanged bool   `json:"unchanged"`
}

// Plan is the whole sync, decided before anything is written.
type Plan struct {
	// Copy, Skip and Rename partition the source tracks. Rename is the
	// narrow case of a file already on the device under a name that differs
	// only in letter case: the bytes are there, so moving it beats sending
	// 30 MB over USB again.
	Copy   []FileOp `json:"copy"`
	Skip   []FileOp `json:"skip"`
	Rename []FileOp `json:"rename"`

	Art       []ArtOp      `json:"art"`
	Playlists []PlaylistOp `json:"playlists"`
	// Prune is everything under Music/ the plan did not produce. It is
	// always computed; it is only acted on with --prune --yes.
	Prune []string `json:"prune"`
	Index IndexOp  `json:"index"`
	// Config and Log say whether CORECFG.DAT / CORELOG.BIN have to be
	// created. A valid file is never rewritten — it holds the user's saved
	// settings and their event log.
	Config bool `json:"config"`
	Log    bool `json:"log"`

	Warnings []string `json:"warnings"`

	// AlbumOrder is the albums' device folder names in scan order — the
	// order Execute works in, and the order the index's records are in.
	AlbumOrder []string `json:"album_order"`

	// Totals, for the summary line.
	Albums     int   `json:"albums"`
	Tracks     int   `json:"tracks"`
	CopyBytes  int64 `json:"copy_bytes"`
	SkipBytes  int64 `json:"skip_bytes"`
	PruneBytes int64 `json:"prune_bytes"`

	// Src and Dst are carried so a plan can be printed on its own.
	Src string `json:"src"`
	Dst string `json:"dst"`
}

// ScanSource runs the checks that must happen before anything reads the
// device, then scans the source tree. It is the first half of a sync; the
// caller hands the result to MakePlan.
func ScanSource(o Options) (*library.Scan, error) {
	if err := CheckPaths(o); err != nil {
		return nil, err
	}
	genres, err := library.LoadGenreMap(o.GenreMap)
	if err != nil {
		return nil, err
	}
	return library.ScanTree(o.Src, library.Options{GenreMap: genres})
}

// CheckPaths refuses the destinations a sync must never touch, before the
// source tree is walked: a WSL drvfs/9p view of a Windows drive (the writes
// are not reliable and the mount can be stale — see devicefs.RefusesMount),
// and a destination that is the source or lives inside it (which would have
// the sync copy its own output).
func CheckPaths(o Options) error {
	if o.Src == "" || o.Dst == "" {
		return errors.New("both --src and --dst are required")
	}
	if err := devicefs.RefusesMount(o.Dst); err != nil {
		return err
	}
	src, err := resolveDir(o.Src, "--src")
	if err != nil {
		return err
	}
	dst, err := resolveDir(o.Dst, "--dst")
	if err != nil {
		return err
	}
	switch {
	case sameOrUnder(dst, src):
		return fmt.Errorf("--dst %s is the source tree (or inside it); the device is a different volume", o.Dst)
	case sameOrUnder(src, dst):
		return fmt.Errorf("--src %s is inside --dst %s; the sync would copy its own output", o.Src, o.Dst)
	}
	return nil
}

func resolveDir(p, what string) (string, error) {
	st, err := os.Stat(p)
	if err != nil {
		return "", fmt.Errorf("%s %s: %w", what, p, err)
	}
	if !st.IsDir() {
		return "", fmt.Errorf("%s %s is not a folder", what, p)
	}
	abs, err := filepath.Abs(p)
	if err != nil {
		return "", err
	}
	if r, err := filepath.EvalSymlinks(abs); err == nil {
		abs = r
	}
	return filepath.Clean(abs), nil
}

// sameOrUnder reports whether a is b or lives under it.
func sameOrUnder(a, b string) bool {
	if a == b {
		return true
	}
	return strings.HasPrefix(a, strings.TrimRight(b, string(filepath.Separator))+string(filepath.Separator))
}

// MakePlan decides the whole sync. It reads the destination (stat, and the
// sidecar headers, and the existing index and playlists) but writes nothing.
func MakePlan(o Options, scan *library.Scan) (*Plan, error) {
	if err := CheckPaths(o); err != nil {
		return nil, err
	}
	if scan == nil {
		return nil, errors.New("syncer: no scan to plan from")
	}

	p := &Plan{Src: o.Src, Dst: o.Dst, Albums: len(scan.Albums)}
	// The scan's own complaints are the user's business: a ".FLAC" that the
	// index does not carry is a track that will not be on the device, and
	// this is where they get told. The copy list is built from what the scan
	// INDEXED, never from a second walk of the source folder — the index and
	// the tree it describes have to be the same set of files.
	p.Warnings = append(p.Warnings, scan.Warnings...)
	for _, f := range scan.Failures {
		p.Warnings = append(p.Warnings, fmt.Sprintf("%s: metadata unreadable (%s); its record carries duration 0", f.Path, f.Reason))
	}

	musicDir := filepath.Join(o.Dst, devicefs.MusicDir)

	// keep records every destination path the plan produces, so prune can be
	// "the device minus the plan" rather than a second set of rules.
	keep := newPathSet()
	keep.add(musicDir)
	keep.add(filepath.Join(musicDir, devicefs.IndexName))

	for i := range scan.Albums {
		a := &scan.Albums[i]
		p.AlbumOrder = append(p.AlbumOrder, a.DeviceFolder)
		albumDir := filepath.Join(musicDir, a.DeviceFolder)
		keep.add(albumDir)

		// Files already in the album folder, so a name that differs only in
		// case can be renamed instead of re-sent.
		existing, _ := dirNames(albumDir)
		claimed := map[string]bool{}

		for _, t := range a.Tracks {
			p.Tracks++
			dst := filepath.Join(albumDir, t.DeviceName)
			keep.add(dst)
			op := FileOp{
				Src:    t.SrcPath,
				Dst:    dst,
				Device: devicefs.MusicDir + "/" + a.DeviceFolder + "/" + t.DeviceName,
				Album:  a.DeviceFolder,
			}
			si, err := os.Stat(t.SrcPath)
			if err != nil {
				// The scan read this file minutes ago; if it is gone now the
				// user moved it mid-sync. Copy it and let Execute report the
				// real error rather than guessing here.
				op.Reason = "source unreadable: " + err.Error()
				p.Copy = append(p.Copy, op)
				continue
			}
			op.Size, op.ModTime = si.Size(), si.ModTime()

			di, derr := os.Stat(dst)
			if derr == nil {
				same, why, err := unchanged(t.SrcPath, dst, si, di, o.Verify)
				if err != nil {
					return nil, err
				}
				op.Reason = why
				if same {
					p.Skip = append(p.Skip, op)
					p.SkipBytes += op.Size
					continue
				}
				p.Copy = append(p.Copy, op)
				p.CopyBytes += op.Size
				continue
			}

			// No file under that exact name. A case-only match is the same
			// bytes under the wrong spelling; FAT is case-insensitive, so
			// the device would find it either way, but the listing shows
			// the name on disk and the index says otherwise.
			if old, ok := caseMatch(existing, t.DeviceName, claimed); ok {
				oldPath := filepath.Join(albumDir, old)
				if oi, err := os.Stat(oldPath); err == nil {
					if same, _, err := unchanged(t.SrcPath, oldPath, si, oi, o.Verify); err == nil && same {
						claimed[old] = true
						keep.add(oldPath) // do not prune what we are about to move
						op.Src = oldPath
						op.Reason = fmt.Sprintf("already on the device as %q", old)
						p.Rename = append(p.Rename, op)
						continue
					}
				}
			}
			op.Reason = "not on the device"
			p.Copy = append(p.Copy, op)
			p.CopyBytes += op.Size
		}

		// Art. The sidecars are kept unless they are missing, invalid or the
		// user asked for a refresh, so a first Go sync does not churn every
		// album's art on a device that already looks right.
		art := ArtOp{Album: a.DeviceFolder, Dir: albumDir}
		keep.add(filepath.Join(albumDir, coreart.ArtName))
		keep.add(filepath.Join(albumDir, coreart.ThumbName))
		switch {
		case o.NoArt:
			art.Reason = "--no-art"
		default:
			art.SrcFLAC = artSource(a)
			switch {
			case art.SrcFLAC == "":
				art.Reason = "no source file to read a cover from"
			case o.ArtRefresh:
				art.Write, art.Reason = true, "--art-refresh"
			case !sidecarsValid(albumDir):
				art.Write, art.Reason = true, "missing or invalid sidecars"
			case sidecarsStale(albumDir, art.SrcFLAC):
				art.Write, art.Reason = true, "the source file is newer than folder.art"
			default:
				art.Reason = "folder.art + folder.thm are already valid"
			}
			p.Art = append(p.Art, art)
		}
	}

	// Playlists.
	pls, warns, err := planPlaylists(o, scan, musicDir)
	if err != nil {
		return nil, err
	}
	p.Playlists = pls
	p.Warnings = append(p.Warnings, warns...)
	if len(pls) > 0 {
		keep.add(filepath.Join(musicDir, devicefs.PlaylistDir))
	}
	for _, pl := range pls {
		keep.add(pl.Dst)
	}

	// Index — built here so --dry-run can report its exact size, written last
	// by Execute.
	recs := cidx.RecordsFromScan(scan)
	data := cidx.Encode(recs)
	p.Index = IndexOp{
		Dst:     filepath.Join(musicDir, devicefs.IndexName),
		Bytes:   data,
		Size:    len(data),
		Records: len(recs),
	}
	if old, err := os.ReadFile(p.Index.Dst); err == nil && len(old) == len(data) && string(old) == string(data) {
		p.Index.Unchanged = true
	}
	if w := capWarnings(scan, len(recs)); len(w) > 0 {
		p.Warnings = append(p.Warnings, w...)
	}

	// Device files.
	p.Config = !configValid(o.Dst)
	p.Log = !logValid(o.Dst)

	// Prune: the device, minus the plan.
	prune, bytes := planPrune(musicDir, keep)
	p.Prune, p.PruneBytes = prune, bytes

	return p, nil
}

// artSource is the file a cover is read from: the album's FIRST file in the
// importer's enumeration (Pos 1), which is the one tools/coreart.py picked
// too. Album.Tracks is in PLAYING order, so Tracks[0] is not it.
func artSource(a *library.Album) string {
	for i := range a.Tracks {
		if a.Tracks[i].Pos == 1 {
			return a.Tracks[i].SrcPath
		}
	}
	if len(a.Tracks) > 0 {
		return a.Tracks[0].SrcPath
	}
	return ""
}

// sidecarsValid reports whether both sidecars are present and would pass the
// firmware's own load check at exactly the dimensions it expects.
func sidecarsValid(dir string) bool {
	art, err := os.ReadFile(filepath.Join(dir, coreart.ArtName))
	if err != nil || !coreart.Valid(art, coreart.ArtSize) {
		return false
	}
	thm, err := os.ReadFile(filepath.Join(dir, coreart.ThumbName))
	return err == nil && coreart.Valid(thm, coreart.ThumbSize)
}

// sidecarsStale reports whether the album's art source has been touched since
// the sidecars on the device were rendered.
//
// This is what makes embedding a cover reach the iPod without a flag (plan §1
// decision 11). `core fix` and `core art --fetch` write a PICTURE block into
// the source FLAC and deliberately do not preserve its mtime; the sidecars
// already on the device are still structurally valid — they are the OLD cover,
// or the placeholder-less pair of an album that had none — so the validity
// test alone would keep them for ever and the user would see the new art only
// after --art-refresh.
//
// The comparison carries MTimeSlack for the same reason the copy decision
// does: FAT stores a timestamp to two seconds, so a sidecar written moments
// after its source can read back as older than it.
func sidecarsStale(dir, srcFLAC string) bool {
	si, err := os.Stat(srcFLAC)
	if err != nil {
		return false // unreadable source: the copy phase has already said so
	}
	for _, name := range []string{coreart.ArtName, coreart.ThumbName} {
		di, err := os.Stat(filepath.Join(dir, name))
		if err != nil {
			return true
		}
		if si.ModTime().Sub(di.ModTime()) > MTimeSlack {
			return true
		}
	}
	return false
}

// unchanged answers "is the file already on the device the one we would send?"
// — by size and mtime normally (FAT's two-second resolution is the slack), by
// content hash under --verify. The string is the reason, for the plan.
func unchanged(src, dst string, si, di os.FileInfo, verify bool) (bool, string, error) {
	if si.Size() != di.Size() {
		return false, fmt.Sprintf("size differs (%d -> %d)", di.Size(), si.Size()), nil
	}
	if verify {
		same, err := sameContent(src, dst)
		if err != nil {
			return false, "", err
		}
		if !same {
			return false, "content differs (--verify)", nil
		}
		return true, "content identical (--verify)", nil
	}
	d := si.ModTime().Sub(di.ModTime())
	if d < 0 {
		d = -d
	}
	if d > MTimeSlack {
		return false, fmt.Sprintf("modified %s after the copy on the device", d.Round(time.Second)), nil
	}
	return true, "same size and time", nil
}

// sameContent compares two files by SHA-256. Content, not a timestamp, is the
// only thing --verify is allowed to trust.
func sameContent(a, b string) (bool, error) {
	ha, err := fileHash(a)
	if err != nil {
		return false, err
	}
	hb, err := fileHash(b)
	if err != nil {
		return false, err
	}
	return ha == hb, nil
}

func fileHash(path string) (string, error) {
	f, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.CopyBuffer(h, f, make([]byte, CopyBufferSize)); err != nil {
		return "", err
	}
	return string(h.Sum(nil)), nil
}

// caseMatch finds a name in the folder that equals want apart from letter
// case and has not already been claimed by another track.
func caseMatch(existing []string, want string, claimed map[string]bool) (string, bool) {
	for _, n := range existing {
		if n == want || claimed[n] {
			continue
		}
		if strings.EqualFold(n, want) {
			return n, true
		}
	}
	return "", false
}

func dirNames(dir string) ([]string, error) {
	ents, err := os.ReadDir(dir)
	if err != nil {
		return nil, err
	}
	out := make([]string, 0, len(ents))
	for _, e := range ents {
		out = append(out, e.Name())
	}
	sort.Strings(out)
	return out, nil
}

// pathSet holds the destination paths the plan produces. Lookup is
// case-insensitive because the target filesystem is: on FAT, "Folder.ART" and
// "folder.art" are one file, and pruning one of them would delete the other.
type pathSet struct{ m map[string]bool }

func newPathSet() *pathSet { return &pathSet{m: map[string]bool{}} }

func (s *pathSet) add(p string)      { s.m[strings.ToLower(filepath.Clean(p))] = true }
func (s *pathSet) has(p string) bool { return s.m[strings.ToLower(filepath.Clean(p))] }

// planPrune lists everything under Music/ that the plan did not produce: album
// folders whose source is gone, files inside an album folder that are not its
// tracks or sidecars, stale playlists, and anything else that found its way
// onto the volume. Directories are listed as one entry (they are removed
// whole); their contents are not listed again.
func planPrune(musicDir string, keep *pathSet) ([]string, int64) {
	var out []string
	var bytes int64

	add := func(p string) {
		out = append(out, p)
		bytes += treeSize(p)
	}

	ents, err := os.ReadDir(musicDir)
	if err != nil {
		return nil, 0
	}
	for _, e := range ents {
		p := filepath.Join(musicDir, e.Name())
		if !e.IsDir() {
			if !keep.has(p) {
				add(p)
			}
			continue
		}
		if strings.EqualFold(e.Name(), devicefs.PlaylistDir) {
			// The playlists folder survives even when no playlist is
			// planned; only files inside it that no source names are stale.
			sub, err := os.ReadDir(p)
			if err != nil {
				continue
			}
			for _, s := range sub {
				sp := filepath.Join(p, s.Name())
				if !keep.has(sp) {
					add(sp)
				}
			}
			continue
		}
		if !keep.has(p) {
			add(p) // an album folder with no source folder any more
			continue
		}
		sub, err := os.ReadDir(p)
		if err != nil {
			continue
		}
		for _, s := range sub {
			sp := filepath.Join(p, s.Name())
			if !keep.has(sp) {
				add(sp)
			}
		}
	}
	sort.Strings(out)
	return out, bytes
}

// treeSize is how many bytes removing p would free.
func treeSize(p string) int64 {
	var n int64
	_ = filepath.Walk(p, func(_ string, fi os.FileInfo, err error) error {
		if err == nil && fi != nil && !fi.IsDir() {
			n += fi.Size()
		}
		return nil
	})
	return n
}

// configValid / logValid mirror what devicefs.EnsureConfig / EnsureLog would
// decide, without writing: the plan has to be able to say "CORECFG.DAT will be
// created" before anything is created.
func configValid(root string) bool {
	b, err := os.ReadFile(filepath.Join(root, devicefs.ConfigName))
	if err != nil {
		return false
	}
	_, ok := devicefs.ConfigFileValid(b)
	return ok
}

func logValid(root string) bool {
	path := filepath.Join(root, devicefs.LogName)
	st, err := os.Stat(path)
	if err != nil {
		return false
	}
	f, err := os.Open(path)
	if err != nil {
		return false
	}
	defer f.Close()
	head := make([]byte, devicefs.LogBlockBytes)
	if _, err := io.ReadFull(f, head); err != nil {
		return false
	}
	n, _, ok := devicefs.DecodeLogHeader(head)
	return ok && int64(n)*devicefs.LogBlockBytes == st.Size()
}

// capWarnings repeats the firmware's silent-truncation caps. Past them the
// device drops records and says nothing, so the host has to.
func capWarnings(scan *library.Scan, records int) []string {
	var w []string
	if records > library.MaxSongs {
		w = append(w, fmt.Sprintf("%d songs exceeds the firmware's LIB_MAX_SONGS (%d); the device loads the first %d and drops the rest silently",
			records, library.MaxSongs, library.MaxSongs))
	}
	if a := len(scan.Albums); a > library.MaxAlbums {
		w = append(w, fmt.Sprintf("%d albums exceeds LIB_MAX_ALBUMS (%d); the device drops the rest silently", a, library.MaxAlbums))
	}
	if g := scan.GenreCount(); g > library.MaxGenres {
		w = append(w, fmt.Sprintf("%d genres exceeds LIB_MAX_GENRES (%d); the device drops the rest silently", g, library.MaxGenres))
	}
	return w
}
