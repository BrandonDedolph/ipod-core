package artfetch

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"image"
	"image/color"
	"image/jpeg"
	"image/png"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

// No test in this package touches the network. Every provider is
// pointed at an httptest server by an explicit BaseURL, the package
// variables (ITunesBaseURL, MusicBrainzBaseURL, CoverArtBaseURL) are
// never read in a test, and the two API fixtures under testdata/ are
// trimmed copies of real replies recorded once by hand.

func readFixture(t *testing.T, name string) []byte {
	t.Helper()
	b, err := os.ReadFile(filepath.Join("testdata", name))
	if err != nil {
		t.Fatal(err)
	}
	return b
}

// coverJPEG and coverPNG are genuine encoded images of the given square
// size, so the validator runs the real decoders.
func coverJPEG(t *testing.T, size int) []byte {
	t.Helper()
	img := image.NewRGBA(image.Rect(0, 0, size, size))
	for y := 0; y < size; y++ {
		for x := 0; x < size; x++ {
			img.Set(x, y, color.RGBA{uint8(x), uint8(y), 0x40, 255})
		}
	}
	var b bytes.Buffer
	if err := jpeg.Encode(&b, img, &jpeg.Options{Quality: 80}); err != nil {
		t.Fatal(err)
	}
	return b.Bytes()
}

func coverPNG(t *testing.T, size int) []byte {
	t.Helper()
	img := image.NewRGBA(image.Rect(0, 0, size, size))
	var b bytes.Buffer
	if err := png.Encode(&b, img); err != nil {
		t.Fatal(err)
	}
	return b.Bytes()
}

// fakeClock is the injectable clock the rate-limiter test runs on: it
// never sleeps, it records what it was asked to sleep for and advances
// itself by exactly that much.
type fakeClock struct {
	now    time.Time
	slept  []time.Duration
	ctxErr error
}

func (c *fakeClock) Now() time.Time { return c.now }

func (c *fakeClock) Sleep(ctx context.Context, d time.Duration) error {
	if c.ctxErr != nil {
		return c.ctxErr
	}
	c.slept = append(c.slept, d)
	c.now = c.now.Add(d)
	return nil
}

func TestITunesSearchParsesTheRealShape(t *testing.T) {
	var gotQuery, gotUA atomic.Value
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		gotQuery.Store(r.URL.RawQuery)
		gotUA.Store(r.Header.Get("User-Agent"))
		w.Header().Set("Content-Type", "text/javascript")
		w.Write(readFixture(t, "itunes_search.json"))
	}))
	defer srv.Close()

	p := &ITunes{BaseURL: srv.URL, UserAgent: "core-app/test (example)"}
	cands, err := p.Search(context.Background(), Query{Artist: "Morgan Wallen", Album: "I'm The Problem"})
	if err != nil {
		t.Fatalf("Search: %v", err)
	}
	q, _ := gotQuery.Load().(string)
	for _, want := range []string{"entity=album", "media=music", "limit=5", "term=Morgan+Wallen+I%27m+The+Problem"} {
		if !strings.Contains(q, want) {
			t.Errorf("query %q is missing %q", q, want)
		}
	}
	if ua, _ := gotUA.Load().(string); ua != "core-app/test (example)" {
		t.Errorf("User-Agent = %q", ua)
	}
	if len(cands) != 3 {
		t.Fatalf("got %d candidates, want 3", len(cands))
	}
	c := cands[1] // the album itself; [0] is the single of the same name
	if c.Artist != "Morgan Wallen" || c.Album != "I’m The Problem" {
		t.Errorf("parsed %q — %q", c.Artist, c.Album)
	}
	if c.ID != "1802103906" {
		t.Errorf("ID = %q", c.ID)
	}
	if !strings.HasSuffix(c.FullURL, "/1200x1200bb.jpg") {
		t.Errorf("FullURL = %q, want the 1200 rewrite", c.FullURL)
	}
	if !strings.HasSuffix(c.ThumbURL, "/200x200bb.jpg") {
		t.Errorf("ThumbURL = %q, want the 200 rewrite", c.ThumbURL)
	}
	if len(c.Fallbacks) != 2 ||
		!strings.HasSuffix(c.Fallbacks[0], "/600x600bb.jpg") ||
		!strings.HasSuffix(c.Fallbacks[1], "/100x100bb.jpg") {
		t.Errorf("Fallbacks = %v, want the 600 then the untouched 100", c.Fallbacks)
	}
	// Everything before the size segment must be untouched: that path
	// is the only thing tying the URL to this album.
	const stem = "https://is1-ssl.mzstatic.com/image/thumb/Music221/v4/fa/60/fe/" +
		"fa60fe4a-4329-a0bb-eb7c-3b6094a4d831/25UMGIM46049.rgb.jpg/"
	if c.FullURL != stem+"1200x1200bb.jpg" {
		t.Errorf("FullURL = %q", c.FullURL)
	}
}

func TestArtworkRewriteLeavesUnknownShapesAlone(t *testing.T) {
	cases := []struct{ in, want string }{
		{"https://x/a/100x100bb.jpg", "https://x/a/1200x1200bb.jpg"},
		{"https://x/a/100x100bf.png", "https://x/a/1200x1200bf.png"},
		{"https://x/a/60x60.jpg", "https://x/a/1200x1200.jpg"},
		{"https://x/a/cover.jpg", "https://x/a/cover.jpg"},
		{"", ""},
	}
	for _, c := range cases {
		if got := rewriteArtwork(c.in, 1200); got != c.want {
			t.Errorf("rewriteArtwork(%q) = %q, want %q", c.in, got, c.want)
		}
	}
}

func TestMusicBrainzSearchParsesTheRealShape(t *testing.T) {
	var gotQuery, gotUA atomic.Value
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		gotQuery.Store(r.URL.Query().Get("query"))
		gotUA.Store(r.Header.Get("User-Agent"))
		if r.URL.Path != "/ws/2/release/" {
			t.Errorf("path = %q", r.URL.Path)
		}
		if r.URL.Query().Get("fmt") != "json" {
			t.Errorf("fmt = %q", r.URL.Query().Get("fmt"))
		}
		w.Write(readFixture(t, "musicbrainz_release.json"))
	}))
	defer srv.Close()

	clk := &fakeClock{now: time.Unix(1700000000, 0)}
	p := &MusicBrainz{
		BaseURL: srv.URL, CAABaseURL: "https://coverartarchive.example",
		UserAgent: UserAgent(), Now: clk.Now, Sleep: clk.Sleep,
	}
	cands, err := p.Search(context.Background(), Query{Artist: `Morgan "Wallen`, Album: "One Thing At A Time"})
	if err != nil {
		t.Fatalf("Search: %v", err)
	}
	if q, _ := gotQuery.Load().(string); q != `artist:"Morgan \"Wallen" AND release:"One Thing At A Time"` {
		t.Errorf("query = %q — the quote in the artist must be escaped, not closing the term", q)
	}
	// The User-Agent is not decoration here: MusicBrainz blocks clients
	// that do not identify themselves.
	ua, _ := gotUA.Load().(string)
	if !strings.HasPrefix(ua, "core-app/") || !strings.Contains(ua, "github.com/BrandonDedolph/ipod-core") {
		t.Errorf("User-Agent = %q", ua)
	}
	if len(cands) != 3 {
		t.Fatalf("got %d candidates, want 3", len(cands))
	}
	c := cands[0]
	if c.Artist != "Morgan Wallen" || c.Album != "One Thing at a Time" || c.Rank != 1 {
		t.Errorf("parsed %q — %q rank %v", c.Artist, c.Album, c.Rank)
	}
	if want := "https://coverartarchive.example/release/b36a69a1-7af5-4ee6-a1b1-5110ee465ad8/front-500"; c.FullURL != want {
		t.Errorf("FullURL = %q, want %q", c.FullURL, want)
	}
	if !strings.HasSuffix(c.ThumbURL, "/front-250") {
		t.Errorf("ThumbURL = %q", c.ThumbURL)
	}
	if len(c.Fallbacks) != 1 || !strings.HasSuffix(c.Fallbacks[0], "/front") {
		t.Errorf("Fallbacks = %v, want the original upload", c.Fallbacks)
	}
}

// TestMusicBrainzRateLimit proves the one-per-second spacing without
// spending three seconds doing it: the clock is injected, so what the
// limiter asked to wait for is inspectable.
func TestMusicBrainzRateLimit(t *testing.T) {
	clk := &fakeClock{now: time.Unix(1700000000, 0)}
	p := &MusicBrainz{Now: clk.Now, Sleep: clk.Sleep}

	ctx := context.Background()
	for i := 0; i < 3; i++ {
		if err := p.Wait(ctx); err != nil {
			t.Fatalf("Wait %d: %v", i, err)
		}
	}
	if len(clk.slept) != 2 {
		t.Fatalf("slept %v, want two waits (the first request goes straight through)", clk.slept)
	}
	for i, d := range clk.slept {
		if d != time.Second {
			t.Errorf("wait %d = %v, want 1s", i, d)
		}
	}
	// A caller that has been idle longer than the interval does not
	// wait at all.
	clk.now = clk.now.Add(5 * time.Second)
	if err := p.Wait(ctx); err != nil {
		t.Fatal(err)
	}
	if len(clk.slept) != 2 {
		t.Errorf("an idle caller was made to wait: %v", clk.slept)
	}
	// And a cancelled context comes back as an error rather than a
	// silent request.
	clk.ctxErr = context.Canceled
	if err := p.Wait(ctx); !errors.Is(err, context.Canceled) {
		t.Errorf("Wait on a cancelled context = %v", err)
	}
}

// fakeProvider is a scripted Provider for the Find tests.
type fakeProvider struct {
	name  string
	cands []Candidate
	err   error
	calls int32
}

func (f *fakeProvider) Name() string { return f.name }
func (f *fakeProvider) Search(ctx context.Context, q Query) ([]Candidate, error) {
	atomic.AddInt32(&f.calls, 1)
	return append([]Candidate(nil), f.cands...), f.err
}

func TestFindScoresSortsAndDedupes(t *testing.T) {
	a := &fakeProvider{name: "itunes", cands: []Candidate{
		{Artist: "Morgan Wallen", Album: "I'm The Problem - Single", FullURL: "u1"},
		{Artist: "Somebody Else", Album: "Other Record", FullURL: "u2", Rank: 1},
	}}
	b := &fakeProvider{name: "musicbrainz", cands: []Candidate{
		{Artist: "Morgan Wallen", Album: "I’m The Problem", FullURL: "u3", Rank: 1},
		{Artist: "Morgan Wallen", Album: "I'm the problem", FullURL: "u4", Rank: 0.9},
	}}
	c := &Client{Providers: []Provider{a, b}, StopAtScore: 2}
	got, err := c.Find(context.Background(), Query{Artist: "Morgan Wallen", Album: "I'm The Problem"})
	if err != nil {
		t.Fatalf("Find: %v", err)
	}
	if len(got) != 3 {
		t.Fatalf("got %d candidates, want 3 (the two MusicBrainz pressings collapse):\n%v", len(got), got)
	}
	if got[0].Provider != "musicbrainz" || got[0].Score != 1.0 {
		t.Errorf("best = %v, want the exact MusicBrainz match", got[0])
	}
	if got[1].Score != 0.9 || got[1].Provider != "itunes" {
		t.Errorf("second = %v, want the iTunes single at 0.9", got[1])
	}
	if got[2].Score >= 0.5 {
		t.Errorf("a different record scored %.2f — a name mismatch must stay under 0.5", got[2].Score)
	}
}

func TestFindStopsEarlyOnAnExactMatch(t *testing.T) {
	a := &fakeProvider{name: "itunes", cands: []Candidate{{Artist: "A", Album: "B", FullURL: "u"}}}
	b := &fakeProvider{name: "musicbrainz"}
	c := &Client{Providers: []Provider{a, b}} // StopAtScore defaults to 1.0
	if _, err := c.Find(context.Background(), Query{Artist: "A", Album: "B"}); err != nil {
		t.Fatal(err)
	}
	if n := atomic.LoadInt32(&b.calls); n != 0 {
		t.Errorf("MusicBrainz was asked %d time(s) after an exact iTunes match", n)
	}
}

func TestFindTreatsOneProviderFailureAsAWarning(t *testing.T) {
	bad := &fakeProvider{name: "itunes", err: errors.New("500 Internal Server Error")}
	good := &fakeProvider{name: "musicbrainz", cands: []Candidate{{Artist: "A", Album: "B", FullURL: "u"}}}
	var warned []string
	c := &Client{Providers: []Provider{bad, good}, OnWarn: func(p string, err error) {
		warned = append(warned, p+": "+err.Error())
	}}
	got, err := c.Find(context.Background(), Query{Artist: "A", Album: "B"})
	if err != nil {
		t.Fatalf("one dead provider failed the whole lookup: %v", err)
	}
	if len(got) != 1 || len(warned) != 1 {
		t.Errorf("got %d candidates and %d warnings", len(got), len(warned))
	}

	// Both dead is an error, not an empty answer that would read as
	// "this album has no art".
	alsoBad := &fakeProvider{name: "musicbrainz", err: errors.New("502 Bad Gateway")}
	c2 := &Client{Providers: []Provider{bad, alsoBad}}
	if _, err := c2.Find(context.Background(), Query{Artist: "A", Album: "B"}); err == nil {
		t.Error("every provider failed and Find returned nil")
	}
}

func TestFindRecordsAndHonoursTheNegativeCache(t *testing.T) {
	clk := &fakeClock{now: time.Unix(1700000000, 0)}
	cache := &Cache{Dir: t.TempDir(), NegativeTTL: time.Hour, Now: clk.Now}
	p := &fakeProvider{name: "itunes"} // answers, with nothing
	c := &Client{Providers: []Provider{p}, Cache: cache}
	q := Query{Artist: "Nobody", Album: "Unreleased Tape"}

	if _, err := c.Find(context.Background(), q); !errors.Is(err, ErrNoMatch) {
		t.Fatalf("first Find = %v, want ErrNoMatch", err)
	}
	if _, err := c.Find(context.Background(), q); !errors.Is(err, ErrNoMatch) {
		t.Fatalf("second Find = %v, want ErrNoMatch", err)
	}
	if n := atomic.LoadInt32(&p.calls); n != 1 {
		t.Errorf("the provider was asked %d times; the remembered miss should have stopped the second", n)
	}
	// Past the TTL the question is worth asking again.
	clk.now = clk.now.Add(2 * time.Hour)
	if _, err := c.Find(context.Background(), q); !errors.Is(err, ErrNoMatch) {
		t.Fatal(err)
	}
	if n := atomic.LoadInt32(&p.calls); n != 2 {
		t.Errorf("the provider was asked %d times; an expired miss must not stop a lookup", n)
	}
	// And Forget drops it immediately.
	if err := cache.Forget(q); err != nil {
		t.Fatal(err)
	}
	if cache.Missed(q) {
		t.Error("Forget left the miss in place")
	}
}

// imageServer serves a scripted set of paths and counts the hits.
type imageServer struct {
	*httptest.Server
	hits int32
}

func newImageServer(t *testing.T, bodies map[string][]byte) *imageServer {
	t.Helper()
	s := &imageServer{}
	s.Server = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		atomic.AddInt32(&s.hits, 1)
		b, ok := bodies[r.URL.Path]
		if !ok {
			http.NotFound(w, r)
			return
		}
		w.Write(b)
	}))
	t.Cleanup(s.Close)
	return s
}

func TestFetchValidatesCachesAndFallsBack(t *testing.T) {
	big := coverJPEG(t, 600)
	srv := newImageServer(t, map[string][]byte{
		"/600.jpg":   big,
		"/tiny.jpg":  coverJPEG(t, 64),
		"/junk.jpg":  []byte("this is not an image at all"),
		"/cover.png": coverPNG(t, 400),
	})
	cache := &Cache{Dir: t.TempDir()}
	c := &Client{Cache: cache}

	// The 1200 is a 404 here; the fallback is taken and the bytes are
	// the ones that arrived.
	data, mime, err := c.Fetch(context.Background(), Candidate{
		Provider: "itunes",
		FullURL:  srv.URL + "/1200.jpg",
		Fallbacks: []string{
			srv.URL + "/600.jpg",
		},
	})
	if err != nil {
		t.Fatalf("Fetch: %v", err)
	}
	if mime != "image/jpeg" || !bytes.Equal(data, big) {
		t.Errorf("Fetch returned %s, %d bytes", mime, len(data))
	}
	files, _ := os.ReadDir(cache.Dir)
	if len(files) != 1 || !strings.HasSuffix(files[0].Name(), ".jpg") {
		t.Errorf("cache holds %v, want one .jpg", files)
	}

	// A second Fetch of the same candidate is served from disk: the
	// server sees no further hits.
	before := atomic.LoadInt32(&srv.hits)
	if _, _, err := c.Fetch(context.Background(), Candidate{
		Provider: "itunes", FullURL: srv.URL + "/1200.jpg",
		Fallbacks: []string{srv.URL + "/600.jpg"},
	}); err != nil {
		t.Fatal(err)
	}
	if got := atomic.LoadInt32(&srv.hits); got != before {
		t.Errorf("the cached image cost %d more request(s)", got-before)
	}

	// PNG is accepted and recorded as PNG.
	if _, mime, err := c.Fetch(context.Background(), Candidate{
		Provider: "musicbrainz", FullURL: srv.URL + "/cover.png"}); err != nil || mime != "image/png" {
		t.Errorf("PNG fetch: %v, %q", err, mime)
	}

	// A thumbnail-sized picture and a body that is not an image are
	// both refused — the device would render either as a smear or as
	// nothing, and both are worse than no cover.
	for _, path := range []string{"/tiny.jpg", "/junk.jpg"} {
		_, _, err := c.Fetch(context.Background(), Candidate{Provider: "itunes", FullURL: srv.URL + path})
		if !errors.Is(err, ErrNotAnImage) {
			t.Errorf("Fetch(%s) = %v, want ErrNotAnImage", path, err)
		}
	}
}

func TestFetchRefusesAnOversizeBody(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Write(bytes.Repeat([]byte{0xFF}, 4096))
	}))
	defer srv.Close()
	c := &Client{MaxBytes: 1024}
	_, _, err := c.Fetch(context.Background(), Candidate{Provider: "itunes", FullURL: srv.URL + "/big.jpg"})
	if !errors.Is(err, ErrTooLarge) {
		t.Errorf("Fetch = %v, want ErrTooLarge", err)
	}
}

func TestFetchRetriesA500AndGivesUpOnA404(t *testing.T) {
	var n int32
	body := coverJPEG(t, 400)
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/gone.jpg" {
			http.NotFound(w, r)
			return
		}
		if atomic.AddInt32(&n, 1) < 3 {
			http.Error(w, "later", http.StatusInternalServerError)
			return
		}
		w.Write(body)
	}))
	defer srv.Close()

	c := &Client{}
	if _, _, err := c.Fetch(context.Background(), Candidate{Provider: "x", FullURL: srv.URL + "/flaky.jpg"}); err != nil {
		t.Errorf("a 500 that clears on the third attempt was not retried: %v", err)
	}
	before := atomic.LoadInt32(&n)
	if _, _, err := c.Fetch(context.Background(), Candidate{Provider: "x", FullURL: srv.URL + "/gone.jpg"}); err == nil {
		t.Error("a 404 was not an error")
	}
	if got := atomic.LoadInt32(&n); got != before {
		t.Errorf("a 404 was retried %d time(s)", got-before)
	}
}

// TestFetchWaitsForARateLimitedProvider proves the Cover Art Archive
// hop is spaced like the search: the provider's limiter is consulted by
// Fetch, not only by Search.
func TestFetchWaitsForARateLimitedProvider(t *testing.T) {
	body := coverJPEG(t, 400)
	srv := newImageServer(t, map[string][]byte{"/front-500": body})
	clk := &fakeClock{now: time.Unix(1700000000, 0)}
	mb := &MusicBrainz{Now: clk.Now, Sleep: clk.Sleep}
	c := &Client{Providers: []Provider{mb}}
	cand := Candidate{Provider: "musicbrainz", FullURL: srv.URL + "/front-500"}

	for i := 0; i < 2; i++ {
		if _, _, err := c.Fetch(context.Background(), cand); err != nil {
			t.Fatalf("Fetch %d: %v", i, err)
		}
	}
	if len(clk.slept) != 1 || clk.slept[0] != time.Second {
		t.Errorf("two Cover Art Archive fetches waited %v, want one 1s gap", clk.slept)
	}
}

func TestNilCacheIsUsable(t *testing.T) {
	var c *Cache
	if _, _, ok := c.Image("itunes", "u"); ok {
		t.Error("a nil cache reported a hit")
	}
	if c.Missed(Query{Album: "x"}) {
		t.Error("a nil cache reported a miss on record")
	}
	if err := c.PutImage("itunes", "u", []byte{1}, "image/jpeg"); err != nil {
		t.Error(err)
	}
	if err := c.PutMiss(Query{Album: "x"}); err != nil {
		t.Error(err)
	}
}

func TestUserAgentNamesTheProject(t *testing.T) {
	ua := UserAgent()
	if !strings.HasPrefix(ua, "core-app/") ||
		!strings.Contains(ua, "(github.com/BrandonDedolph/ipod-core)") {
		t.Errorf("UserAgent() = %q", ua)
	}
	if strings.Contains(ua, "  ") || strings.TrimSpace(ua) != ua {
		t.Errorf("UserAgent() = %q: MusicBrainz parses this", ua)
	}
}

// Example of what the CLI prints, so the line format is covered.
func ExampleCandidate_String() {
	fmt.Println(Candidate{Provider: "itunes", Artist: "Morgan Wallen", Album: "I’m The Problem", Score: 1}.String())
	// Output: 1.00  itunes       Morgan Wallen — I’m The Problem
}
