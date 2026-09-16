// Package librarian is the "things to fix" list for a music library, and the
// one job that fixes them.
//
// It is a report, not a wizard (plan §1 decision 12). Inspect walks the source
// tree ONCE and answers four questions at the same time:
//
//	which albums have no cover art
//	which files are misnamed (right folder, wrong name)
//	which files are unorganized (wrong folder)
//	which files nobody can name from their tags, and why
//
// plus the cost of acting on it: how many tracks the next sync will copy again
// because their filename — which IS the device's locator — changed.
//
// Nothing in Inspect touches the network and nothing in it writes. The network
// step is Candidates, which asks internal/artfetch about the albums with no
// cover and hands back ranked matches for a human to accept; the writing step
// is Fix, which takes those decisions plus the two tick boxes and does the
// whole thing as ONE job with ONE undo journal.
//
// # What Undo does and does not put back
//
// Undo replays the journal organizer wrote, so every rename and every move
// goes back. It does NOT remove embedded art: the pre-image of a 25 MB FLAC is
// not kept, so there is nothing to put back (plan §5, "Art embedding is not
// undone by the journal"). Removing a cover again is flac.RemovePictures, by
// hand, and Undo's report says so.
//
// # The order inside Fix
//
// Covers into the files, then the moves, then the sidecars. That is not the
// order the screen reads in, and it is not negotiable: embedding a cover
// rewrites the FLAC, and the journal organizer writes records each file's size
// and mtime AS IT MOVES IT. A cover written after the rename would therefore
// make that rename un-undoable — Undo refuses a file that changed since — so
// the pictures go in first and the journal stamps the files as they then are.
// The two sidecars and cover.jpg come last, in whatever folder the album ended
// up in.
//
// A failure fetching or embedding one album's cover is recorded in
// Result.Failures and the rest of the job carries on, renames included: a
// cover download that timed out has nothing to do with a filename.
package librarian

import (
	"context"
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strings"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/artfetch"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/coreart"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flac"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/library"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/organizer"
)

// The job names an Event carries. They are strings rather than an enum
// because this package must not import internal/app (app is the Gio window;
// linking it into core.exe would pull Vulkan in behind it), and a string is
// the one shape the adapter on the app side can map without either package
// knowing the other's constants.
const (
	JobInspect = "inspect"
	JobArt     = "art"
	JobFix     = "fix"
)

// EventKind is what an Event says. The two kinds line up one for one with
// internal/app's EventLog and EventProgress; app's adapter maps them and adds
// its own terminal Done/Error, which the Runner owns there.
type EventKind int

// The kinds of Event.
const (
	// EventLog is one line for the log pane.
	EventLog EventKind = iota
	// EventProgress updates the running job's status line and bar.
	EventProgress
)

// Event is one thing a librarian job has to say. The field names and meanings
// are internal/app's Event, deliberately: the UI's adapter is a switch on Kind
// and a copy of Text and Pct, not a translation.
type Event struct {
	Kind EventKind
	// Job is JobInspect, JobArt or JobFix.
	Job  string
	Text string
	// Pct is 0..1, or negative for "still working, no idea how far".
	Pct float32
}

// ArtClient is the part of *artfetch.Client this package uses. It is an
// interface so a test can answer without a network and so the app can wrap the
// real client with its own logging.
type ArtClient interface {
	Find(ctx context.Context, q artfetch.Query) ([]artfetch.Candidate, error)
	Fetch(ctx context.Context, c artfetch.Candidate) (data []byte, mime string, err error)
}

// Options are the seams: where metadata comes from, where the journal goes,
// who does the writing, and who to tell about progress. The zero value is
// usable for Inspect (tags via flac.ReadFile, the default journal directory,
// no progress); Candidates and Fix's art phase need Art.
type Options struct {
	// Meta reads one file's metadata. Defaults to flac.ReadFile.
	Meta func(path string) (*flac.Meta, error)
	// JournalDir overrides <UserConfigDir>/core/journal.
	JournalDir string
	// Progress, when set, is called as the work happens.
	Progress func(Event)
	// Art finds and downloads cover art. Candidates and Fix need it; a nil
	// Art with art decisions to act on is an error, not a silent skip.
	Art ArtClient
	// WritePicture embeds a cover in one FLAC. Defaults to flac.WritePicture
	// with a front-cover block.
	WritePicture func(path string, data []byte, mime string) error
	// WriteSidecars renders folder.art + folder.thm for an album. Defaults
	// to coreart.WriteAlbum.
	WriteSidecars func(dir string, first *flac.Meta) error
	// DryRun makes Fix decide everything and write nothing. Result.DryRun is
	// then set and its counts say what WOULD have happened.
	DryRun bool
	// DiscFolders is organizer.Options.DiscFolders: split a FLAT multi-disc
	// album into "Disc N" subfolders. It is off by default — the device
	// takes disc and track from the TAGS, so a flat album already indexes
	// and sorts correctly, and splitting one renames every file in it.
	//
	// With it OFF the split is still REPORTED (Report.DiscSplit) and costs
	// nothing: it is not in Report.Moves, so the re-copy line does not count
	// it and Fix cannot apply it. Turn it on to inspect a tree with the
	// split IN the plan, which is what Choices.DiscFolders then ticks.
	DiscFolders bool
}

func (o Options) emit(e Event) {
	if o.Progress != nil {
		o.Progress(e)
	}
}

func (o Options) log(job, format string, args ...any) {
	o.emit(Event{Kind: EventLog, Job: job, Text: fmt.Sprintf(format, args...)})
}

func (o Options) progress(job, text string, done, total int) {
	pct := float32(-1)
	if total > 0 {
		pct = float32(done) / float32(total)
	}
	o.emit(Event{Kind: EventProgress, Job: job, Text: text, Pct: pct})
}

func (o Options) meta() func(string) (*flac.Meta, error) {
	if o.Meta != nil {
		return o.Meta
	}
	return flac.ReadFile
}

func (o Options) writePicture() func(string, []byte, string) error {
	if o.WritePicture != nil {
		return o.WritePicture
	}
	return func(path string, data []byte, mime string) error {
		return flac.WritePicture(path, flac.Picture{
			Type: flac.PictureTypeFrontCover,
			MIME: mime,
			Data: data,
		})
	}
}

func (o Options) writeSidecars() func(string, *flac.Meta) error {
	if o.WriteSidecars != nil {
		return o.WriteSidecars
	}
	return func(dir string, first *flac.Meta) error {
		_, err := coreart.WriteAlbum(dir, first)
		return err
	}
}

// AlbumKey names one album inside one report: its folder path relative to the
// report's root, with forward slashes. It is stable across the rename phase
// (it is what the album was called when it was inspected) and it is what the
// UI keys its accept/skip decisions by.
type AlbumKey string

// AlbumRef is one album the report has something to say about.
type AlbumRef struct {
	Key AlbumKey `json:"key"`
	// Dir is the album folder as it is now, absolute.
	Dir string `json:"dir"`
	// Artist and Album are what a search would be made with: the tags of
	// the art source, with the folder name as the fallback.
	Artist string `json:"artist"`
	Album  string `json:"album"`
	// FirstFLAC is the album's art source — the file at enumeration
	// position 1, which is the one the index and the sidecars read a cover
	// from (syncer.artSource).
	FirstFLAC string `json:"first_flac"`
	// Tracks is how many FLACs the album holds.
	Tracks int `json:"tracks"`
	// Files is every FLAC of the album in enumeration order (the folder's
	// own, sorted, then each "Disc N" subfolder's). Fix embeds the accepted
	// cover in all of them.
	Files []string `json:"-"`
}

// Report is everything one Inspect found. It is a description, not a plan: the
// caller ticks what it wants in Choices and hands both to Fix.
type Report struct {
	Root string `json:"root"`

	// MissingArt are the albums whose art source carries no front cover.
	MissingArt []AlbumRef `json:"missing_art"`
	// Misnamed are files in the right folder under the wrong name.
	Misnamed []organizer.Move `json:"misnamed"`
	// Unorganized are files that have to change folder (including the
	// sidecars that follow an album folder), EXCEPT the multi-disc split,
	// which is DiscSplit's.
	Unorganized []organizer.Move `json:"unorganized"`
	// DiscSplit are the moves that exist only because a flat multi-disc
	// album would be split into "Disc N" subfolders. They are their own
	// category because the split is opt-in: the device reads disc and track
	// off the TAGS, so the flat album is already right, and splitting it
	// renames every file in it. Nothing here is counted in Unorganized, in
	// the re-copy line, or applied unless Choices.DiscFolders says so — and
	// for that the report has to have been inspected with
	// Options.DiscFolders on, which is what puts these moves in Moves.
	DiscSplit []organizer.Move `json:"disc_split,omitempty"`
	// NeedsAttention is every file the organizer refuses to touch, and why.
	// Nothing here is ever fixed automatically.
	NeedsAttention []organizer.Attention `json:"needs_attention"`
	// Warnings are the scan's own remarks (a ".FLAC" that library skips).
	Warnings []string `json:"warnings,omitempty"`

	// Albums and Tracks are the whole tree, for the summary line.
	Albums int `json:"albums"`
	Tracks int `json:"tracks"`

	// RecopyTracks is how many tracks already on the iPod the next sync
	// will copy AGAIN because their device name changed. It comes from
	// organizer and it is bigger than the number of moved files: a file
	// that stays put still changes its device name when a neighbour leaves
	// its folder, because the name carries the file's position.
	RecopyTracks int `json:"recopy_tracks"`
	// RecopyBytes is the size of the moved FLACs that the library can see
	// today. It is the honest part of the cost above: the bytes of the
	// tracks whose position shifts without them moving are not counted,
	// because nothing in the plan names those files.
	RecopyBytes int64 `json:"recopy_bytes"`
	// NewTracks is how many tracks library.ScanTree cannot see today (they
	// sit below the root, or in a folder with no " - ") and will be copied
	// for the first time. NewBytes is their size.
	NewTracks int   `json:"new_tracks"`
	NewBytes  int64 `json:"new_bytes"`

	// Moves is the whole rename plan in the order organizer decided to
	// apply it — the order that keeps a rename from landing on a file that
	// has not moved out of the way yet. Misnamed and Unorganized are views
	// of it. It is not serialised: a Report decoded from JSON can be shown
	// but not applied, and Fix says so rather than half-applying it.
	Moves []organizer.Move `json:"-"`
}

// Empty reports whether there is nothing to fix and nothing to look at.
func (r *Report) Empty() bool {
	return len(r.MissingArt) == 0 && len(r.Misnamed) == 0 &&
		len(r.Unorganized) == 0 && len(r.NeedsAttention) == 0
}

// Album returns the album with this key.
func (r *Report) Album(key AlbumKey) (AlbumRef, bool) {
	for _, a := range r.MissingArt {
		if a.Key == key {
			return a, true
		}
	}
	return AlbumRef{}, false
}

// Inspect reads the tree and returns the report. It writes nothing and makes
// no network request.
//
// One organizer.Scan decides the three name problems and the re-copy cost; one
// pass over the albums decides which have no cover. The album pass reads the
// art source's metadata a second time (the scan has already read every file's
// tags) rather than holding every album's PICTURE bytes in memory through the
// scan — a thousand-album library would be gigabytes of cover data kept alive
// for one nil check.
func Inspect(ctx context.Context, src string, o Options) (*Report, error) {
	if ctx == nil {
		ctx = context.Background()
	}
	if strings.TrimSpace(src) == "" {
		return nil, errors.New("fix: no source folder (pass --src)")
	}
	root, err := filepath.Abs(src)
	if err != nil {
		return nil, err
	}
	if st, err := os.Stat(root); err != nil || !st.IsDir() {
		return nil, fmt.Errorf("source tree not found: %s  (pass --src)", src)
	}

	o.progress(JobInspect, "reading the tags", 0, 0)
	plan, err := organizer.Scan(organizer.Options{
		Root:        root,
		Meta:        o.Meta,
		JournalDir:  o.JournalDir,
		DiscFolders: o.DiscFolders,
		Progress: func(e organizer.Event) {
			if e.Phase == "scan" {
				o.progress(JobInspect, e.Album, e.Done, e.Total)
			}
		},
	})
	if err != nil {
		return nil, err
	}
	if err := ctx.Err(); err != nil {
		return nil, err
	}

	rep := &Report{
		Root:           root,
		NeedsAttention: plan.Attention,
		Warnings:       plan.Warnings,
		RecopyTracks:   plan.TracksRecopied,
		NewTracks:      plan.TracksNew,
		Moves:          plan.Moves,
		// With the split off these are the moves the option WOULD make;
		// with it on they are picked out of the plan below.
		DiscSplit: plan.DiscSplit,
	}
	for i, m := range plan.Moves {
		switch reason := effectiveReason(plan.Moves, i); {
		case reason == "disc":
			rep.DiscSplit = append(rep.DiscSplit, m)
		case isRename(reason):
			rep.Misnamed = append(rep.Misnamed, m)
		default:
			rep.Unorganized = append(rep.Unorganized, m)
		}
		if strings.HasSuffix(m.From, ".flac") {
			// Which side of the cost a moved track is on: one the library
			// can see today is already on the iPod under a name that is
			// about to change (a re-copy); one it cannot see has never been
			// there (a first copy).
			if visibleToLibrary(root, m.From) {
				rep.RecopyBytes += m.Bytes
			} else {
				rep.NewBytes += m.Bytes
			}
		}
	}

	albums, err := albumsIn(root)
	if err != nil {
		return nil, err
	}
	read := o.meta()
	for i, a := range albums {
		if err := ctx.Err(); err != nil {
			return nil, err
		}
		rep.Albums++
		rep.Tracks += len(a.files)
		o.progress(JobInspect, rel(root, a.dir), i+1, len(albums))
		m, err := read(a.files[0])
		if err != nil {
			// The scan has already put this file on the attention list
			// with the same reason; saying it twice helps nobody.
			continue
		}
		if m.FrontCover() != nil {
			continue
		}
		artist, album := albumQuery(a.dir, m)
		rep.MissingArt = append(rep.MissingArt, AlbumRef{
			Key:       AlbumKey(rel(root, a.dir)),
			Dir:       a.dir,
			Artist:    artist,
			Album:     album,
			FirstFLAC: a.files[0],
			Tracks:    len(a.files),
			Files:     a.files,
		})
	}
	o.log(JobInspect, "%d album(s), %d track(s): %d need art, %d misnamed, %d to re-folder, "+
		"%d in flat multi-disc albums, %d need attention",
		rep.Albums, rep.Tracks, len(rep.MissingArt), len(rep.Misnamed),
		len(rep.Unorganized), len(rep.DiscSplit), len(rep.NeedsAttention))
	return rep, nil
}

// ------------------------------------------------------------- the albums

// album is one album folder and its FLACs, in enumeration order.
type album struct {
	dir   string
	files []string
}

// albumsIn finds every album folder under root and lists its FLACs in the
// order library.ScanTree enumerates them: the folder's own files sorted, then
// each "Disc N" subfolder's files sorted.
//
// "Every folder that holds FLACs" is wider than library's rule (which only
// looks one level down, at folders whose name has a " - " in it) on purpose:
// an album sitting in Downloads\X\ is invisible to the index today and is
// exactly the thing this report exists to point at.
func albumsIn(root string) ([]album, error) {
	flacs := map[string][]string{}
	err := filepath.WalkDir(root, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if d.IsDir() || d.Type()&fs.ModeSymlink != 0 {
			return nil
		}
		base := d.Name()
		// A leading dot hides a file from library's glob, and the extension
		// match is case-sensitive there too: ".FLAC" is not a track.
		if strings.HasPrefix(base, ".") || !strings.HasSuffix(base, ".flac") {
			return nil
		}
		dir := filepath.Dir(p)
		flacs[dir] = append(flacs[dir], base)
		return nil
	})
	if err != nil {
		return nil, err
	}

	dirs := map[string]bool{}
	for d := range flacs {
		if isDiscDir(d) {
			dirs[filepath.Dir(d)] = true
		} else {
			dirs[d] = true
		}
	}
	var keys []string
	for d := range dirs {
		keys = append(keys, d)
	}
	sort.Strings(keys)

	var out []album
	for _, dir := range keys {
		own := append([]string(nil), flacs[dir]...)
		sort.Strings(own)
		a := album{dir: dir}
		for _, base := range own {
			a.files = append(a.files, filepath.Join(dir, base))
		}
		var discs []string
		for d := range flacs {
			if filepath.Dir(d) == dir && isDiscDir(d) {
				discs = append(discs, d)
			}
		}
		sort.Strings(discs)
		for _, d := range discs {
			sub := append([]string(nil), flacs[d]...)
			sort.Strings(sub)
			for _, base := range sub {
				a.files = append(a.files, filepath.Join(d, base))
			}
		}
		if len(a.files) > 0 {
			out = append(out, a)
		}
	}
	return out, nil
}

// visibleToLibrary reports whether library.ScanTree can see this file today:
// its album folder is an immediate child of the root and its name carries the
// " - " that SplitAlbumArtist needs. An album in Downloads\X\ is not visible,
// which is why its tracks are first copies rather than re-copies.
func visibleToLibrary(root, file string) bool {
	albumDir := albumDirOf(file)
	if filepath.Dir(albumDir) != root {
		return false
	}
	artist, _ := library.SplitAlbumArtist(filepath.Base(albumDir))
	return artist != ""
}

// isDiscDir is library's own test: the enumeration keys off the "Disc "
// prefix, so that is what this matches.
func isDiscDir(dir string) bool { return strings.HasPrefix(filepath.Base(dir), "Disc ") }

// albumDirOf is the album folder a track belongs to: its own folder, or the
// parent when it sits in a "Disc N" subfolder.
func albumDirOf(file string) string {
	d := filepath.Dir(file)
	if isDiscDir(d) {
		return filepath.Dir(d)
	}
	return d
}

// albumQuery decides what to search for. The tags are asked first — they are
// the truth this program trusts everywhere else — and the folder name is the
// fallback, split on its LAST " - " the way library.SplitAlbumArtist does,
// because the source convention is "Album - Artist".
func albumQuery(dir string, m *flac.Meta) (artist, album string) {
	album = strings.TrimSpace(m.Tag("album"))
	artist = strings.TrimSpace(m.Tag("albumartist", "album_artist", "album artist", "artist"))
	if album == "" || artist == "" {
		fArtist, fAlbum := library.SplitAlbumArtist(filepath.Base(dir))
		if album == "" {
			album = fAlbum
		}
		if artist == "" {
			artist = fArtist
		}
	}
	return artist, album
}

// --------------------------------------------------------------- helpers

// effectiveReason is a move's category, seeing through the temporary hop that
// breaks a rename cycle: that hop carries the reason "cycle", and what it is
// really part of is whatever its partner — the move that starts where the hop
// ended — is part of.
func effectiveReason(moves []organizer.Move, i int) string {
	for hops := 0; hops < len(moves); hops++ {
		if moves[i].Reason != "cycle" {
			return moves[i].Reason
		}
		next := -1
		for j := i + 1; j < len(moves); j++ {
			if samePath(moves[j].From, moves[i].To) {
				next = j
				break
			}
		}
		if next < 0 {
			return "move"
		}
		i = next
	}
	return "move"
}

func isRename(reason string) bool { return reason == "rename" }

// caseFoldPaths is whether two paths differing only in case name the same
// file — organizer's rule, for the same reason (Windows and macOS say yes).
var caseFoldPaths = runtime.GOOS == "windows" || runtime.GOOS == "darwin"

func samePath(a, b string) bool {
	a, b = filepath.Clean(a), filepath.Clean(b)
	if caseFoldPaths {
		return strings.EqualFold(a, b)
	}
	return a == b
}

func rel(root, p string) string {
	if r, err := filepath.Rel(root, p); err == nil && !strings.HasPrefix(r, "..") {
		return filepath.ToSlash(r)
	}
	return p
}
