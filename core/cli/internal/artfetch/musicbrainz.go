package artfetch

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"sync"
	"time"
)

// Service roots. Package variables for the same reason ghrelease has
// one: a test points the package somewhere else and a real request in a
// test becomes impossible to write by accident.
var (
	MusicBrainzBaseURL = "https://musicbrainz.org"
	CoverArtBaseURL    = "https://coverartarchive.org"
)

// MusicBrainzInterval is the rate limit MusicBrainz asks every client
// to honour: one request per second, averaged. It is not advisory —
// clients that ignore it get blocked by User-Agent — and it applies to
// the Cover Art Archive hop as well, which is why Client.Fetch waits on
// the provider's limiter too.
const MusicBrainzInterval = time.Second

// MusicBrainz searches the release index and turns each hit into a
// Cover Art Archive URL.
//
//	GET /ws/2/release/?query=artist:"A" AND release:"B"&fmt=json&limit=5
//	GET https://coverartarchive.org/release/<mbid>/front-500
//
// Two rules the service states and this honours: a descriptive
// User-Agent naming the application and a contact URL (Client.UserAgent,
// "core-app/<version> (github.com/BrandonDedolph/ipod-core)"), and at
// most one request a second.
//
// The front-500 answers with two redirects — coverartarchive.org to
// archive.org to a storage node — which is why MaxRedirects is not
// zero. A 404 there means the release is catalogued but nobody has
// uploaded a cover, which is common and is not an error.
type MusicBrainz struct {
	BaseURL    string
	CAABaseURL string
	HTTP       *http.Client
	UserAgent  string
	Limit      int

	// Interval overrides MusicBrainzInterval (tests only — making it
	// shorter against the real service is the thing that gets an
	// application blocked).
	Interval time.Duration
	// Now and Sleep are the limiter's clock. Both default to the real
	// ones; the test that proves the spacing injects a fake pair and
	// runs in no time at all.
	Now   func() time.Time
	Sleep func(ctx context.Context, d time.Duration) error

	mu   sync.Mutex
	last time.Time
}

// Name implements Provider.
func (m *MusicBrainz) Name() string { return "musicbrainz" }

func (m *MusicBrainz) base() string {
	if m.BaseURL != "" {
		return strings.TrimSuffix(m.BaseURL, "/")
	}
	return strings.TrimSuffix(MusicBrainzBaseURL, "/")
}

func (m *MusicBrainz) caaBase() string {
	if m.CAABaseURL != "" {
		return strings.TrimSuffix(m.CAABaseURL, "/")
	}
	return strings.TrimSuffix(CoverArtBaseURL, "/")
}

func (m *MusicBrainz) limit() int {
	if m.Limit > 0 {
		return m.Limit
	}
	return 5
}

func (m *MusicBrainz) interval() time.Duration {
	if m.Interval > 0 {
		return m.Interval
	}
	return MusicBrainzInterval
}

func (m *MusicBrainz) now() time.Time {
	if m.Now != nil {
		return m.Now()
	}
	return time.Now()
}

func (m *MusicBrainz) sleep(ctx context.Context, d time.Duration) error {
	if m.Sleep != nil {
		return m.Sleep(ctx, d)
	}
	t := time.NewTimer(d)
	defer t.Stop()
	select {
	case <-ctx.Done():
		return ctx.Err()
	case <-t.C:
		return nil
	}
}

// Wait blocks until this provider is allowed to make a request, and
// books the slot. It is the token bucket of size one the service asks
// for: requests are spaced by at least Interval, and a caller that has
// been idle longer than that goes straight through.
//
// It implements the unexported waiter interface, so Client.Fetch waits
// here before downloading a Cover Art Archive image too.
func (m *MusicBrainz) Wait(ctx context.Context) error {
	m.mu.Lock()
	now := m.now()
	wait := time.Duration(0)
	if !m.last.IsZero() {
		if gap := now.Sub(m.last); gap < m.interval() {
			wait = m.interval() - gap
		}
	}
	// The slot is booked before the lock is released, so two goroutines
	// asking at once queue rather than both deciding they may go.
	m.last = now.Add(wait)
	m.mu.Unlock()

	if wait <= 0 {
		return nil
	}
	return m.sleep(ctx, wait)
}

// mbRelease is the subset of one search result this reads.
type mbRelease struct {
	ID     string `json:"id"`
	Title  string `json:"title"`
	Score  int    `json:"score"` // 0..100, the service's own relevance
	Credit []struct {
		Name string `json:"name"`
	} `json:"artist-credit"`
}

func (r mbRelease) artist() string {
	names := make([]string, 0, len(r.Credit))
	for _, c := range r.Credit {
		if c.Name != "" {
			names = append(names, c.Name)
		}
	}
	return strings.Join(names, ", ")
}

// Search implements Provider.
func (m *MusicBrainz) Search(ctx context.Context, q Query) ([]Candidate, error) {
	query := lucene("release", q.Album)
	if a := strings.TrimSpace(q.Artist); a != "" {
		query = lucene("artist", a) + " AND " + query
	}
	v := url.Values{}
	v.Set("query", query)
	v.Set("fmt", "json")
	v.Set("limit", strconv.Itoa(m.limit()))
	u := m.base() + "/ws/2/release/?" + v.Encode()

	if err := m.Wait(ctx); err != nil {
		return nil, err
	}
	c := &Client{HTTP: m.HTTP, UserAgent: m.UserAgent}
	body, err := c.do(ctx, u, "application/json", 8<<20)
	if err != nil {
		return nil, err
	}
	var doc struct {
		Releases []mbRelease `json:"releases"`
	}
	if err := json.Unmarshal(body, &doc); err != nil {
		return nil, fmt.Errorf("musicbrainz: %s did not return a search document: %w", u, err)
	}
	out := make([]Candidate, 0, len(doc.Releases))
	for _, r := range doc.Releases {
		if r.ID == "" {
			continue
		}
		front := m.caaBase() + "/release/" + url.PathEscape(r.ID) + "/front-"
		out = append(out, Candidate{
			Provider: m.Name(),
			Artist:   r.artist(),
			Album:    r.Title,
			ID:       r.ID,
			ThumbURL: front + "250",
			FullURL:  front + "500",
			// The original upload, for a release whose 500 px
			// derivative has not been generated.
			Fallbacks: []string{strings.TrimSuffix(front, "-")},
			Rank:      float64(r.Score) / 100,
		})
	}
	return out, nil
}

// lucene builds one `field:"value"` term for the search index, with the
// two characters that would otherwise end the term escaped. The value
// is a folder name off the user's disk: it must not be able to write
// the query.
func lucene(field, value string) string {
	value = strings.ReplaceAll(value, `\`, `\\`)
	value = strings.ReplaceAll(value, `"`, `\"`)
	return field + `:"` + value + `"`
}
