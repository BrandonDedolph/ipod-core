package cli

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"sync/atomic"
	"testing"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/firmware"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flasher"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/ghrelease"
	"github.com/spf13/cobra"
)

// Every test here drives the real command through the real cobra tree.
// The three things that would otherwise reach outside the process — the
// GitHub API, the user's cache directory and the iPod — are replaced:
// the API by an httptest server, the cache by a temp HOME, the device
// by a stub version reader. No test makes a network call and none of
// them opens a disk.

// --- fakes ------------------------------------------------------------

type fakeGitHub struct {
	*httptest.Server
	releases  map[string]ghrelease.Release
	bodies    map[string][]byte
	downloads int32
}

// newFakeGitHub starts the server AND installs it as the package's
// release client for the duration of the test.
func newFakeGitHub(t *testing.T) *fakeGitHub {
	t.Helper()
	g := &fakeGitHub{
		releases: map[string]ghrelease.Release{},
		bodies:   map[string][]byte{},
	}
	mux := http.NewServeMux()
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		if name, ok := strings.CutPrefix(r.URL.Path, "/dl/"); ok {
			body, found := g.bodies[name]
			if !found {
				http.NotFound(w, r)
				return
			}
			atomic.AddInt32(&g.downloads, 1)
			w.Write(body)
			return
		}
		key := "latest"
		if tag, ok := strings.CutPrefix(r.URL.Path, "/repos/owner/name/releases/tags/"); ok {
			key = tag
		}
		rel, ok := g.releases[key]
		if !ok {
			http.Error(w, `{"message":"Not Found"}`, http.StatusNotFound)
			return
		}
		json.NewEncoder(w).Encode(rel)
	})
	g.Server = httptest.NewServer(mux)
	t.Cleanup(g.Close)

	prev := newReleaseClient
	newReleaseClient = func() *ghrelease.Client {
		return &ghrelease.Client{HTTP: g.Server.Client(), BaseURL: g.Server.URL}
	}
	t.Cleanup(func() { newReleaseClient = prev })
	return g
}

// asset registers a downloadable file and returns the release asset
// that points at it.
func (g *fakeGitHub) asset(name string, body []byte) ghrelease.Asset {
	g.bodies[name] = body
	return ghrelease.Asset{Name: name, Size: int64(len(body)), URL: g.URL + "/dl/" + name}
}

// publish makes a release answer /releases/latest and its own tag.
func (g *fakeGitHub) publish(rel ghrelease.Release) {
	g.releases["latest"] = rel
	g.releases[rel.Tag] = rel
}

func (g *fakeGitHub) hits() int32 { return atomic.LoadInt32(&g.downloads) }

// stubDeviceVersion replaces the read of the installed version.
func stubDeviceVersion(t *testing.T, version string, err error) {
	t.Helper()
	prev := deviceVersionFunc
	deviceVersionFunc = func(*cobra.Command) (string, error) { return version, err }
	t.Cleanup(func() { deviceVersionFunc = prev })
}

// isolateCache points os.UserCacheDir at a temp directory on every OS
// this builds for, so a test download lands somewhere disposable.
func isolateCache(t *testing.T) string {
	t.Helper()
	dir := t.TempDir()
	t.Setenv("XDG_CACHE_HOME", dir) // linux
	t.Setenv("HOME", dir)           // darwin: <HOME>/Library/Caches
	t.Setenv("LocalAppData", dir)   // windows
	return dir
}

type flashCall struct {
	Called bool
	Opts   flasher.Options
	Deps   flasher.Deps
}

// captureFlash replaces the flasher and records what it was handed.
func captureFlash(t *testing.T) *flashCall {
	t.Helper()
	got := &flashCall{}
	prev := flashImage
	flashImage = func(ctx context.Context, o flasher.Options, d flasher.Deps) (*flasher.Result, error) {
		got.Called, got.Opts, got.Deps = true, o, d
		return &flasher.Result{Verified: true, Device: "/dev/fake"}, nil
	}
	t.Cleanup(func() { flashImage = prev })
	return got
}

func ipodFile(t *testing.T, image []byte) []byte {
	t.Helper()
	var b bytes.Buffer
	if err := firmware.WriteIPodFile(&b, firmware.ModelIPodVideo,
		firmware.ModelNameIPodVideo, image); err != nil {
		t.Fatalf("WriteIPodFile: %v", err)
	}
	return b.Bytes()
}

// releaseV013 is the shape of a real release: host binaries plus the
// versioned firmware image the human release flow uploads.
func releaseV013(g *fakeGitHub, body []byte) ghrelease.Release {
	return ghrelease.Release{
		Tag:   "v0.1.3",
		Name:  "the version marker",
		Notes: "- the OSOS body says which build it is\n- `core update` and `core doctor`\n",
		Assets: []ghrelease.Asset{
			{Name: "core-windows-amd64.exe", Size: 3, URL: g.URL + "/dl/core-windows-amd64.exe"},
			g.asset("core-v0.1.3.ipod", body),
		},
	}
}

var errNoDeviceForTest = errors.New("no iPod found: put it in disk mode")

// --- tests ------------------------------------------------------------

func TestUpdateCheckPrintsTheComparisonAndTheNotes(t *testing.T) {
	g := newFakeGitHub(t)
	g.publish(releaseV013(g, ipodFile(t, plausibleImage(4096))))
	stubDeviceVersion(t, "v0.1.2", nil)
	isolateCache(t)

	out, _, err := runCore(t, "update", "--check", "--repo", "owner/name")
	if err != nil {
		t.Fatalf("update --check: %v", err)
	}
	for _, want := range []string{
		"device: v0.1.2 · latest: v0.1.3",
		"release: v0.1.3 — the version marker",
		"asset:  core-v0.1.3.ipod (",
		"--- release notes ---",
		"- `core update` and `core doctor`",
		"--- end of release notes ---",
		"--check: nothing was downloaded and nothing was written.",
	} {
		if !strings.Contains(out, want) {
			t.Errorf("update --check did not print %q:\n%s", want, out)
		}
	}
	if g.hits() != 0 {
		t.Error("--check downloaded something")
	}
}

// Long release notes are elided rather than pasted in full.
func TestUpdateCheckElidesLongNotes(t *testing.T) {
	g := newFakeGitHub(t)
	var sb strings.Builder
	for i := 0; i < notesLineLimit+20; i++ {
		fmt.Fprintf(&sb, "line %d\n", i)
	}
	g.publish(ghrelease.Release{Tag: "v0.1.3", Notes: sb.String()})
	stubDeviceVersion(t, "v0.1.2", nil)
	isolateCache(t)

	out, _, err := runCore(t, "update", "--check", "--repo", "owner/name")
	if err != nil {
		t.Fatalf("update --check: %v", err)
	}
	if !strings.Contains(out, "line 39") || strings.Contains(out, "line 41") {
		t.Errorf("notes were not cut at %d lines:\n%s", notesLineLimit, out)
	}
	if !strings.Contains(out, "\n…\n") {
		t.Errorf("the elision is not marked:\n%s", out)
	}
}

// An image with no marker is every image before v0.1.3, which is what
// is on the device right now. --check must still work and must say why
// it cannot name a version.
func TestUpdateCheckWithAnUnknownDeviceVersion(t *testing.T) {
	g := newFakeGitHub(t)
	g.publish(ghrelease.Release{Tag: "v0.1.3"})
	stubDeviceVersion(t, "", nil)
	isolateCache(t)

	out, _, err := runCore(t, "update", "--check", "--repo", "owner/name")
	if err != nil {
		t.Fatalf("update --check: %v", err)
	}
	if !strings.Contains(out, "device: unknown · latest: v0.1.3") {
		t.Errorf("missing the unknown-version comparison:\n%s", out)
	}
	if !strings.Contains(out, "images before v0.1.3 carry none") {
		t.Errorf("the unknown version is not explained:\n%s", out)
	}
}

// No iPod attached is not a reason for --check to fail: the release is
// still worth reporting, and the reason the device could not be read is
// printed under it.
func TestUpdateCheckWithNoDevice(t *testing.T) {
	g := newFakeGitHub(t)
	g.publish(ghrelease.Release{Tag: "v0.1.3"})
	stubDeviceVersion(t, "", errNoDeviceForTest)
	isolateCache(t)

	out, _, err := runCore(t, "update", "--check", "--repo", "owner/name")
	if err != nil {
		t.Fatalf("update --check with no device: %v", err)
	}
	if !strings.Contains(out, "device: not read · latest: v0.1.3") {
		t.Errorf("missing the not-read line:\n%s", out)
	}
	if !strings.Contains(out, errNoDeviceForTest.Error()) {
		t.Errorf("the reason the device could not be read is not printed:\n%s", out)
	}
}

func TestUpdateAlreadyUpToDate(t *testing.T) {
	g := newFakeGitHub(t)
	g.publish(releaseV013(g, ipodFile(t, plausibleImage(4096))))
	stubDeviceVersion(t, "v0.1.3", nil)
	isolateCache(t)
	flash := captureFlash(t)

	out, _, err := runCore(t, "update", "--yes", "--repo", "owner/name")
	if err != nil {
		t.Fatalf("update when up to date: %v", err)
	}
	if !strings.Contains(out, "already up to date: the device runs v0.1.3") {
		t.Errorf("update did not say it was up to date:\n%s", out)
	}
	if !strings.Contains(out, "--tag v0.1.3") {
		t.Errorf("update did not name the way to write it anyway:\n%s", out)
	}
	if flash.Called {
		t.Error("update flashed a device that already runs the latest release")
	}
	if g.hits() != 0 {
		t.Error("update downloaded an image it was not going to write")
	}
}

// --tag is the explicit override: someone naming a version is asking
// for it to be written, which is how a bad flash of the current version
// gets fixed.
func TestUpdateTagForcesAReflashOfTheInstalledVersion(t *testing.T) {
	g := newFakeGitHub(t)
	g.publish(releaseV013(g, ipodFile(t, plausibleImage(4096))))
	stubDeviceVersion(t, "v0.1.3", nil)
	isolateCache(t)
	flash := captureFlash(t)

	out, _, err := runCore(t, "update", "--tag", "v0.1.3", "--yes", "--repo", "owner/name")
	if err != nil {
		t.Fatalf("update --tag: %v", err)
	}
	if !strings.Contains(out, "device: v0.1.3 · requested: v0.1.3") {
		t.Errorf("--tag did not relabel the comparison:\n%s", out)
	}
	if !strings.Contains(out, "--tag was given, so it will be written again") {
		t.Errorf("--tag did not override up-to-date:\n%s", out)
	}
	if !flash.Called {
		t.Fatal("--tag did not reach the flash step")
	}
	if g.hits() != 1 {
		t.Errorf("%d downloads for one forced update", g.hits())
	}
}

// The seam this test guards is the one that matters most on Windows:
// the flasher is handed the CACHED FILE, and the elevated child it
// would start is a `flash` of that file — never another `update`, which
// would re-resolve the release and re-download it as Administrator.
func TestUpdateFlashesTheCachedFileAndTheChildRunsFlash(t *testing.T) {
	body := ipodFile(t, plausibleImage(8192))
	g := newFakeGitHub(t)
	g.publish(releaseV013(g, body))
	stubDeviceVersion(t, "v0.1.2", nil)
	cache := isolateCache(t)
	flash := captureFlash(t)

	out, _, err := runCore(t, "update", "--yes", "--repo", "owner/name",
		"--device", "/dev/sdz", "--untested-hardware", "--backup-dir", cache)
	if err != nil {
		t.Fatalf("update: %v", err)
	}
	if !flash.Called {
		t.Fatal("update never reached the flash step")
	}

	want, err := ghrelease.CachePath("v0.1.3", "core-v0.1.3.ipod")
	if err != nil {
		t.Fatal(err)
	}
	if flash.Opts.Image != want {
		t.Errorf("flasher got image %q, want the cached path %q", flash.Opts.Image, want)
	}
	if got, rerr := os.ReadFile(want); rerr != nil || !bytes.Equal(got, body) {
		t.Errorf("the cached file is not the downloaded image (%v)", rerr)
	}
	if !strings.HasPrefix(want, cache) {
		t.Errorf("the cache path %q is not inside the isolated cache %q", want, cache)
	}
	if !flash.Opts.Yes || !flash.Opts.Untested || flash.Opts.Device != "/dev/sdz" ||
		flash.Opts.BackupDir != cache || flash.Opts.DryRun {
		t.Errorf("flasher options did not carry the flags: %+v", flash.Opts)
	}

	args := flash.Deps.ChildArgs
	wantArgs := "flash " + want + " --device /dev/sdz --backup-dir " + cache + " --untested-hardware"
	if strings.Join(args, " ") != wantArgs {
		t.Errorf("the elevated child would run\n  %v\nwant\n  %s", args, wantArgs)
	}
	for _, a := range args {
		if a == "update" || a == "--repo" || a == "--tag" {
			t.Errorf("the elevated child would do network work: %v", args)
		}
	}
	if !strings.Contains(out, "verified:") {
		t.Errorf("the download was not reported as verified:\n%s", out)
	}
	if g.hits() != 1 {
		t.Errorf("%d downloads for one update", g.hits())
	}

	// A second run reuses the verified cache entry and fetches nothing.
	out2, _, err := runCore(t, "update", "--yes", "--repo", "owner/name")
	if err != nil {
		t.Fatalf("second update: %v", err)
	}
	if g.hits() != 1 {
		t.Errorf("the cached image was downloaded again (%d requests)", g.hits())
	}
	if !strings.Contains(out2, "already downloaded and verified") {
		t.Errorf("the second run did not report a cache hit:\n%s", out2)
	}
}

func TestUpdateUnknownTagFails(t *testing.T) {
	g := newFakeGitHub(t)
	g.publish(ghrelease.Release{Tag: "v0.1.3"})
	stubDeviceVersion(t, "v0.1.2", nil)
	isolateCache(t)

	_, _, err := runCore(t, "update", "--check", "--tag", "v9.9.9", "--repo", "owner/name")
	if err == nil || !strings.Contains(err.Error(), "v9.9.9") {
		t.Errorf("update --tag v9.9.9 = %v, want a failure naming the tag", err)
	}
}

func TestUpdateRefusesAReleaseWithNoFirmwareAsset(t *testing.T) {
	g := newFakeGitHub(t)
	g.publish(ghrelease.Release{
		Tag:    "v0.1.3",
		Assets: []ghrelease.Asset{{Name: "core-linux-amd64"}},
	})
	stubDeviceVersion(t, "v0.1.2", nil)
	isolateCache(t)
	flash := captureFlash(t)

	_, _, err := runCore(t, "update", "--yes", "--repo", "owner/name")
	if err == nil || !strings.Contains(err.Error(), "core.ipod") {
		t.Errorf("update against an assetless release = %v", err)
	}
	if flash.Called {
		t.Error("update reached the flash step with no image")
	}
}

func TestUpdateHasItsFlags(t *testing.T) {
	cmd := findCmd(t, "update")
	for _, flag := range []string{"check", "tag", "yes", "repo", "untested-hardware",
		"no-relaunch", "backup-dir"} {
		if cmd.Flags().Lookup(flag) == nil {
			t.Errorf("core update has no --%s", flag)
		}
	}
	if cmd.InheritedFlags().Lookup("device") == nil {
		t.Error("core update cannot see the global --device flag")
	}
	if got := cmd.Flags().Lookup("repo").DefValue; got != ghrelease.DefaultRepo {
		t.Errorf("--repo default = %q, want %q", got, ghrelease.DefaultRepo)
	}
}
