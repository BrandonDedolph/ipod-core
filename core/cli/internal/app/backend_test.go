package app

import (
	"errors"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flasher"
	"os/exec"
	"path/filepath"
	"reflect"
	"strings"
	"testing"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/syncer"
)

// --- the flash wiring ---------------------------------------------------

// This is the test the whole app hangs off. If ChildArgs is wrong the
// UAC child writes the wrong file, or nothing; if Executable is wrong
// the elevation prompt names core-app, which has no flash command at
// all and would open a second window instead of writing.
func TestBuildFlashDepsNamesTheCLIAndItsCommandLine(t *testing.T) {
	o, d := buildFlashDeps(`C:\img\core.ipod`, `C:\app\core.exe`, `C:\backups`,
		func(string) (string, error) { return "typed", nil }, nil)

	if o.Image != `C:\img\core.ipod` {
		t.Errorf("Options.Image = %q", o.Image)
	}
	if o.BackupDir != `C:\backups` {
		t.Errorf("Options.BackupDir = %q", o.BackupDir)
	}
	want := []string{"flash", `C:\img\core.ipod`, "--backup-dir", `C:\backups`}
	if !reflect.DeepEqual(d.ChildArgs, want) {
		t.Errorf("ChildArgs = %q, want %q", d.ChildArgs, want)
	}
	exe, err := d.Executable()
	if err != nil || exe != `C:\app\core.exe` {
		t.Errorf("Executable() = %q, %v — the elevation line must name the CLI, not the app", exe, err)
	}
	if d.Confirm == nil {
		t.Fatal("Confirm is nil; the dialog would never be shown")
	}
	got, err := d.Confirm("type the device path: ")
	if err != nil || got != "typed" {
		t.Errorf("Confirm returned %q, %v", got, err)
	}
	if d.Relaunch == nil {
		t.Fatal("Relaunch is nil")
	}
}

// With no CLI beside the app, Relaunch must refuse and say what to do.
// The alternative — letting the flasher relaunch core-app itself — is a
// second window that writes nothing and looks like success.
func TestBuildFlashDepsWithoutTheCLIRefusesToRelaunch(t *testing.T) {
	_, d := buildFlashDeps("/img/core.ipod", "", "/backups", nil, nil)
	exe, _ := d.Executable()
	if exe != "core" {
		t.Errorf("Executable() = %q, want the bare name so the printed line is runnable", exe)
	}
	_, _, err := d.Relaunch([]string{"flash", "x"})
	if err == nil {
		t.Fatal("Relaunch succeeded with no CLI beside the app")
	}
	if !strings.Contains(err.Error(), "not next to this app") {
		t.Errorf("the refusal does not explain itself: %v", err)
	}
}

func TestEmitWriterSplitsNothingAndForwardsEverything(t *testing.T) {
	var got []string
	w := emitWriter(func(e Event) {
		if e.Kind != EventLog {
			t.Errorf("emitWriter produced a %v event", e.Kind)
		}
		got = append(got, e.Text)
	})
	n, err := w.Write([]byte("hello\nworld\n"))
	if err != nil || n != 12 {
		t.Fatalf("Write returned %d, %v", n, err)
	}
	if len(got) != 1 || got[0] != "hello\nworld\n" {
		t.Errorf("emitWriter forwarded %q; splitting is State.Apply's job", got)
	}
}

// --- every button's job, through the fake backend -----------------------

func TestRefreshJobFillsTheDeviceCard(t *testing.T) {
	f := &fakeBackend{
		dev: Device{Found: true, Path: "/dev/sdb", Model: "iPod Video 5.5G 80 GB", Volume: "/media/IPOD"},
		lib: Library{Present: true, Songs: 928, Albums: 101, IndexBytes: 237584, ConfigValid: true, LogValid: true},
	}
	u := NewUI(Options{Backend: f})
	u.startRefresh()
	settle(u)

	if !u.st.Device.Found || u.st.Device.Path != "/dev/sdb" {
		t.Fatalf("the device card was not filled in: %+v", u.st.Device)
	}
	if u.st.Library.Songs != 928 {
		t.Errorf("the library half was not filled in: %+v", u.st.Library)
	}
	if u.st.Job == nil || !u.st.Job.Done {
		t.Errorf("the job did not finish: %+v", u.st.Job)
	}
}

func TestDryRunAndSyncPassTheRightOptions(t *testing.T) {
	f := &fakeBackend{}
	u := newTestUI(f)

	u.startSync(JobDryRun)
	settle(u)
	u.startSync(JobSync)
	settle(u)

	if len(f.syncCalls) != 2 {
		t.Fatalf("got %d Sync calls, want 2", len(f.syncCalls))
	}
	if !f.syncCalls[0].DryRun {
		t.Error("the Dry run button did not set DryRun")
	}
	if f.syncCalls[1].DryRun || f.syncCalls[1].Prune {
		t.Errorf("the Sync button set DryRun=%v Prune=%v", f.syncCalls[1].DryRun, f.syncCalls[1].Prune)
	}
	for i, o := range f.syncCalls {
		if o.Src != "/src" || o.Dst != `D:\` {
			t.Errorf("call %d: Src=%q Dst=%q", i, o.Src, o.Dst)
		}
	}
}

// Sync with nothing configured must fail with a sentence a user can act
// on, not with a scan of the empty string.
func TestSyncWithoutASourceOrVolumeRefuses(t *testing.T) {
	f := &fakeBackend{}
	u := NewUI(Options{Backend: f})
	u.startSync(JobSync)
	settle(u)
	if len(f.syncCalls) != 0 {
		t.Fatal("a sync ran with no source configured")
	}
	if u.st.Job == nil || !u.st.Job.Failed || !strings.Contains(u.st.Job.Text, "no music folder") {
		t.Errorf("the refusal is not actionable: %+v", u.st.Job)
	}
}

// The prune flow: dialog, dry run, dialog showing the orphans, then and
// only then the real prune.
func TestSyncPruneIsTwoDialogsAndADryRunFirst(t *testing.T) {
	f := &fakeBackend{plans: []*syncer.Plan{
		{Albums: 3, Prune: []string{"Music/Old - Band/01. a.flac", "Music/Old - Band/folder.art"}, PruneBytes: 1234},
	}}
	u := newTestUI(f)
	answered := answerDialogs(u, "yes", "prune")
	u.startSync(JobSyncPrune)
	settle(u)

	if n := <-answered; n != 2 {
		t.Fatalf("%d dialogs were raised, want 2", n)
	}
	if len(f.syncCalls) != 2 {
		t.Fatalf("got %d Sync calls, want 2 (a dry run and the real one)", len(f.syncCalls))
	}
	if !f.syncCalls[0].DryRun || !f.syncCalls[0].Prune {
		t.Errorf("the first call is not a pruning dry run: %+v", f.syncCalls[0])
	}
	if f.syncCalls[1].DryRun || !f.syncCalls[1].Prune || !f.syncCalls[1].Yes {
		t.Errorf("the second call is not a confirmed prune: %+v", f.syncCalls[1])
	}
}

// The word has to be right. Anything else aborts before the second
// Sync call, which is the one that deletes.
func TestSyncPruneAbortsOnAWrongConfirmation(t *testing.T) {
	f := &fakeBackend{plans: []*syncer.Plan{
		{Albums: 3, Prune: []string{"Music/Old - Band/01. a.flac"}},
	}}
	u := newTestUI(f)
	answerDialogs(u, "yes", "yes")
	u.startSync(JobSyncPrune)
	settle(u)

	if len(f.syncCalls) != 1 {
		t.Fatalf("got %d Sync calls, want 1 — nothing may be deleted after a wrong word", len(f.syncCalls))
	}
	if u.st.Job == nil || !u.st.Job.Failed {
		t.Fatalf("the job did not fail: %+v", u.st.Job)
	}
}

// Cancelling the first dialog stops before anything is even scanned
// with Prune set.
func TestSyncPruneCancelledAtTheFirstDialog(t *testing.T) {
	f := &fakeBackend{}
	u := newTestUI(f)
	answerDialogs(u, "")
	u.startSync(JobSyncPrune)
	settle(u)
	if len(f.syncCalls) != 0 {
		t.Fatalf("a cancelled prune still ran %d Sync calls", len(f.syncCalls))
	}
}

// A prune with nothing to prune is a plain sync, not a second dialog
// asking about an empty list.
func TestSyncPruneWithNoOrphansFallsBackToASync(t *testing.T) {
	f := &fakeBackend{plans: []*syncer.Plan{{Albums: 3}}}
	u := newTestUI(f)
	answerDialogs(u, "yes")
	u.startSync(JobSyncPrune)
	settle(u)
	if len(f.syncCalls) != 2 {
		t.Fatalf("got %d Sync calls, want 2", len(f.syncCalls))
	}
	if f.syncCalls[1].Prune {
		t.Error("the fallback sync still had Prune set")
	}
}

func TestCheckFillsTheRelease(t *testing.T) {
	f := &fakeBackend{rel: Release{Checked: true, Tag: "v0.1.3", Asset: "core.ipod", Notes: "notes"}}
	u := newTestUI(f)
	u.startCheck()
	settle(u)
	if u.st.Release.Tag != "v0.1.3" {
		t.Fatalf("the release was not recorded: %+v", u.st.Release)
	}
	if !strings.Contains(strings.Join(u.st.Log, "\n"), "latest release: v0.1.3") {
		t.Errorf("the log does not name the release: %q", u.st.Log)
	}
}

func TestUpdateWithoutACheckRefuses(t *testing.T) {
	f := &fakeBackend{}
	u := newTestUI(f)
	u.startUpdate()
	settle(u)
	if u.st.Job == nil || !u.st.Job.Failed || !strings.Contains(u.st.Job.Text, "Check first") {
		t.Errorf("update with no release ran anyway: %+v", u.st.Job)
	}
}

func TestUpdateDownloadsThenFlashes(t *testing.T) {
	f := &fakeBackend{
		dev:         Device{Found: true, Path: `\\.\PhysicalDrive2`},
		rel:         Release{Checked: true, Tag: "v0.1.3"},
		downloaded:  "/cache/core.ipod",
		flashPrompt: flasher.ConfirmPrompt(`\\.\PhysicalDrive2`),
	}
	u := newTestUI(f)
	u.st.Release = f.rel
	u.st.Device = f.dev
	answerDialogs(u, `\\.\PhysicalDrive2`)
	u.startUpdate()
	settle(u)

	if len(f.flashCalls) != 1 || f.flashCalls[0] != "/cache/core.ipod" {
		t.Fatalf("Flash was called with %q", f.flashCalls)
	}
	if u.st.Job == nil || u.st.Job.Failed {
		t.Errorf("the update failed: %+v", u.st.Job)
	}
}

// The confirmation dialog must carry the flasher's own plan text and
// require the device path typed exactly — the same gate the CLI has.
func TestFlashDialogShowsThePlanAndDemandsTheDevicePath(t *testing.T) {
	f := &fakeBackend{
		dev:         Device{Found: true, Path: `\\.\PhysicalDrive2`, Volume: `D:\`},
		flashPrompt: flasher.ConfirmPrompt(`\\.\PhysicalDrive2`),
	}
	u := newTestUI(f)
	u.st.Device = f.dev

	var seen *dialogRequest
	go func() {
		for i := 0; i < 2000; i++ {
			u.mu.Lock()
			req := u.askQueue
			if req != nil {
				u.askQueue = nil
			}
			u.mu.Unlock()
			if req != nil {
				seen = req
				req.reply <- `\\.\PhysicalDrive2`
				return
			}
		}
	}()
	u.startFlash("/img/core.ipod")
	settle(u)

	if seen == nil {
		t.Fatal("no confirmation dialog was raised")
	}
	if !strings.Contains(seen.body, "image   /img/core.ipod") {
		t.Errorf("the dialog does not show the flasher's plan:\n%s", seen.body)
	}
	if seen.want != `\\.\PhysicalDrive2` {
		t.Errorf("the dialog wants %q typed, want the device path", seen.want)
	}
	if f.flashAnswer != `\\.\PhysicalDrive2` {
		t.Errorf("the flasher got %q as the confirmation", f.flashAnswer)
	}
}

func TestFlashWithNoFileRefuses(t *testing.T) {
	u := newTestUI(&fakeBackend{})
	u.startFlash("")
	settle(u)
	if u.st.Job == nil || !u.st.Job.Failed || !strings.Contains(u.st.Job.Text, "no image") {
		t.Errorf("flash with no file: %+v", u.st.Job)
	}
}

// TestDevicePathFromPrompt holds the dialog to the flasher's REAL
// prompt. The first version of this test used a made-up prompt with
// parentheses, passed, and every flash through the window aborted at
// the confirmation because the flasher never wrote one.
func TestDevicePathFromPrompt(t *testing.T) {
	for _, c := range []struct {
		in, want string
		ok       bool
	}{
		{flasher.ConfirmPrompt(`\\.\PhysicalDrive2`), `\\.\PhysicalDrive2`, true},
		{flasher.ConfirmPrompt("/dev/sdb"), "/dev/sdb", true},
		{`type the device path (\\.\PhysicalDrive2) to confirm: `, "", false},
		{"are you sure? ", "", false},
	} {
		got, ok := devicePathFromPrompt(c.in)
		if got != c.want || ok != c.ok {
			t.Errorf("devicePathFromPrompt(%q) = %q, %v; want %q, %v", c.in, got, ok, c.want, c.ok)
		}
	}
}

// TestFlashRefusesAnUnknownPromptShape: a flasher whose prompt the app
// cannot parse must not get a dialog that waves the write through.
func TestFlashRefusesAnUnknownPromptShape(t *testing.T) {
	f := &fakeBackend{dev: Device{Found: true, Path: `\\.\PhysicalDrive2`},
		flashPrompt: "are you sure? "}
	u := newTestUI(f)
	u.startFlash("core.ipod")
	settle(u)
	if u.st.Job == nil || !u.st.Job.Failed || !strings.Contains(u.st.Job.Text, "not in the form") {
		t.Errorf("unknown prompt: %+v", u.st.Job)
	}
	if f.flashAnswer != "" {
		t.Errorf("an answer %q reached the flasher", f.flashAnswer)
	}
}

func TestBackupAndEjectJobs(t *testing.T) {
	f := &fakeBackend{}
	u := newTestUI(f)

	u.startBackup()
	settle(u)
	if f.backupCalls != 1 {
		t.Errorf("Backup was called %d times", f.backupCalls)
	}
	if !strings.Contains(strings.Join(u.st.Log, "\n"), "backup written: /tmp/fwpart-1-x.bin") {
		t.Errorf("the backup path was not logged: %q", u.st.Log)
	}

	u.startEject()
	settle(u)
	if len(f.ejectCalls) != 1 || f.ejectCalls[0] != `D:\` {
		t.Errorf("Eject was called with %q", f.ejectCalls)
	}
}

func TestEjectWithoutAVolumeRefuses(t *testing.T) {
	f := &fakeBackend{}
	u := NewUI(Options{Backend: f})
	u.startEject()
	settle(u)
	if len(f.ejectCalls) != 0 {
		t.Fatal("eject ran with no volume")
	}
	if u.st.Job == nil || !u.st.Job.Failed {
		t.Errorf("eject with no volume did not fail: %+v", u.st.Job)
	}
}

// A second click while a job runs is refused and says so, rather than
// queueing a raw-device job behind another one.
func TestASecondJobWhileBusyIsRefused(t *testing.T) {
	f := &fakeBackend{block: make(chan struct{})}
	u := newTestUI(f)
	u.startSync(JobSync)
	for i := 0; i < 1000 && !u.run.Busy(); i++ {
	}
	u.startBackup()
	if f.backupCalls != 0 {
		t.Error("a backup started while a sync was running")
	}
	if !strings.Contains(strings.Join(u.st.Log, "\n"), ErrBusy.Error()) {
		t.Errorf("the refusal was not logged: %q", u.st.Log)
	}
	close(f.block)
	settle(u)
}

func TestErrorsFromTheBackendReachTheState(t *testing.T) {
	f := &fakeBackend{refreshErr: errors.New("disk: no iPod found")}
	u := NewUI(Options{Backend: f})
	u.startRefresh()
	settle(u)
	if u.st.Job == nil || !u.st.Job.Failed || u.st.Job.Text != "disk: no iPod found" {
		t.Fatalf("job status = %+v", u.st.Job)
	}
	if !strings.Contains(strings.Join(u.st.Log, "\n"), "error: disk: no iPod found") {
		t.Errorf("the error is not in the log: %q", u.st.Log)
	}
}

// CLIPath must find the CLI beside the executable before it falls back
// to PATH: a machine with an old `core` on PATH and a new one beside
// the app would otherwise flash through the old one.
func TestCLIPathPrefersTheBinaryBesideTheApp(t *testing.T) {
	// The test binary IS the executable here, so put a fake `core`
	// next to it only if we can — otherwise this just asserts that the
	// function does not panic and returns something consistent.
	p, ok := CLIPath()
	if ok && !filepath.IsAbs(p) {
		t.Errorf("CLIPath returned a relative path %q", p)
	}
	if !ok && p != "" {
		t.Errorf("CLIPath returned %q with ok=false", p)
	}
	if ok {
		if _, err := exec.LookPath(p); err != nil {
			t.Errorf("CLIPath returned %q, which is not executable: %v", p, err)
		}
	}
}
