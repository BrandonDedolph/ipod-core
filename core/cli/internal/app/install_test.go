package app

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/devicefs"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/fwpart"
)

// The Install button, all the way down: the Runner starts the job, the
// job runs the REAL internal/installer (see fakeBackend.Install), the
// installer asks the flasher, the flasher's prompt becomes the modal,
// and only the device path typed exactly lets the write happen.

// catchDialog answers the next modal with answer and hands the request
// back, so a test can assert what the person was shown. It waits on a
// deadline rather than spinning a fixed number of times: a job that
// does a little more work before asking must not be able to wedge the
// package for the full test timeout.
func catchDialog(u *UI, answer string) chan *dialogRequest {
	seen := make(chan *dialogRequest, 1)
	go func() {
		deadline := time.Now().Add(2 * time.Second)
		for time.Now().Before(deadline) {
			u.mu.Lock()
			req := u.askQueue
			if req != nil {
				u.askQueue = nil
			}
			u.mu.Unlock()
			if req != nil {
				req.reply <- answer
				seen <- req
				return
			}
			time.Sleep(time.Millisecond)
		}
		seen <- nil
	}()
	return seen
}

func installUI(t *testing.T) (*UI, *fakeBackend, string) {
	t.Helper()
	volume := t.TempDir()
	f := &fakeBackend{
		dev: Device{
			Found: true, Path: testPath, Serial: "TESTSERIAL", Volume: volume,
			Installed: appleFirmware(),
		},
		installed: map[string]fwpart.Installed{testPath: appleFirmware()},
		volumeDir: volume,
	}
	u := newTestUI(f)
	u.st.Device = f.dev
	return u, f, volume
}

func TestInstallButtonWritesAndReachesReady(t *testing.T) {
	u, f, volume := installUI(t)
	if u.st.Phase() != PhaseNotInstalled {
		t.Fatalf("the fixture is not on the Install screen: %v", u.st.Phase())
	}

	seen := catchDialog(u, testPath)
	u.startInstall(`C:\img\core.ipod`)
	settle(u)

	req := <-seen
	if req == nil {
		t.Fatal("Install never asked for a confirmation")
	}
	if req.want != testPath {
		t.Errorf("the dialog wants %q typed, want the device path", req.want)
	}
	if !strings.Contains(req.body, "installed: Apple firmware") {
		t.Errorf("the dialog does not show what is being replaced:\n%s", req.body)
	}
	if !strings.Contains(req.title, "Install") {
		t.Errorf("the dialog is titled %q", req.title)
	}

	if len(f.installCalls) != 1 {
		t.Fatalf("Install was called %d times", len(f.installCalls))
	}
	if f.installCalls[0].Image != `C:\img\core.ipod` {
		t.Errorf("Install got image %q", f.installCalls[0].Image)
	}
	if !f.installFlash.Install {
		t.Error("the flasher was not told this write is an install")
	}
	if f.installFlash.Image != `C:\img\core.ipod` {
		t.Errorf("the flasher got image %q", f.installFlash.Image)
	}
	if f.installAns != testPath {
		t.Errorf("the flasher was handed %q as the confirmation", f.installAns)
	}

	if u.st.Job == nil || u.st.Job.Failed {
		t.Fatalf("the install failed: %+v", u.st.Job)
	}
	if u.st.Phase() != PhaseReady {
		t.Fatalf("after a verified install the window is on %v", u.st.Phase())
	}
	if u.st.Device.Installed.Kind != fwpart.Core {
		t.Errorf("the device was not re-read: %+v", u.st.Device.Installed)
	}

	// The three things the firmware cannot make for itself.
	for _, name := range []string{devicefs.ConfigName, devicefs.LogName} {
		if _, err := os.Stat(filepath.Join(volume, name)); err != nil {
			t.Errorf("%s was not created: %v", name, err)
		}
	}
	if st, err := os.Stat(filepath.Join(volume, devicefs.MusicDir)); err != nil || !st.IsDir() {
		t.Errorf("%s\\ was not created: %v", devicefs.MusicDir, err)
	}

	log := strings.Join(u.st.Log, "\n")
	if !strings.Contains(log, "apple-TESTSERIAL") {
		t.Errorf("the log does not name the Apple backup:\n%s", log)
	}
}

// Cancelling the modal must leave the device exactly as it was: no
// write, no device files, and the window still on the Install screen.
func TestInstallCancelledAtTheDialogChangesNothing(t *testing.T) {
	u, f, volume := installUI(t)

	catchDialog(u, "")
	u.startInstall(`C:\img\core.ipod`)
	settle(u)

	if f.installAns != "" {
		t.Errorf("the flasher was handed %q after a cancel", f.installAns)
	}
	if u.st.Job == nil || !u.st.Job.Failed {
		t.Errorf("a cancelled install reported success: %+v", u.st.Job)
	}
	if u.st.Phase() != PhaseNotInstalled {
		t.Errorf("a cancelled install moved the window to %v", u.st.Phase())
	}
	ents, err := os.ReadDir(volume)
	if err != nil {
		t.Fatal(err)
	}
	if len(ents) != 0 {
		t.Errorf("a cancelled install created %d files on the volume", len(ents))
	}
}

// With no file the latest release is fetched first, in this process,
// and the path it produced is what gets written.
func TestInstallWithNoFileDownloadsTheRelease(t *testing.T) {
	u, f, _ := installUI(t)
	f.rel = Release{Checked: true, Tag: "v0.1.3", Asset: "core.ipod"}
	f.downloaded = `C:\cache\core-v0.1.3.ipod`
	u.st.Release = f.rel

	catchDialog(u, testPath)
	u.startInstall("")
	settle(u)

	if len(f.installCalls) != 1 || f.installCalls[0].Image != f.downloaded {
		t.Fatalf("Install got %+v, want the downloaded release", f.installCalls)
	}
	if u.st.Job == nil || u.st.Job.Failed {
		t.Errorf("the install failed: %+v", u.st.Job)
	}
}

// An install is a write, so Cancel must not offer to stop it: the
// flasher does not check the context between the body write and the
// directory write.
func TestInstallCannotBeCancelled(t *testing.T) {
	if !JobInstall.Writes() {
		t.Fatal("JobInstall does not count as a write")
	}
	st := State{Job: &JobStatus{Kind: JobInstall}}
	if st.CanCancel() {
		t.Error("the Cancel button is offered during an install")
	}
}

// The Install screen's promise names a file that does not exist yet.
// If the name it prints is not the name the flasher will use, the
// sentence is a lie the first time somebody goes looking for it.
func TestTheInstallScreenNamesTheRealBackupFile(t *testing.T) {
	ui := NewUI(Options{})
	ui.SetState(NotInstalledState())
	got := ui.appleBackupPromise()
	want := "apple-000A2700168F1E3C-" + time.Now().Format("2006-01-02") + ".bin"
	if !strings.Contains(got, want) {
		t.Errorf("the promise does not name %s:\n%s", want, got)
	}
	if !strings.Contains(got, "only copy") {
		t.Errorf("the promise does not say why the file matters:\n%s", got)
	}
}
