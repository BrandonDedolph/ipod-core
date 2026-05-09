// Package artistart fetches artist photos from open-source music
// databases for the binary tagcache.
//
// Chain (all free, no API keys, all rate-limited):
//
//  1. MusicBrainz artist search        artist name  -> MBID
//  2. MusicBrainz artist lookup        MBID         -> Wikidata QID (via url-rels)
//  3. Wikidata entity data             QID          -> Wikimedia Commons filename (P18)
//  4. Wikimedia Commons file URL       filename     -> actual image URL
//  5. HTTP GET that URL                              -> JPEG/PNG bytes
//
// The MB API rate-limits to 1 req/sec per User-Agent. A library of 50
// artists therefore takes at minimum ~100 seconds of MB calls (search +
// lookup), plus a few hundred ms each for the Wikipedia / Commons
// hops. Plan for a few minutes per fresh fetch; cache aggressively.
//
// Design choices:
//
//   - We do not require an API key anywhere. MusicBrainz, Wikidata, and
//     Commons are all free + token-less; trading the polish of keyed
//     APIs (Last.fm, Spotify, fanart.tv) for a simpler distribution
//     story.
//   - The User-Agent string identifies us per MusicBrainz's policy so
//     we don't get rate-limit-blocked harder than usual. Bump the
//     version when the project name changes.
//   - The fetcher returns raw image bytes (whatever Commons serves —
//     typically JPEG, sometimes PNG). Callers downscale + recompress
//     before storing into the .tcdb.
//   - Failures are not fatal: an artist with no MB match, no Wikidata
//     link, or no Commons image returns ErrNotFound and the build
//     continues. The user sees fewer artist thumbs, not a build break.
package artistart

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"time"
)

// ErrNotFound is returned when any step in the fetch chain comes up
// empty (no MB match, no Wikidata link, no P18, etc.). Distinct from
// network / parse errors so callers can render a graceful "no photo"
// outcome rather than aborting the whole build.
var ErrNotFound = errors.New("artistart: not found")

// userAgent satisfies MusicBrainz's policy that scripted clients must
// identify themselves. They suggest "Application/Version (contact)".
// Keep this stable so MB's per-UA budgets aren't double-counted across
// runs.
const userAgent = "core-tagcache/0.1 (https://github.com/BrandonDedolph/ipod_theme)"

// Fetcher serializes calls to MB through a 1-req/sec ticker (their
// stated rate limit). Wikidata / Commons aren't rate-limited as
// aggressively but go through the same client to share the http
// connection pool and timeouts.
type Fetcher struct {
	HTTP *http.Client
	tick *time.Ticker
}

// NewFetcher returns a Fetcher with sensible defaults: 30 s overall
// HTTP timeout, MB requests paced at 1/sec.
func NewFetcher() *Fetcher {
	return &Fetcher{
		HTTP: &http.Client{Timeout: 30 * time.Second},
		tick: time.NewTicker(time.Second),
	}
}

// Close releases the rate-limit ticker. Idempotent.
func (f *Fetcher) Close() {
	if f.tick != nil {
		f.tick.Stop()
		f.tick = nil
	}
}

// Fetch runs the full chain for `artistName` and returns the raw image
// bytes the source served, plus that source's URL (for logging /
// cache keys). Returns ErrNotFound when every available source comes
// up empty.
//
// Order of attempts:
//
//  1. Deezer's public Search API — no auth, broad coverage of the
//     modern catalog. One JSON call + one image download.
//  2. MusicBrainz → Wikidata → Commons — kept as a backstop for the
//     rare niche artist Deezer doesn't index.
//
// Either source can fail without aborting; we only return the
// originating error when *both* paths fail.
func (f *Fetcher) Fetch(ctx context.Context, artistName string) ([]byte, string, error) {
	if imgURL, err := f.fetchDeezer(ctx, artistName); err == nil {
		bytes, derr := f.httpGet(ctx, imgURL)
		if derr == nil {
			return bytes, imgURL, nil
		}
		/* Download failure on a Deezer URL — surprising but possible
		 * (CDN hiccup). Fall through to MB rather than giving up. */
	}
	mbid, err := f.searchMBID(ctx, artistName)
	if err != nil {
		return nil, "", fmt.Errorf("mb search %q: %w", artistName, err)
	}
	qid, err := f.lookupWikidataQID(ctx, mbid)
	if err != nil {
		return nil, "", fmt.Errorf("mb lookup mbid=%s: %w", mbid, err)
	}
	filename, err := f.wikidataImageFilename(ctx, qid)
	if err != nil {
		return nil, "", fmt.Errorf("wikidata qid=%s: %w", qid, err)
	}
	imgURL, err := f.commonsImageURL(ctx, filename)
	if err != nil {
		return nil, "", fmt.Errorf("commons file=%s: %w", filename, err)
	}
	bytes, err := f.httpGet(ctx, imgURL)
	if err != nil {
		return nil, imgURL, fmt.Errorf("download %s: %w", imgURL, err)
	}
	return bytes, imgURL, nil
}

// searchMBID asks MB for the artist's MBID. Picks the top score match;
// MB's scoring is reasonably accurate for canonical names.
func (f *Fetcher) searchMBID(ctx context.Context, name string) (string, error) {
	f.waitMB(ctx)
	q := url.Values{}
	q.Set("query", "artist:"+name)
	q.Set("fmt", "json")
	q.Set("limit", "1")
	u := "https://musicbrainz.org/ws/2/artist/?" + q.Encode()

	body, err := f.httpGet(ctx, u)
	if err != nil {
		return "", err
	}
	var resp struct {
		Artists []struct {
			ID    string `json:"id"`
			Score int    `json:"score"`
			Name  string `json:"name"`
		} `json:"artists"`
	}
	if err := json.Unmarshal(body, &resp); err != nil {
		return "", fmt.Errorf("parse: %w", err)
	}
	if len(resp.Artists) == 0 {
		return "", ErrNotFound
	}
	a := resp.Artists[0]
	/* MB's score is 0..100. Anything below ~85 is usually a wrong
	 * artist (e.g. matching a remix of the name). The rate-limit cost
	 * of guessing wrong is high, so be picky. */
	if a.Score < 85 {
		return "", ErrNotFound
	}
	return a.ID, nil
}

// lookupWikidataQID fetches the artist's external relationships and
// extracts the wikidata QID (a Q-prefixed integer like Q23426).
func (f *Fetcher) lookupWikidataQID(ctx context.Context, mbid string) (string, error) {
	f.waitMB(ctx)
	u := "https://musicbrainz.org/ws/2/artist/" + mbid + "?fmt=json&inc=url-rels"
	body, err := f.httpGet(ctx, u)
	if err != nil {
		return "", err
	}
	var resp struct {
		Relations []struct {
			Type string `json:"type"`
			URL  struct {
				Resource string `json:"resource"`
			} `json:"url"`
		} `json:"relations"`
	}
	if err := json.Unmarshal(body, &resp); err != nil {
		return "", fmt.Errorf("parse: %w", err)
	}
	for _, r := range resp.Relations {
		if r.Type == "wikidata" {
			/* URL looks like https://www.wikidata.org/wiki/Q23426 */
			i := strings.LastIndex(r.URL.Resource, "/")
			if i >= 0 && i+1 < len(r.URL.Resource) {
				return r.URL.Resource[i+1:], nil
			}
		}
	}
	return "", ErrNotFound
}

// wikidataImageFilename pulls the value of the P18 ("image") claim
// from the entity. P18's mainsnak.datavalue.value is the bare filename
// on Commons (no "File:" prefix), which is what the Commons file API
// expects.
//
// Wikidata's `datavalue.value` is polymorphic — string for
// commonsmedia, object for time / globe-coords / wikibase-entityid /
// etc. We only care about P18 (commonsmedia) so we capture every
// claim's value as RawMessage and only attempt the string decode for
// P18's first entry. That sidesteps Go's strict-typed unmarshal
// failing on the very first foreign-typed datavalue it walks past.
func (f *Fetcher) wikidataImageFilename(ctx context.Context, qid string) (string, error) {
	u := "https://www.wikidata.org/wiki/Special:EntityData/" + qid + ".json"
	body, err := f.httpGet(ctx, u)
	if err != nil {
		return "", err
	}
	var resp struct {
		Entities map[string]struct {
			Claims map[string][]struct {
				MainSnak struct {
					DataValue struct {
						Value json.RawMessage `json:"value"`
					} `json:"datavalue"`
				} `json:"mainsnak"`
			} `json:"claims"`
		} `json:"entities"`
	}
	if err := json.Unmarshal(body, &resp); err != nil {
		return "", fmt.Errorf("parse: %w", err)
	}
	ent, ok := resp.Entities[qid]
	if !ok {
		return "", ErrNotFound
	}
	p18, ok := ent.Claims["P18"]
	if !ok || len(p18) == 0 {
		return "", ErrNotFound
	}
	var name string
	if err := json.Unmarshal(p18[0].MainSnak.DataValue.Value, &name); err != nil {
		return "", fmt.Errorf("decode P18 value: %w", err)
	}
	if name == "" {
		return "", ErrNotFound
	}
	return name, nil
}

// commonsImageURL resolves a Commons filename to its hosted URL.
// We use the imageinfo API with iiurlwidth so Commons returns a
// downscaled thumbnail rather than a multi-megabyte original.
func (f *Fetcher) commonsImageURL(ctx context.Context, filename string) (string, error) {
	q := url.Values{}
	q.Set("action", "query")
	q.Set("titles", "File:"+filename)
	q.Set("prop", "imageinfo")
	q.Set("iiprop", "url")
	q.Set("iiurlwidth", "512")     /* max width; Commons returns a sized thumb */
	q.Set("format", "json")
	u := "https://commons.wikimedia.org/w/api.php?" + q.Encode()

	body, err := f.httpGet(ctx, u)
	if err != nil {
		return "", err
	}
	var resp struct {
		Query struct {
			Pages map[string]struct {
				ImageInfo []struct {
					URL       string `json:"url"`
					ThumbURL  string `json:"thumburl"`
				} `json:"imageinfo"`
			} `json:"pages"`
		} `json:"query"`
	}
	if err := json.Unmarshal(body, &resp); err != nil {
		return "", fmt.Errorf("parse: %w", err)
	}
	for _, page := range resp.Query.Pages {
		if len(page.ImageInfo) == 0 {
			continue
		}
		ii := page.ImageInfo[0]
		if ii.ThumbURL != "" {
			return ii.ThumbURL, nil
		}
		if ii.URL != "" {
			return ii.URL, nil
		}
	}
	return "", ErrNotFound
}

// httpGet does a GET with the project User-Agent, returns the body
// bytes. Non-2xx responses become errors.
func (f *Fetcher) httpGet(ctx context.Context, u string) ([]byte, error) {
	req, err := http.NewRequestWithContext(ctx, "GET", u, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("User-Agent", userAgent)
	return f.do(req)
}

// do executes a prepared request and returns the body bytes. Shared
// by httpGet (which sets the UA) and the Spotify path (which sets
// Authorization on top of the UA). Non-2xx responses become errors.
func (f *Fetcher) do(req *http.Request) ([]byte, error) {
	resp, err := f.HTTP.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(resp.Body)
	if err != nil {
		return nil, err
	}
	if resp.StatusCode/100 != 2 {
		snippet := string(body)
		if len(snippet) > 200 {
			snippet = snippet[:200]
		}
		return nil, fmt.Errorf("http %d: %s", resp.StatusCode, snippet)
	}
	return body, nil
}

// waitMB blocks until the next MB rate-limit slot opens. Honors ctx
// cancellation so callers can abort a long fetch run cleanly.
func (f *Fetcher) waitMB(ctx context.Context) {
	if f.tick == nil {
		return
	}
	select {
	case <-f.tick.C:
	case <-ctx.Done():
	}
}
