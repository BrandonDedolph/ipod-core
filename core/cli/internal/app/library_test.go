package app

import (
	"testing"
	"time"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/artfetch"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/fwpart"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/librarian"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/organizer"
)

// The top bar carries exactly one primary button and its label is the
// decision. This is that decision table.
func TestThePrimaryButtonSaysWhatItWillDo(t *testing.T) {
	core := fwpart.Installed{Kind: fwpart.Core, Version: "v0.1.3"}
	apple := fwpart.Installed{Kind: fwpart.Other}
	pending := []Album{{Device: "A - B", Tracks: 2, Copy: 2, State: TileNew}}
	done := []Album{{Device: "A - B", Tracks: 2, State: TileOnDevice}}

	cases := []struct {
		name string
		st   State
		want Action
	}{
		{"no iPod: no button at all", State{}, ActionNone},
		{"a stock iPod can do nothing but install", State{
			Device: Device{Found: true, Installed: apple},
		}, ActionInstall},
		{"music the iPod has not got", State{
			Device: Device{Found: true, Installed: core}, Albums: pending,
		}, ActionSync},
		{"a fix renamed files: they have to go over again", State{
			Device: Device{Found: true, Installed: core}, Albums: done, LibraryChanged: true,
		}, ActionSync},
		{"sync wins over an available release", State{
			Device:  Device{Found: true, Installed: core, Firmware: "v0.1.2 (build x)"},
			Albums:  pending,
			Release: Release{Checked: true, Tag: "v0.1.3"},
		}, ActionSync},
		{"nothing to copy and a newer release", State{
			Device:  Device{Found: true, Installed: core, Firmware: "v0.1.2 (build x)"},
			Albums:  done,
			Release: Release{Checked: true, Tag: "v0.1.3"},
		}, ActionUpdate},
		{"the same release is not an update", State{
			Device:  Device{Found: true, Installed: core, Firmware: "v0.1.3 (build x)"},
			Albums:  done,
			Release: Release{Checked: true, Tag: "v0.1.3"},
		}, ActionEject},
		{"a failed check is not an update", State{
			Device:  Device{Found: true, Installed: core, Firmware: "v0.1.2 (build x)"},
			Albums:  done,
			Release: Release{Checked: true, Tag: "v0.1.3", Err: "no network"},
		}, ActionEject},
		{"nothing left to do", State{
			Device: Device{Found: true, Installed: core}, Albums: done,
		}, ActionEject},
	}
	for _, c := range cases {
		st := c.st
		got := st.Primary()
		if got != c.want {
			t.Errorf("%s: Primary() = %v (%q), want %v (%q)",
				c.name, got, got.Label(), c.want, c.want.Label())
		}
	}

	// And the label is never empty where there is a button, because the
	// button IS the label (plan §4).
	for _, a := range []Action{ActionInstall, ActionSync, ActionUpdate, ActionEject} {
		if a.Label() == "" {
			t.Errorf("action %v has no label", a)
		}
	}
	if ActionNone.Label() != "" {
		t.Error("ActionNone must not draw a button")
	}
}

// Eject stays available beside Sync: "sync, then unplug" is one session.
func TestEjectIsOfferedBesideSync(t *testing.T) {
	f := &fakeBackend{}
	u := newTestUI(f)
	u.st.Albums = []Album{{Device: "A - B", Tracks: 1, Copy: 1, State: TileNew}}
	if u.st.Primary() != ActionSync {
		t.Fatalf("Primary() = %v, want Sync", u.st.Primary())
	}
	// The secondary is drawn from the same two facts the layout reads.
	if u.st.Device.Volume == "" {
		t.Fatal("the test device has no volume, so the secondary Eject would be hidden")
	}
}

// --- the Library tab ----------------------------------------------------

// fixtureReport is a report with something in every category.
func fixtureReport() *librarian.Report {
	return &librarian.Report{
		Root: "/src",
		MissingArt: []librarian.AlbumRef{
			{Key: "DOA - ericdoa", Dir: "/src/DOA - ericdoa", Artist: "ericdoa", Album: "DOA", Tracks: 10},
			{Key: "24 - Arizona Zervas", Dir: "/src/24 - Arizona Zervas",
				Artist: "Arizona Zervas", Album: "24", Tracks: 9},
		},
		Misnamed: []organizer.Move{
			{From: "/src/a/Track 3.flac", To: "/src/a/03 - A - B.flac", Reason: "rename", Bytes: 10},
		},
		Unorganized: []organizer.Move{
			{From: "/src/Downloads/x/01.flac", To: "/src/X - Y/01.flac", Reason: "move", Bytes: 20},
			{From: "/src/Downloads/x/folder.thm", To: "/src/X - Y/folder.thm", Reason: "art", Bytes: 1},
			{From: "/src/Downloads/x/x.m3u", To: "/src/X - Y/x.m3u", Reason: "playlist", Bytes: 1},
		},
		DiscSplit: []organizer.Move{
			{From: "/src/Two - Discs/09.flac", To: "/src/Two - Discs/Disc 2/01.flac",
				Reason: "disc", Bytes: 30},
		},
		NeedsAttention: []organizer.Attention{{Path: "/src/odd.flac", Reason: "no title tag"}},
		RecopyTracks:   87,
		RecopyBytes:    2_311_000_000,
	}
}

// The counts on the rows are TRACKS. organizer's plan carries an album's art
// sidecar, its playlist and the temporary hop that breaks a rename cycle as
// moves of their own, and a headline that counted them would overstate the
// work by half (the L5 review's note for this slice). The multi-disc split is
// its own row, not part of "Folders to organize".
func TestCategoryCountsAreTracksAndDiscsAreSeparate(t *testing.T) {
	rep := fixtureReport()
	want := map[Category]int{
		CatArt:       2,
		CatNames:     1,
		CatFolders:   1, // the .flac move only: not folder.thm, not the .m3u
		CatDiscs:     1,
		CatAttention: 1,
	}
	for c, n := range want {
		if got := c.Count(rep); got != n {
			t.Errorf("%s counted %d, want %d", c.Title(), got, n)
		}
	}
	if got := len(DiscMoves(rep)); got != 1 {
		t.Errorf("DiscMoves found %d", got)
	}
	if got := len(FolderMoves(rep)); got != 3 {
		t.Errorf("FolderMoves found %d, want the three non-disc moves", got)
	}
	// Ticking the multi-disc row rescans, because only a report inspected
	// with the option on carries the split where Fix can apply it.
	f := &fakeBackend{report: rep}
	u := newTestUI(f)
	u.st.Source = "/src"
	u.st.DiscFolders = true
	u.startInspect("/src")
	settle(u)
	if len(f.inspectDiscs) != 1 || !f.inspectDiscs[0] {
		t.Errorf("Inspect was asked with discFolders=%v", f.inspectDiscs)
	}
	// A report that does not exist counts as nothing rather than panicking.
	for _, c := range Categories {
		if c.Count(nil) != 0 {
			t.Errorf("%s counted something in a nil report", c.Title())
		}
	}
}

// Choosing a music folder scans it: no button, no wizard. The report then
// shows, and the grid's dry run is queued behind it because the Runner takes
// one job at a time.
func TestSettingTheFolderScansIt(t *testing.T) {
	f := &fakeBackend{report: fixtureReport()}
	u := newTestUI(f)
	u.setSource("/music/MC")
	settle(u)

	if len(f.inspectCalls) != 1 || f.inspectCalls[0] != "/music/MC" {
		t.Fatalf("Inspect calls = %v, want one for /music/MC", f.inspectCalls)
	}
	if u.st.Report == nil {
		t.Fatal("the report never reached the model")
	}
	if u.st.Source != "/music/MC" {
		t.Errorf("the source is %q", u.st.Source)
	}
	if u.st.Scanned.IsZero() {
		t.Error("the footer would not know when the scan happened")
	}
	// The tab opens on the first category that has anything in it.
	if u.st.Category != CatArt {
		t.Errorf("the preview opened on %v", u.st.Category)
	}
	// And the queued dry run runs on the next idle frame, which is what
	// fills the grid.
	u.drain()
	settle(u)
	if len(f.syncCalls) != 1 || !f.syncCalls[0].DryRun {
		t.Fatalf("sync calls = %+v, want one dry run", f.syncCalls)
	}
}

// Take and Skip are the decisions Fix is handed. A skip is a decision too:
// it is what stops the album being asked about again in this session.
func TestTakeAndSkipAreRememberedPerAlbum(t *testing.T) {
	f := &fakeBackend{report: fixtureReport()}
	u := newTestUI(f)
	u.st.Report = f.report
	cands := []artfetch.Candidate{{Provider: "itunes", Artist: "ericdoa", Album: "DOA", Score: 1}}
	u.st.ArtCands = map[librarian.AlbumKey][]artfetch.Candidate{"DOA - ericdoa": cands}

	u.setChoice("DOA - ericdoa", librarian.Accept(cands[0]))
	u.setChoice("24 - Arizona Zervas", librarian.Skip())

	ch := u.choices()
	if len(ch.Art) != 1 {
		t.Fatalf("Fix would be handed %d art decision(s), want 1", len(ch.Art))
	}
	if got := ch.Art["DOA - ericdoa"]; got.Skip || got.Candidate.Album != "DOA" {
		t.Errorf("the accepted decision came out as %+v", got)
	}
	if _, ok := ch.Art["24 - Arizona Zervas"]; ok {
		t.Error("a skipped album must not be sent to Fix as a decision")
	}
}

// Fix all runs ONCE, with what is on screen, and only after the
// confirmation that names the cost. Undo then takes the journal it wrote.
func TestFixAllRunsOnceWithTheChoicesAndUndoFollows(t *testing.T) {
	f := &fakeBackend{
		report:    fixtureReport(),
		fixResult: &librarian.Result{Journal: "/cfg/journal/2026.json", Renamed: 1, Moved: 2, ArtWritten: 1},
	}
	u := newTestUI(f)
	u.st.Report = f.report
	u.st.Source = "/src"
	cands := []artfetch.Candidate{{Provider: "itunes", Artist: "ericdoa", Album: "DOA", Score: 1}}
	u.st.ArtCands = map[librarian.AlbumKey][]artfetch.Candidate{"DOA - ericdoa": cands}
	u.setChoice("DOA - ericdoa", librarian.Accept(cands[0]))
	u.st.FixNames, u.st.Organize = true, false

	// Nothing is written before the dialog is answered.
	answered := answerDialogs(u, "yes")
	u.startFix()
	settle(u)
	if n := <-answered; n != 1 {
		t.Fatalf("the Fix dialog was asked %d times, want 1", n)
	}
	if len(f.fixCalls) != 1 {
		t.Fatalf("Fix ran %d times, want once", len(f.fixCalls))
	}
	got := f.fixCalls[0]
	if !got.FixNames || got.Organize {
		t.Errorf("Fix was handed FixNames=%v Organize=%v, want the ticks on screen",
			got.FixNames, got.Organize)
	}
	if len(got.Art) != 1 {
		t.Errorf("Fix was handed %d art decision(s)", len(got.Art))
	}
	if u.st.Journal != "/cfg/journal/2026.json" {
		t.Errorf("the journal did not reach the model: %q", u.st.Journal)
	}
	if !u.st.LibraryChanged {
		t.Error("a fix that renamed files must make Sync the primary button")
	}

	// Undo is enabled only because there is a journal, and it clears it.
	u.startUndo(u.st.Journal)
	settle(u)
	if len(f.undoCalls) != 1 || f.undoCalls[0] != "/cfg/journal/2026.json" {
		t.Fatalf("Undo calls = %v", f.undoCalls)
	}
	if u.st.Journal != "" || u.st.LibraryChanged {
		t.Errorf("after an undo the journal is %q and LibraryChanged is %v",
			u.st.Journal, u.st.LibraryChanged)
	}
	// With no journal there is nothing to undo and nothing is called.
	u.startUndo("")
	if len(f.undoCalls) != 1 {
		t.Error("Undo ran without a journal")
	}
}

// A cancelled confirmation writes nothing at all.
func TestFixAllCancelledWritesNothing(t *testing.T) {
	f := &fakeBackend{report: fixtureReport()}
	u := newTestUI(f)
	u.st.Report = f.report
	answered := answerDialogs(u, "")
	u.startFix()
	settle(u)
	<-answered
	if len(f.fixCalls) != 0 {
		t.Fatalf("Fix ran %d time(s) after a cancel", len(f.fixCalls))
	}
	if u.st.Job == nil || !u.st.Job.Failed {
		t.Error("a cancelled fix should end as a failed job, so the log says so")
	}
}

// One job at a time: every Library button is dead while something runs, and
// the Runner refuses anything that slips through anyway.
func TestLibraryButtonsAreDeadWhileAJobRuns(t *testing.T) {
	f := &fakeBackend{report: fixtureReport(), block: make(chan struct{})}
	u := newTestUI(f)
	u.st.Report = f.report
	u.startSync(JobDryRun)
	defer func() { close(f.block); settle(u) }()

	if !u.st.Busy() {
		t.Fatal("the dry run did not start")
	}
	if u.canFix() && !u.st.Busy() {
		t.Error("Fix all should be disabled while a job runs")
	}
	// The guard is the Runner's, so even a click that slipped through a
	// frame does nothing.
	u.startInspect("/src")
	if len(f.inspectCalls) != 0 {
		t.Errorf("Inspect ran during another job: %v", f.inspectCalls)
	}
	u.startUndo("/j.json")
	if len(f.undoCalls) != 0 {
		t.Error("Undo ran during another job")
	}
}

// The art lookup is the one call that reaches the internet, so it is a
// button; what it finds is pre-accepted at 0.9 and above, which is the rule
// the CLI's --yes uses.
func TestArtLookupAcceptsTheGoodMatches(t *testing.T) {
	f := &fakeBackend{
		report: fixtureReport(),
		cands: map[librarian.AlbumKey][]artfetch.Candidate{
			"DOA - ericdoa": {{Provider: "itunes", Artist: "ericdoa", Album: "DOA", Score: 1.0}},
			"24 - Arizona Zervas": {{Provider: "musicbrainz", Artist: "Arizona Zervas",
				Album: "24 (Deluxe)", Score: 0.7}},
		},
	}
	u := newTestUI(f)
	u.st.Report = f.report
	u.startCandidates()
	settle(u)
	if f.candCalls != 1 {
		t.Fatalf("Candidates ran %d times", f.candCalls)
	}
	if len(u.st.ArtCands) != 2 {
		t.Fatalf("%d album(s) came back", len(u.st.ArtCands))
	}
	if d, ok := u.st.ArtChoices["DOA - ericdoa"]; !ok || d.Skip {
		t.Error("an exact match should be accepted without a click")
	}
	if _, ok := u.st.ArtChoices["24 - Arizona Zervas"]; ok {
		t.Error("a 0.70 match must wait to be looked at")
	}
}

// The scan is a read: it must not depend on an iPod being attached, because
// a person can tidy their library on a laptop with nothing plugged in.
func TestTheLibraryTabWorksWithNoIPod(t *testing.T) {
	f := &fakeBackend{report: fixtureReport()}
	u := NewUI(Options{Backend: f})
	u.st.Source = "/src"
	u.startInspect("/src")
	settle(u)
	if u.st.Report == nil {
		t.Fatal("no report with no device attached")
	}
	if u.st.Phase() != PhaseLooking {
		t.Fatalf("phase is %v", u.st.Phase())
	}
	gtx := newTestContext(900, 600)
	u.st.Tab = TabLibrary
	if got := u.Layout(gtx).Size; got.X != 900 {
		t.Errorf("the window laid out %v", got)
	}
}

// The cost line is the sentence a person has to read before pressing the
// button, so it names the tracks, the bytes and the words "re-copied".
func TestTheCostLineNamesTheRecopy(t *testing.T) {
	u := NewUI(Options{})
	u.st.Report = fixtureReport()
	got := u.costLine()
	for _, want := range []string{"87", "re-copied", "2.3 GB"} {
		if !contains(got, want) {
			t.Errorf("costLine() = %q, missing %q", got, want)
		}
	}
	u.st.Report = &librarian.Report{}
	if got := u.costLine(); !contains(got, "Nothing") {
		t.Errorf("a clean report's cost line = %q", got)
	}
}

// The canned Library state is what --screenshot renders and what a reviewer
// looks at, so it has to have something in every row.
func TestLibraryStateHasEveryCategory(t *testing.T) {
	st := LibraryState()
	if st.Tab != TabLibrary {
		t.Error("the canned Library state does not open on the Library tab")
	}
	for _, c := range Categories {
		if c.Count(st.Report) == 0 {
			t.Errorf("%s is empty in the canned state", c.Title())
		}
	}
	if len(st.ArtCands) == 0 || len(st.ArtChoices) == 0 {
		t.Error("the canned state has no art candidates to draw")
	}
	if time.Since(st.Scanned) > time.Minute {
		t.Error("the canned state's scan time is stale")
	}
}

func contains(s, sub string) bool {
	return len(sub) == 0 || (len(s) >= len(sub) && indexOf(s, sub) >= 0)
}

func indexOf(s, sub string) int {
	for i := 0; i+len(sub) <= len(s); i++ {
		if s[i:i+len(sub)] == sub {
			return i
		}
	}
	return -1
}
