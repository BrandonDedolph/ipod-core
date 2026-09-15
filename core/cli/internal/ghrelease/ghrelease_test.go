package ghrelease

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/firmware"
)

// No test in this file touches the network. Every one of them points a
// Client at an httptest server; the package-level BaseURL is never used
// in a test, so a mistake here cannot turn into a CI job that fails
// when GitHub is slow.

// ipodBytes wraps an image in the .ipod transport format the same way
// `core firmware pack` does, so the checksum a download is verified
// against is a real one.
func ipodBytes(t *testing.T, image []byte) []byte {
	t.Helper()
	var b bytes.Buffer
	if err := firmware.WriteIPodFile(&b, firmware.ModelIPodVideo,
		firmware.ModelNameIPodVideo, image); err != nil {
		t.Fatalf("WriteIPodFile: %v", err)
	}
	return b.Bytes()
}

func plausibleImage(n int) []byte {
	b := make([]byte, n)
	for i := range b {
		b[i] = byte(i * 7)
	}
	b[0], b[1], b[2], b[3] = 0x0E, 0x00, 0x00, 0xEA
	return b
}

// apiServer serves the two release endpoints and, at /dl/<name>, the
// asset bodies. downloads counts asset fetches so the cache-reuse test
// can prove no second request happened.
type apiServer struct {
	*httptest.Server
	downloads int32
	authSeen  chan string
}

func newAPIServer(t *testing.T, releases map[string]Release, bodies map[string][]byte) *apiServer {
	t.Helper()
	s := &apiServer{authSeen: make(chan string, 16)}
	mux := http.NewServeMux()
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		select {
		case s.authSeen <- r.Header.Get("Authorization"):
		default:
		}
		if name, ok := strings.CutPrefix(r.URL.Path, "/dl/"); ok {
			body, found := bodies[name]
			if !found {
				http.NotFound(w, r)
				return
			}
			atomic.AddInt32(&s.downloads, 1)
			w.Write(body)
			return
		}
		if r.Header.Get("Accept") != "application/vnd.github+json" {
			http.Error(w, "wrong Accept: "+r.Header.Get("Accept"), http.StatusBadRequest)
			return
		}
		key := ""
		switch {
		case strings.HasSuffix(r.URL.Path, "/releases/latest"):
			key = "latest"
		default:
			if tag, ok := strings.CutPrefix(r.URL.Path, "/repos/owner/name/releases/tags/"); ok {
				key = tag
			}
		}
		rel, ok := releases[key]
		if !ok {
			http.Error(w, `{"message":"Not Found"}`, http.StatusNotFound)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(rel)
	})
	s.Server = httptest.NewServer(mux)
	t.Cleanup(s.Close)
	return s
}

func (s *apiServer) client() *Client {
	return &Client{HTTP: s.Server.Client(), BaseURL: s.Server.URL}
}

func TestLatestParsesARelease(t *testing.T) {
	rel := Release{
		Tag:   "v0.1.3",
		Name:  "v0.1.3 — the version marker",
		Notes: "- the OSOS body now says which build it is\n- `core update`\n",
		Assets: []Asset{
			{Name: "core-v0.1.3.ipod", Size: 1234, URL: "https://example.invalid/x"},
			{Name: "core-windows-amd64.exe", Size: 9, URL: "https://example.invalid/y"},
		},
	}
	s := newAPIServer(t, map[string]Release{"latest": rel}, nil)
	got, err := s.client().Latest(context.Background(), "owner/name")
	if err != nil {
		t.Fatalf("Latest: %v", err)
	}
	if got.Tag != "v0.1.3" || got.Name != rel.Name || got.Notes != rel.Notes {
		t.Errorf("Latest = %+v", got)
	}
	if len(got.Assets) != 2 || got.Assets[0].Size != 1234 ||
		got.Assets[0].URL != "https://example.invalid/x" {
		t.Errorf("assets = %+v", got.Assets)
	}
}

func TestByTagParsesARelease(t *testing.T) {
	s := newAPIServer(t, map[string]Release{
		"latest": {Tag: "v0.1.3"},
		"v0.1.1": {Tag: "v0.1.1", Notes: "older"},
	}, nil)
	got, err := s.client().ByTag(context.Background(), "owner/name", "v0.1.1")
	if err != nil {
		t.Fatalf("ByTag: %v", err)
	}
	if got.Tag != "v0.1.1" || got.Notes != "older" {
		t.Errorf("ByTag = %+v, want the v0.1.1 release", got)
	}
}

func TestByTagUnknownTagIsNotFound(t *testing.T) {
	s := newAPIServer(t, map[string]Release{"latest": {Tag: "v0.1.3"}}, nil)
	_, err := s.client().ByTag(context.Background(), "owner/name", "v9.9.9")
	if !errors.Is(err, ErrNotFound) {
		t.Errorf("ByTag on a missing tag = %v, want ErrNotFound", err)
	}
}

func TestRateLimitNamesTheTokenVariable(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		http.Error(w, `{"message":"API rate limit exceeded"}`, http.StatusForbidden)
	}))
	defer srv.Close()
	c := &Client{HTTP: srv.Client(), BaseURL: srv.URL}
	_, err := c.Latest(context.Background(), "owner/name")
	if !errors.Is(err, ErrRateLimited) {
		t.Fatalf("Latest against a 403 = %v, want ErrRateLimited", err)
	}
	if !strings.Contains(err.Error(), "GITHUB_TOKEN") {
		t.Errorf("the rate-limit error does not name the way out:\n%v", err)
	}
}

func TestTokenIsSentWhenSet(t *testing.T) {
	s := newAPIServer(t, map[string]Release{"latest": {Tag: "v1"}}, nil)
	c := s.client()
	c.Token = "ghp_secret"
	if _, err := c.Latest(context.Background(), "owner/name"); err != nil {
		t.Fatalf("Latest: %v", err)
	}
	if got := <-s.authSeen; got != "Bearer ghp_secret" {
		t.Errorf("Authorization = %q", got)
	}
}

func TestBadRepoIsRefusedBeforeAnyRequest(t *testing.T) {
	c := &Client{BaseURL: "http://127.0.0.1:0"} // any request would fail loudly
	for _, repo := range []string{"", "owner", "owner/name/extra", "owner/../x", "own er/name"} {
		if _, err := c.Latest(context.Background(), repo); err == nil {
			t.Errorf("Latest(%q) was accepted", repo)
		}
	}
}

// FirmwareAsset has to accept both names: core-<tag>.ipod is what the
// releases published so far carry, core.ipod is what the plan wrote
// down.
func TestFirmwareAssetAcceptsBothNames(t *testing.T) {
	versioned := Release{Tag: "v0.1.2", Assets: []Asset{
		{Name: "core-linux-amd64"},
		{Name: "core-v0.1.2.ipod", Size: 7},
	}}
	a, ok := FirmwareAsset(versioned)
	if !ok || a.Name != "core-v0.1.2.ipod" {
		t.Errorf("FirmwareAsset(versioned) = %+v, %v", a, ok)
	}

	plain := Release{Tag: "v0.2.0", Assets: []Asset{{Name: "core.ipod", Size: 9}}}
	a, ok = FirmwareAsset(plain)
	if !ok || a.Name != "core.ipod" {
		t.Errorf("FirmwareAsset(plain) = %+v, %v", a, ok)
	}

	// With both present the versioned name wins: it is the one that
	// names the release it came from.
	both := Release{Tag: "v0.3.0", Assets: []Asset{
		{Name: "core.ipod"}, {Name: "core-v0.3.0.ipod"},
	}}
	if a, _ := FirmwareAsset(both); a.Name != "core-v0.3.0.ipod" {
		t.Errorf("FirmwareAsset(both) picked %q", a.Name)
	}

	// A release with only host binaries on it has no firmware image,
	// and saying so is not the same as saying the download failed.
	none := Release{Tag: "v0.4.0", Assets: []Asset{{Name: "core-windows-amd64.exe"}}}
	if _, ok := FirmwareAsset(none); ok {
		t.Error("FirmwareAsset found an image on a release that has none")
	}
}

func TestDownloadVerifiesAndCaches(t *testing.T) {
	body := ipodBytes(t, plausibleImage(4096))
	s := newAPIServer(t, nil, map[string][]byte{"core-v0.1.3.ipod": body})
	a := Asset{Name: "core-v0.1.3.ipod", Size: int64(len(body)), URL: s.URL + "/dl/core-v0.1.3.ipod"}
	dst := filepath.Join(t.TempDir(), "v0.1.3", a.Name)

	if Cached(a, dst) {
		t.Fatal("Cached said yes before anything was downloaded")
	}
	if err := s.client().Download(context.Background(), a, dst); err != nil {
		t.Fatalf("Download: %v", err)
	}
	got, err := os.ReadFile(dst)
	if err != nil {
		t.Fatalf("read the download: %v", err)
	}
	if !bytes.Equal(got, body) {
		t.Fatal("the downloaded bytes are not the ones served")
	}
	if n := atomic.LoadInt32(&s.downloads); n != 1 {
		t.Fatalf("%d asset requests for one download", n)
	}

	// Cache reuse: the second call must not go near the server.
	if !Cached(a, dst) {
		t.Error("Cached said no about a file it had just verified")
	}
	if err := s.client().Download(context.Background(), a, dst); err != nil {
		t.Fatalf("second Download: %v", err)
	}
	if n := atomic.LoadInt32(&s.downloads); n != 1 {
		t.Errorf("the cached file was re-fetched (%d requests)", n)
	}

	// No temp files left behind.
	entries, _ := os.ReadDir(filepath.Dir(dst))
	for _, e := range entries {
		if strings.HasPrefix(e.Name(), ".download-") {
			t.Errorf("a temp file survived: %s", e.Name())
		}
	}
}

func TestDownloadRejectsABadChecksum(t *testing.T) {
	body := ipodBytes(t, plausibleImage(4096))
	body[len(body)-1] ^= 0xFF // one flipped bit in the image, header untouched
	s := newAPIServer(t, nil, map[string][]byte{"core.ipod": body})
	a := Asset{Name: "core.ipod", Size: int64(len(body)), URL: s.URL + "/dl/core.ipod"}
	dst := filepath.Join(t.TempDir(), "v1", a.Name)

	err := s.client().Download(context.Background(), a, dst)
	if !errors.Is(err, ErrChecksum) {
		t.Fatalf("Download of a corrupt image = %v, want ErrChecksum", err)
	}
	if _, err := os.Stat(dst); !os.IsNotExist(err) {
		t.Error("a download that failed its checksum was left in the cache")
	}
}

func TestDownloadRejectsAWrongSize(t *testing.T) {
	body := ipodBytes(t, plausibleImage(4096))
	s := newAPIServer(t, nil, map[string][]byte{"core.ipod": body})
	a := Asset{Name: "core.ipod", Size: int64(len(body)) + 32, URL: s.URL + "/dl/core.ipod"}
	dst := filepath.Join(t.TempDir(), "v1", a.Name)

	err := s.client().Download(context.Background(), a, dst)
	if err == nil || !strings.Contains(err.Error(), "arrived") {
		t.Fatalf("Download with a size mismatch = %v, want a size complaint", err)
	}
	if _, err := os.Stat(dst); !os.IsNotExist(err) {
		t.Error("a short download was left in the cache")
	}
}

// A cache entry that is the right length but the wrong bytes must be
// re-fetched, not trusted: the size is metadata and the checksum is the
// thing that decides.
func TestCachedRejectsACorruptCacheEntry(t *testing.T) {
	body := ipodBytes(t, plausibleImage(4096))
	dir := t.TempDir()
	dst := filepath.Join(dir, "core.ipod")
	bad := append([]byte(nil), body...)
	bad[len(bad)-1] ^= 0xFF
	if err := os.WriteFile(dst, bad, 0o644); err != nil {
		t.Fatal(err)
	}
	a := Asset{Name: "core.ipod", Size: int64(len(body))}
	if Cached(a, dst) {
		t.Error("Cached trusted a file with the right size and the wrong bytes")
	}
}

func TestCachePathIsUnderTheUserCacheDir(t *testing.T) {
	p, err := CachePath("v0.1.3", "core-v0.1.3.ipod")
	if err != nil {
		t.Skipf("no user cache directory on this machine: %v", err)
	}
	if filepath.Base(p) != "core-v0.1.3.ipod" ||
		filepath.Base(filepath.Dir(p)) != "v0.1.3" ||
		filepath.Base(filepath.Dir(filepath.Dir(p))) != "core" {
		t.Errorf("CachePath = %q, want <cache>/core/v0.1.3/core-v0.1.3.ipod", p)
	}
	// A tag off the network must not be able to address a directory of
	// its own choosing.
	esc, err := CachePath("../../etc", "../passwd")
	if err != nil {
		t.Fatal(err)
	}
	if strings.Contains(esc, "..") {
		t.Errorf("CachePath let a hostile tag escape: %q", esc)
	}
}
