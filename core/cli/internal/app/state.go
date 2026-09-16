// Package app is core-app, the desktop application: a Gio window over
// exactly the same internal packages the `core` CLI drives.
//
// The split inside this package is the one that makes a GUI testable.
// State is a plain struct; Event is a plain struct; applying an Event
// to a State is a pure function; Runner is the only thing that starts
// goroutines; Backend is an interface with one real implementation and
// one fake. ui.go turns a State into widgets and never does I/O, and
// run.go owns the window. Everything anybody could get wrong about job
// serialisation, event ordering or cancellation is therefore reachable
// from `go test` with no display attached.
//
// One rule holds the whole thing together: State is mutated only by the
// UI goroutine. Jobs run on their own goroutines and say what happened
// by sending Events; the window's pump appends them to a queue and asks
// for a frame; Layout drains the queue into State at the top of the
// frame. No lock protects State itself, because nothing else ever
// touches it.
package app

import (
	"fmt"
	"strings"
	"time"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/artfetch"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/fwpart"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/librarian"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/organizer"
)

// LogLines is how much of the log the window keeps. It is a ring: a
// long sync prints one line per album and a big library is a thousand
// of them, and a GUI that grows without bound while a user leaves it
// open overnight is a GUI that eventually stops repainting.
const LogLines = 2000

// JobKind names one unit of work. There is at most one running at a
// time — see Runner — so this doubles as "what is the app doing".
type JobKind int

// The jobs.
const (
	JobNone JobKind = iota
	JobRefresh
	JobDryRun
	JobSync
	JobSyncPrune
	JobCheck
	JobUpdate
	JobFlash
	JobBackup
	JobEject
	JobRename
	JobInstall
	JobInspect
	JobArt
	JobFix
	JobUndoFix
)

// String is the label shown in the status line and written into the
// log, so it reads as a sentence fragment rather than as an identifier.
func (k JobKind) String() string {
	switch k {
	case JobRefresh:
		return "refresh"
	case JobDryRun:
		return "dry run"
	case JobSync:
		return "sync"
	case JobSyncPrune:
		return "sync + prune"
	case JobCheck:
		return "check for updates"
	case JobUpdate:
		return "update firmware"
	case JobFlash:
		return "flash"
	case JobBackup:
		return "backup"
	case JobEject:
		return "eject"
	case JobRename:
		return "rename"
	case JobInstall:
		return "install"
	case JobInspect:
		return "scan the library"
	case JobArt:
		return "look for cover art"
	case JobFix:
		return "fix the library"
	case JobUndoFix:
		return "undo"
	default:
		return "idle"
	}
}

// Writes reports whether this job writes to the device's raw firmware
// partition. The two that do are the two the UI guards with a typed
// confirmation, and the two whose Cancel button is a lie (see
// Runner.Cancel).
func (k JobKind) Writes() bool {
	return k == JobFlash || k == JobUpdate || k == JobInstall
}

// EventKind is what an Event says.
type EventKind int

// The kinds of Event.
const (
	// EventLog is one line for the log pane.
	EventLog EventKind = iota
	// EventProgress updates the running job's status line and bar.
	// Pct < 0 means indeterminate.
	EventProgress
	// EventDone ends a job successfully. Runner sends exactly one per
	// job that returns nil.
	EventDone
	// EventError ends a job with a failure. Runner sends exactly one
	// per job that returns an error.
	EventError
)

// String makes an Event readable in a test failure.
func (k EventKind) String() string {
	switch k {
	case EventLog:
		return "log"
	case EventProgress:
		return "progress"
	case EventDone:
		return "done"
	case EventError:
		return "error"
	default:
		return "?"
	}
}

// Event is one thing a job has to say.
type Event struct {
	Kind EventKind
	Job  JobKind
	Text string
	// Pct is 0..1 for EventProgress, or negative for "still working,
	// no idea how far".
	Pct float32
}

// Device is what the Device card shows.
type Device struct {
	Found               bool
	Path, Model, Serial string
	// Name is the friendly name the user typed, remembered in
	// config.json under Serial; Label is the FAT volume label as it is
	// actually stored on the music partition (upper-case, 11 bytes).
	// They are two fields because they really are two facts, and the
	// header is honest about the difference.
	Name, Label string
	Size        int64
	SectorSize  int
	// Installed is what fwpart made of the OSOS row and its body: Core,
	// an old Core, or something else. It is the field the launch phase
	// turns on — Other means Apple's firmware is still on this iPod and
	// there is nothing else to offer but an install — and it is the zero
	// Installed until something has actually classified the device.
	Installed             fwpart.Installed
	Tested                bool
	Volume                string
	Firmware              string
	OSOSOK                bool
	OSOSNote              string
	Err                   string
	Elevated              bool
	ElevationAdviceNeeded bool
}

// DefaultDeviceName is what an iPod with no name at all is called.
// Not "Apple iPod": that is the SCSI model string, it is the same on
// every iPod ever made, and it reads like the app failed to find out
// anything. It belongs on the details line with the path and the
// serial, not where a name goes.
const DefaultDeviceName = "iPod Video 80 GB"

// DisplayName is the name resolution, in one place: the friendly name
// the user gave this serial, else the volume label as it is stored
// (upper-case — what Explorer shows), else DefaultDeviceName.
func (d Device) DisplayName() string {
	if n := strings.TrimSpace(d.Name); n != "" {
		return n
	}
	if l := strings.TrimSpace(d.Label); l != "" {
		return l
	}
	return DefaultDeviceName
}

// Phase is which of the three screens the window is on. It is derived
// from the model rather than stored, because a phase that can disagree
// with the device it describes is a window that offers Install to
// somebody whose iPod already runs Core.
//
// "Working" is not one of them: a job runs ON a phase (an install runs
// on NotInstalled, a sync on Ready) and State.Busy() is what the buttons
// read. A fourth value would have to be left and re-entered, and the
// screen it left would be the one thing nobody could see.
type Phase int

// The phases.
const (
	// PhaseLooking: no iPod. The poll is running and the screen says how
	// to put one into disk mode.
	PhaseLooking Phase = iota
	// PhaseNotInstalled: an iPod whose firmware is not ours. One screen,
	// one button.
	PhaseNotInstalled
	// PhaseReady: an iPod running Core (either era). The main screen.
	PhaseReady
)

// String names the phase, for tests and the log.
func (p Phase) String() string {
	switch p {
	case PhaseLooking:
		return "looking"
	case PhaseNotInstalled:
		return "not installed"
	default:
		return "ready"
	}
}

// Phase is the whole launch decision, in one place.
//
// An unclassified device (Installed.Kind == "") is Ready, not
// NotInstalled: every test and canned state that predates the
// classification describes an iPod that works, and a window that
// demanded an install because nobody had filled a field in would be
// worse than one that shows the main screen for a device it has not
// finished reading.
func (s *State) Phase() Phase {
	switch {
	case !s.Device.Found:
		return PhaseLooking
	case s.Device.Installed.Kind == fwpart.Other:
		return PhaseNotInstalled
	default:
		return PhaseReady
	}
}

// Library is the volume half of the Device card.
type Library struct {
	Present               bool
	Songs, Albums, Genres int
	IndexBytes            int64
	ConfigValid, LogValid bool
	Note                  string
}

// Release is what `Check` filled in.
type Release struct {
	Checked bool
	Tag     string
	Notes   string
	Asset   string
	Err     string
}

// JobStatus is the running (or last-finished) job.
type JobStatus struct {
	Kind JobKind
	Text string
	// Pct is 0..1, or negative for indeterminate.
	Pct          float32
	Done, Failed bool
}

// Running reports whether this job is still going.
func (j *JobStatus) Running() bool { return j != nil && !j.Done && !j.Failed }

// State is the whole model. Everything on screen is a function of it.
type State struct {
	Device  Device
	Library Library
	Release Release
	// Source is the music folder; FlashFile is the image path typed
	// into the Firmware card.
	Source    string
	FlashFile string
	// CLIPath is the `core` binary beside the app, or "" when there is
	// none — the Firmware card says so rather than pretending a flash
	// would work.
	CLIPath string
	// Detecting reports that the 2-second poll is running, so the
	// Looking screen can promise to notice by itself. A screenshot or a
	// --no-detect run leaves it false and the screen says nothing it
	// cannot deliver.
	Detecting bool
	Job       *JobStatus
	Log       []string

	// Tab is which of the three screens the body shows.
	Tab Tab
	// Albums is the grid: one tile per source album, each carrying what
	// the last dry run said about it. It is empty until a dry run has
	// produced a plan, which is what the window starts as soon as it
	// has both a source folder and a volume.
	Albums []Album

	// --- the Library tab ---------------------------------------------

	// Report is the last librarian.Inspect. nil means the source has
	// not been scanned yet, which the tab says rather than showing four
	// zeroes as if the library were clean.
	Report *librarian.Report
	// ArtCands is what artfetch found for the coverless albums, keyed
	// the way the report keys them.
	ArtCands map[librarian.AlbumKey][]artfetch.Candidate
	// ArtChoices is one decision per album: take the candidate, or skip
	// it. An album with no entry is neither, and Fix leaves it alone.
	ArtChoices map[librarian.AlbumKey]librarian.Decision
	// FixNames and Organize are the two tick boxes. Both default to on
	// (NewUI and the canned states set them): the report only lists
	// things that are wrong, and a Fix all that silently did nothing
	// because two boxes started empty is worse than one that does what
	// the list says.
	FixNames, Organize bool
	// DiscFolders is the multi-disc tick, and it starts OFF: splitting a
	// flat album into Disc N folders is a change of shape, not a repair.
	DiscFolders bool
	// Category is the problem row the preview pane is showing.
	Category Category
	// Journal is the undo journal the last Fix wrote, or "" when there
	// is nothing to undo. Undo is disabled without one.
	Journal string
	// Scanned is when Report was produced, for the footer line.
	Scanned time.Time
	// LibraryChanged records that a Fix moved or renamed files since
	// the last sync, which is what makes Sync the primary button even
	// when the grid's plan is stale.
	LibraryChanged bool
}

// Tab is one of the three screens in the body.
type Tab int

// The tabs. Playlists is phase 3 and is deliberately not here: a tab
// that opens an empty pane teaches people the app is unfinished.
const (
	// TabAlbums is the grid.
	TabAlbums Tab = iota
	// TabLibrary is the "things to fix" list.
	TabLibrary
	// TabDetails is the device facts, the firmware and the log.
	TabDetails
)

// String names the tab as it is drawn.
func (t Tab) String() string {
	switch t {
	case TabLibrary:
		return "Library"
	case TabDetails:
		return "Details"
	default:
		return "Albums"
	}
}

// TileState is what one album tile says about itself.
type TileState int

// The tile states, which are the mockup's three: nothing (it is on the
// iPod), NEW (the iPod has never seen this album) and CHANGED (some of
// its tracks are about to be copied).
const (
	// TileOnDevice: every track is already on the iPod.
	TileOnDevice TileState = iota
	// TileChanged: some tracks will be copied, some are already there.
	TileChanged
	// TileNew: nothing of this album is on the iPod yet.
	TileNew
)

// Badge is the word drawn in the tile's corner, or "" for an album that
// is simply on the iPod.
func (t TileState) Badge() string {
	switch t {
	case TileNew:
		return "NEW"
	case TileChanged:
		return "CHANGED"
	default:
		return ""
	}
}

// Album is one tile.
type Album struct {
	// Dir is the source album folder, absolute. It is the thumbnail
	// cache's key and the only thing the grid needs from the disk.
	Dir string
	// Device is the album's folder name on the iPod (the locator), and
	// Title is what the tile says under the picture.
	Device string
	Title  string
	// Tracks is how many files the album holds; Copy is how many of
	// them this plan would copy, and Bytes is what those cost.
	Tracks, Copy int
	Bytes        int64
	State        TileState
	// Mod is the newest mtime among the album's files in the plan. It
	// is the second half of the thumbnail cache key: a cover embedded
	// by a Fix has to reach the grid without restarting the app.
	Mod time.Time
}

// Category is one row of the Library tab's problem list.
type Category int

// The five categories, in the order the mockup lists them, with the
// multi-disc split as its own row: it is the one change here a person
// may simply not want (a flat album folder is a legitimate way to keep
// a two-disc record), so it is separate and it starts unticked.
const (
	// CatArt is "Missing cover art".
	CatArt Category = iota
	// CatNames is "Filenames don't match tags".
	CatNames
	// CatFolders is "Folders to organize".
	CatFolders
	// CatDiscs is "Multi-disc albums" — splitting a flat folder into
	// Disc N subfolders. Off by default.
	CatDiscs
	// CatAttention is "Needs attention" — never fixed automatically.
	CatAttention
)

// Title and Sub are the two lines of the problem row.
func (c Category) Title() string {
	switch c {
	case CatNames:
		return "Filenames don't match tags"
	case CatFolders:
		return "Folders to organize"
	case CatDiscs:
		return "Multi-disc albums"
	case CatAttention:
		return "Needs attention"
	default:
		return "Missing cover art"
	}
}

// Sub is the smaller line under the title: what fixing it would do.
func (c Category) Sub() string {
	switch c {
	case CatNames:
		return "renamed from the tags"
	case CatFolders:
		return "into Album - Artist"
	case CatDiscs:
		return "into Disc N folders — off unless you tick it"
	case CatAttention:
		return "tags too thin to name a file from"
	default:
		return "looked up by artist and album"
	}
}

// Categories is the list, in order.
var Categories = []Category{CatArt, CatNames, CatFolders, CatDiscs, CatAttention}

// Count is how many things this category holds in r, and 0 for a report
// that has not been made yet.
//
// It counts TRACKS, not plan lines. organizer's plan carries the album's
// sidecars (folder.art, folder.thm, cover.jpg, an album-local .m3u) and
// the temporary hop that breaks a rename cycle as moves of their own, so
// "17 to organize" for eleven tracks is a headline that overstates the
// work by half (the L5 review's note for this slice). Reason "art",
// "playlist" and "cycle" are therefore not counted, and "disc" is
// counted in its own row rather than in Folders.
func (c Category) Count(r *librarian.Report) int {
	if r == nil {
		return 0
	}
	switch c {
	case CatNames:
		return len(TrackMoves(r.Misnamed))
	case CatFolders:
		return len(TrackMoves(FolderMoves(r)))
	case CatDiscs:
		return len(DiscMoves(r))
	case CatAttention:
		return len(r.NeedsAttention)
	default:
		return len(r.MissingArt)
	}
}

// TrackMoves keeps the moves that are a track changing its name or its
// folder, which is the only kind whose count a person can check against
// their own library.
func TrackMoves(moves []organizer.Move) []organizer.Move {
	out := make([]organizer.Move, 0, len(moves))
	for _, m := range moves {
		switch m.Reason {
		case "art", "playlist", "cycle":
		default:
			out = append(out, m)
		}
	}
	return out
}

// DiscMoves is the multi-disc split: the moves that exist only because a
// flat album whose tags say two discs would become "Disc 1" / "Disc 2".
// librarian keeps them out of Unorganized, out of the re-copy cost and out
// of the plan unless the report was inspected with the option on.
func DiscMoves(r *librarian.Report) []organizer.Move {
	if r == nil {
		return nil
	}
	return r.DiscSplit
}

// FolderMoves is everything else that changes folder.
func FolderMoves(r *librarian.Report) []organizer.Move {
	if r == nil {
		return nil
	}
	return r.Unorganized
}

// Action is what the top bar's one primary button does.
type Action int

// The actions, in the order Primary decides between them.
const (
	// ActionNone: no iPod, so the top bar has no button at all.
	ActionNone Action = iota
	// ActionInstall: an iPod that is not running Core.
	ActionInstall
	// ActionSync: the library and the iPod differ.
	ActionSync
	// ActionUpdate: a newer firmware release exists.
	ActionUpdate
	// ActionEject: nothing is left to do.
	ActionEject
)

// Label is the button's text. It names what pressing it will do, which
// is the whole discipline of a one-button top bar (plan §4: "the
// primary button's label always names what it will do").
func (a Action) Label() string {
	switch a {
	case ActionInstall:
		return "Install Core"
	case ActionSync:
		return "Sync"
	case ActionUpdate:
		return "Update"
	case ActionEject:
		return "Eject"
	default:
		return ""
	}
}

// Primary is the whole decision table for the top bar's button.
//
// Order matters and it is the mockup's: an iPod without Core can do
// nothing else; music the user has and the iPod has not is the thing
// they came for; a firmware release is next; and when there is nothing
// left, the honest offer is to let go of the drive. Eject is ALSO
// offered as a secondary button whenever Sync is primary, because
// "sync then unplug" is one session and the second half should not
// require finding another screen.
func (s *State) Primary() Action {
	switch {
	case !s.Device.Found:
		return ActionNone
	case s.Phase() == PhaseNotInstalled:
		return ActionInstall
	case s.PendingTracks() > 0 || s.LibraryChanged:
		return ActionSync
	case s.UpdateAvailable():
		return ActionUpdate
	default:
		return ActionEject
	}
}

// PendingAlbums and PendingTracks are what the last plan says is not on
// the iPod yet: albums with at least one file to copy, and the files
// themselves.
func (s *State) PendingAlbums() int {
	n := 0
	for _, a := range s.Albums {
		if a.Copy > 0 {
			n++
		}
	}
	return n
}

// PendingTracks is the track half of the same answer.
func (s *State) PendingTracks() int {
	n := 0
	for _, a := range s.Albums {
		n += a.Copy
	}
	return n
}

// PendingBytes is what those tracks weigh.
func (s *State) PendingBytes() int64 {
	var n int64
	for _, a := range s.Albums {
		n += a.Bytes
	}
	return n
}

// UpdateAvailable reports whether a release newer than what is on the
// iPod is known. "Newer" is "a different tag", not a version compare:
// `core update` re-flashes a tag on purpose, and a release whose tag
// this app cannot parse is still a release a user may want.
func (s *State) UpdateAvailable() bool {
	if !s.Release.Checked || s.Release.Tag == "" || s.Release.Err != "" {
		return false
	}
	return installedTag(s.Device.Firmware) != s.Release.Tag
}

// Busy reports whether a job is running. Every button except Cancel is
// disabled while it is true.
func (s *State) Busy() bool { return s.Job.Running() }

// CanCancel reports whether the Cancel button does anything. It is
// false for the two jobs that write the device: the flasher does not
// stop between the body write and the directory write, and the
// elevated child on Windows cannot be reached at all, so a Cancel that
// only cancelled the parent's context would report "cancelled" over a
// write that finished. The button is greyed and says why instead.
func (s *State) CanCancel() bool { return s.Busy() && !s.Job.Kind.Writes() }

// Logf appends one line to the ring.
func (s *State) Logf(format string, args ...any) {
	s.appendLines(fmt.Sprintf(format, args...))
}

// appendLines splits on newlines so a multi-line block from a
// subprocess or from the flasher arrives as separate lines, which is
// what a log pane with a scrollbar wants.
func (s *State) appendLines(text string) {
	text = strings.ReplaceAll(text, "\r\n", "\n")
	text = strings.TrimRight(text, "\n")
	if text == "" {
		// A deliberate blank line is still a line: the flasher prints
		// them to separate the plan from the result.
		s.push("")
		return
	}
	for _, line := range strings.Split(text, "\n") {
		s.push(strings.TrimRight(line, "\r"))
	}
}

func (s *State) push(line string) {
	s.Log = append(s.Log, line)
	if len(s.Log) > LogLines {
		// Copy rather than reslice: reslicing keeps the whole backing
		// array alive forever, which is the leak this ring exists to
		// avoid.
		keep := make([]string, LogLines)
		copy(keep, s.Log[len(s.Log)-LogLines:])
		s.Log = keep
	}
}

// Apply folds one Event into the State. It is the only place a State
// changes in response to a job, and it is pure: no I/O, no goroutines,
// no clock beyond the timestamps the caller already put in the text.
func (s *State) Apply(e Event) {
	switch e.Kind {
	case EventLog:
		s.appendLines(e.Text)
	case EventProgress:
		if s.Job == nil || s.Job.Kind != e.Job {
			s.Job = &JobStatus{Kind: e.Job}
		}
		if e.Text != "" {
			s.Job.Text = e.Text
		}
		s.Job.Pct = e.Pct
	case EventDone:
		if s.Job == nil || s.Job.Kind != e.Job {
			s.Job = &JobStatus{Kind: e.Job}
		}
		s.Job.Done, s.Job.Failed = true, false
		s.Job.Pct = 1
		if e.Text != "" {
			s.Job.Text = e.Text
			s.appendLines(e.Text)
		}
	case EventError:
		if s.Job == nil || s.Job.Kind != e.Job {
			s.Job = &JobStatus{Kind: e.Job}
		}
		s.Job.Done, s.Job.Failed = false, true
		s.Job.Text = e.Text
		s.appendLines("error: " + e.Text)
	}
}

// StatusLine is the sentence in the header: what is happening, or what
// happened last.
func (s *State) StatusLine() string {
	if s.Job == nil {
		return "ready"
	}
	switch {
	case s.Job.Failed:
		return s.Job.Kind.String() + " failed — " + firstLine(s.Job.Text)
	case s.Job.Done:
		if s.Job.Text != "" {
			return s.Job.Kind.String() + ": " + firstLine(s.Job.Text)
		}
		return s.Job.Kind.String() + " finished"
	default:
		if s.Job.Text != "" {
			return s.Job.Kind.String() + ": " + firstLine(s.Job.Text)
		}
		return s.Job.Kind.String() + " …"
	}
}

func firstLine(s string) string {
	if i := strings.IndexByte(s, '\n'); i >= 0 {
		return s[:i]
	}
	return s
}

// stamp is the "14:02:11" prefix the log lines carry. A sync that
// stalls on one album looks exactly like a sync that finished without
// saying so, unless the lines are timed.
func stamp(t time.Time) string { return t.Format("15:04:05") }
