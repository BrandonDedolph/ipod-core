// Package artfetch finds album cover art on the public music metadata
// services and hands the bytes back, ranked, so a human can confirm the
// match before anything is written.
//
// Two providers, in this order:
//
//	iTunes Search API   https://itunes.apple.com/search  (no key, big images)
//	MusicBrainz + CAA   https://musicbrainz.org/ws/2/release/
//	                    https://coverartarchive.org/release/<mbid>/front-500
//
// iTunes goes first because it needs no key, answers in one request and
// serves a 1200 px original for nearly every album; MusicBrainz is the
// fallback, and it is the one with rules: a descriptive User-Agent is
// mandatory and a client may make at most one request per second. Both
// are honoured here (see MusicBrainz.Limit and Client.UserAgent), and
// the limiter's clock is injectable so the test that proves the spacing
// does not have to sleep.
//
// # Confirm first
//
// Nothing in this package decides that a picture belongs to an album.
// Lookup returns candidates with a Score (see Score) and the caller —
// the Fix screen, or `core art --fetch` — accepts one. The CLI's --yes
// accepts the top candidate only when it scored 0.9 or better, which is
// "the artist and the album match once the edition decorations are
// stripped". Wrong art on the iPod is the most visible mistake this
// program can make, so a weak match is printed and skipped, never
// guessed.
//
// # Licensing
//
// The cover images these services return are the rights-holders' work,
// not this project's and not the services'. iTunes artwork is served
// under Apple's terms for the Search API; Cover Art Archive images
// carry whatever licence the uploader gave them. This package therefore
// writes a fetched image to exactly two places: the user's own music
// files (an embedded FLAC PICTURE block, and cover.jpg beside them) and
// the user's own cache directory. It never writes one into this
// repository, into a release artifact, or anywhere shared, and it
// attaches no licence claim of its own to what it downloads.
package artfetch

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"image"
	_ "image/jpeg" // image.DecodeConfig must know the two formats we accept
	_ "image/png"
	"io"
	"net/http"
	"sort"
	"strings"
	"time"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/version"
)

// Limits on one fetched image. MinPixels rejects the placeholder and
// favicon-sized junk some search results point at — the device renders
// a 120 px sidecar, so anything under 300 is a downgrade on what the
// user could scan themselves. MaxImageBytes refuses to buffer a body
// that is not a cover at all.
const (
	MinPixels     = 300
	MaxImageBytes = 10 << 20
)

// RequestTimeout bounds one HTTP request (search or image). The plan's
// 10 s: generous for a JSON reply on a bad connection, mean for an
// endpoint that has stopped answering.
const RequestTimeout = 10 * time.Second

// MaxRedirects is the redirect budget for any request this package
// makes. Cover Art Archive answers a front-500 with two redirects (to
// archive.org and then to a specific node), so the budget cannot be
// zero; five is room for that plus a service reorganising itself, and
// not a loop.
const MaxRedirects = 5

// Retries on a 5xx or a 429, with backoff. Three attempts total.
const (
	maxAttempts  = 3
	retryBackoff = 500 * time.Millisecond
)

// Errors callers distinguish.
var (
	// ErrNoMatch: every provider answered, none had this album.
	ErrNoMatch = errors.New("artfetch: no cover art found for this album")
	// ErrNotAnImage: the body that arrived is not a JPEG or a PNG, or
	// is too small to be a cover.
	ErrNotAnImage = errors.New("artfetch: the fetched bytes are not a usable cover image")
	// ErrTooLarge: the body is past MaxImageBytes.
	ErrTooLarge = errors.New("artfetch: the image is too large")
)

// Query is one album to find art for.
type Query struct{ Artist, Album string }

// Candidate is one possible cover, as a provider described it.
//
// FullURL is what Fetch downloads; Fallbacks are the same picture at
// smaller sizes, tried in order when FullURL is not there (iTunes
// promises 1200×1200 for nearly every album, not for every album).
// ThumbURL is a ~200 px version for a confirmation UI, and is never
// what gets embedded.
type Candidate struct {
	Provider string
	Artist   string
	Album    string
	// ID is the provider's own identifier (an iTunes collectionId, a
	// MusicBrainz release MBID) — for logs and for a later "not this
	// one, the other release" UI.
	ID        string
	ThumbURL  string
	FullURL   string
	Fallbacks []string
	Score     float64
	// Rank is the provider's own confidence, normalised to 0..1. It
	// only matters when nothing matched by name; see Score.
	Rank float64
}

// String is the one line the CLI and the log print.
func (c Candidate) String() string {
	return fmt.Sprintf("%.2f  %-12s %s — %s", c.Score, c.Provider, c.Artist, c.Album)
}

// Provider is one search service.
type Provider interface {
	Name() string
	Search(ctx context.Context, q Query) ([]Candidate, error)
}

// waiter is implemented by a provider with a rate limit of its own.
// Client.Fetch consults it before downloading that provider's image, so
// the MusicBrainz one-per-second applies to the Cover Art Archive hop
// too rather than only to the search.
type waiter interface {
	Wait(ctx context.Context) error
}

// Client runs the providers and fetches the bytes.
//
// The zero value is not usable — use New, or fill Providers in. Every
// field exists so a test can replace it; no test in this package ever
// makes a real network call.
type Client struct {
	Providers []Provider
	HTTP      *http.Client
	Cache     *Cache
	UserAgent string

	// StopAtScore ends the provider walk early once a candidate has
	// scored at least this much. The default (1.0, from New) means an
	// exact iTunes match spares MusicBrainz a request it would gain
	// nothing from — which is the polite reading of a rate limit, not
	// just the fast one. Set it above 1 to always ask everybody.
	StopAtScore float64

	// OnWarn, when set, is told about a provider that failed while
	// another answered. A failure of ALL providers is returned as an
	// error instead.
	OnWarn func(provider string, err error)

	// MinPixels and MaxBytes default to the package constants.
	MinPixels int
	MaxBytes  int64
}

// UserAgent is the identification MusicBrainz requires and everything
// else here sends anyway. The version is this build's.
func UserAgent() string {
	v := strings.TrimSpace(version.Version)
	if v == "" {
		v = "(devel)"
	}
	return "core-app/" + v + " (github.com/BrandonDedolph/ipod-core)"
}

// New returns a client with both providers, the shared cache and the
// mandatory User-Agent. A cache that cannot be located (no writable
// user cache directory) is not fatal: the client runs without one.
func New() *Client {
	ua := UserAgent()
	hc := &http.Client{
		Timeout:       RequestTimeout,
		CheckRedirect: limitRedirects,
	}
	cache, _ := DefaultCache()
	return &Client{
		Providers: []Provider{
			&ITunes{HTTP: hc, UserAgent: ua},
			&MusicBrainz{HTTP: hc, UserAgent: ua},
		},
		HTTP:        hc,
		Cache:       cache,
		UserAgent:   ua,
		StopAtScore: 1.0,
	}
}

func limitRedirects(req *http.Request, via []*http.Request) error {
	if len(via) >= MaxRedirects {
		return fmt.Errorf("artfetch: stopped after %d redirects", MaxRedirects)
	}
	return nil
}

func (c *Client) httpClient() *http.Client {
	if c.HTTP != nil {
		return c.HTTP
	}
	return &http.Client{Timeout: RequestTimeout, CheckRedirect: limitRedirects}
}

func (c *Client) minPixels() int {
	if c.MinPixels > 0 {
		return c.MinPixels
	}
	return MinPixels
}

func (c *Client) maxBytes() int64 {
	if c.MaxBytes > 0 {
		return c.MaxBytes
	}
	return MaxImageBytes
}

func (c *Client) warn(provider string, err error) {
	if c.OnWarn != nil {
		c.OnWarn(provider, err)
	}
}

// Lookup asks the providers about one album and returns the candidates
// scored and sorted, best first.
//
// A provider that fails is a warning as long as another one answered;
// only an album that no provider could be asked about is an error. An
// album that every provider answered about with nothing is
// ErrNoMatch, and that answer is remembered in the negative cache for
// Cache.NegativeTTL — a library with forty obscure bootlegs in it
// should not re-ask the internet about all forty on every inspect.
func (c *Client) Lookup(ctx context.Context, artist, album string) ([]Candidate, error) {
	return c.Find(ctx, Query{Artist: artist, Album: album})
}

// Find is Lookup in the Query form the librarian uses.
func (c *Client) Find(ctx context.Context, q Query) ([]Candidate, error) {
	if strings.TrimSpace(q.Album) == "" {
		return nil, errors.New("artfetch: an album name is required")
	}
	if c.Cache != nil && c.Cache.Missed(q) {
		return nil, fmt.Errorf("%w: %s — %s (remembered)", ErrNoMatch, q.Artist, q.Album)
	}

	var (
		out    []Candidate
		errs   []error
		asked  int
		stopAt = c.StopAtScore
	)
	if stopAt <= 0 {
		stopAt = 1.0
	}
	for _, p := range c.Providers {
		asked++
		cands, err := p.Search(ctx, q)
		if err != nil {
			errs = append(errs, fmt.Errorf("%s: %w", p.Name(), err))
			c.warn(p.Name(), err)
			continue
		}
		for i := range cands {
			cands[i].Provider = p.Name()
			cands[i].Score = Score(q, cands[i])
		}
		out = append(out, cands...)
		if best(out) >= stopAt {
			break
		}
	}
	if len(errs) == asked && asked > 0 {
		return nil, fmt.Errorf("artfetch: every provider failed: %w", errors.Join(errs...))
	}
	out = dedupe(out)
	sort.SliceStable(out, func(i, j int) bool { return out[i].Score > out[j].Score })
	if len(out) == 0 {
		if c.Cache != nil {
			_ = c.Cache.PutMiss(q)
		}
		return nil, fmt.Errorf("%w: %s — %s", ErrNoMatch, q.Artist, q.Album)
	}
	return out, nil
}

func best(c []Candidate) float64 {
	b := 0.0
	for _, x := range c {
		if x.Score > b {
			b = x.Score
		}
	}
	return b
}

// dedupe collapses candidates that name the same record. MusicBrainz
// returns one release per pressing — clean, explicit, the 2019 vinyl —
// and they all carry the same cover; printing six identical lines with
// the same score would make a confirmation screen useless.
func dedupe(in []Candidate) []Candidate {
	seen := map[string]int{}
	var out []Candidate
	for _, c := range in {
		key := c.Provider + "\x00" + NormKey(c.Artist) + "\x00" + NormKey(c.Album)
		if i, ok := seen[key]; ok {
			if c.Score > out[i].Score {
				out[i] = c
			}
			continue
		}
		seen[key] = len(out)
		out = append(out, c)
	}
	return out
}

// Fetch downloads a candidate's picture and returns the bytes and their
// MIME type.
//
// The bytes are checked before they are returned: at most MaxBytes,
// decodable by image.DecodeConfig as JPEG or PNG, and at least
// MinPixels on both sides. A cache hit skips the network entirely; a
// fetched image is written to the cache keyed by provider+URL.
//
// The cache is consulted for every URL — the preferred size and each
// fallback — before any request is made, so an album whose 1200 px
// version does not exist is not re-asked about it on every run just
// because the 600 is the one on disk.
func (c *Client) Fetch(ctx context.Context, cand Candidate) ([]byte, string, error) {
	var urls []string
	for _, u := range append([]string{cand.FullURL}, cand.Fallbacks...) {
		if strings.TrimSpace(u) != "" {
			urls = append(urls, u)
		}
	}
	if c.Cache != nil {
		for _, u := range urls {
			if data, mime, ok := c.Cache.Image(cand.Provider, u); ok {
				return data, mime, nil
			}
		}
	}
	var errs []error
	for _, u := range urls {
		data, mime, err := c.fetchOne(ctx, cand, u)
		if err != nil {
			errs = append(errs, err)
			continue
		}
		if c.Cache != nil {
			_ = c.Cache.PutImage(cand.Provider, u, data, mime)
		}
		return data, mime, nil
	}
	if len(errs) == 0 {
		return nil, "", fmt.Errorf("artfetch: %s candidate %q has no image URL", cand.Provider, cand.Album)
	}
	return nil, "", fmt.Errorf("artfetch: fetching %s art for %q: %w",
		cand.Provider, cand.Album, errors.Join(errs...))
}

func (c *Client) fetchOne(ctx context.Context, cand Candidate, url string) ([]byte, string, error) {
	// A provider with a rate limit owns its image host too (the Cover
	// Art Archive hop is MusicBrainz's), so wait here, not only in
	// Search.
	for _, p := range c.Providers {
		if p.Name() == cand.Provider {
			if w, ok := p.(waiter); ok {
				if err := w.Wait(ctx); err != nil {
					return nil, "", err
				}
			}
			break
		}
	}
	body, err := c.do(ctx, url, "image/jpeg, image/png;q=0.9, image/*;q=0.8", c.maxBytes())
	if err != nil {
		return nil, "", err
	}
	mime, err := c.validate(body)
	if err != nil {
		return nil, "", fmt.Errorf("%s: %w", url, err)
	}
	return body, mime, nil
}

// validate is the whole trust boundary for a downloaded picture: it has
// to decode, be one of the two formats the device's art pipeline reads,
// and be big enough to be a cover.
func (c *Client) validate(body []byte) (string, error) {
	cfg, format, err := image.DecodeConfig(bytes.NewReader(body))
	if err != nil {
		return "", fmt.Errorf("%w: %v", ErrNotAnImage, err)
	}
	switch format {
	case "jpeg", "png":
	default:
		return "", fmt.Errorf("%w: it decodes as %s, not JPEG or PNG", ErrNotAnImage, format)
	}
	if min := c.minPixels(); cfg.Width < min || cfg.Height < min {
		return "", fmt.Errorf("%w: %dx%d is under the %d px minimum",
			ErrNotAnImage, cfg.Width, cfg.Height, min)
	}
	if format == "png" {
		return "image/png", nil
	}
	return "image/jpeg", nil
}

// do is one GET with the User-Agent, the timeout, the size limit and
// the retry policy. 5xx and 429 are retried with backoff; a 404 is not
// (the album simply has no art there) and neither is a 4xx of any other
// kind.
func (c *Client) do(ctx context.Context, url, accept string, limit int64) ([]byte, error) {
	var lastErr error
	for attempt := 1; attempt <= maxAttempts; attempt++ {
		body, retry, err := c.attempt(ctx, url, accept, limit)
		if err == nil {
			return body, nil
		}
		lastErr = err
		if !retry || attempt == maxAttempts {
			break
		}
		select {
		case <-ctx.Done():
			return nil, ctx.Err()
		case <-time.After(time.Duration(attempt) * retryBackoff):
		}
	}
	return nil, lastErr
}

func (c *Client) attempt(ctx context.Context, url, accept string, limit int64) (body []byte, retry bool, err error) {
	ctx, cancel := context.WithTimeout(ctx, RequestTimeout)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return nil, false, fmt.Errorf("artfetch: %w", err)
	}
	ua := c.UserAgent
	if ua == "" {
		ua = UserAgent()
	}
	req.Header.Set("User-Agent", ua)
	if accept != "" {
		req.Header.Set("Accept", accept)
	}
	resp, err := c.httpClient().Do(req)
	if err != nil {
		return nil, true, fmt.Errorf("artfetch: GET %s: %w", url, err)
	}
	defer resp.Body.Close()
	switch {
	case resp.StatusCode == http.StatusOK:
	case resp.StatusCode >= 500 || resp.StatusCode == http.StatusTooManyRequests:
		return nil, true, fmt.Errorf("artfetch: GET %s: %s", url, resp.Status)
	default:
		return nil, false, fmt.Errorf("artfetch: GET %s: %s", url, resp.Status)
	}
	// One byte past the limit is how an overlong body is caught
	// without reading a stream that never ends.
	b, err := io.ReadAll(io.LimitReader(resp.Body, limit+1))
	if err != nil {
		return nil, true, fmt.Errorf("artfetch: reading %s: %w", url, err)
	}
	if int64(len(b)) > limit {
		return nil, false, fmt.Errorf("%w: %s is over %d bytes", ErrTooLarge, url, limit)
	}
	return b, false, nil
}

// key is the cache name for one image: the provider and the URL, so the
// same picture offered by two services is stored once per service and a
// URL change is a miss rather than a stale hit.
func key(provider, url string) string {
	sum := sha256.Sum256([]byte(provider + "\x00" + url))
	return hex.EncodeToString(sum[:16])
}
