package artfetch

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/url"
	"regexp"
	"strconv"
	"strings"
)

// ITunesBaseURL is the Search API root. It is a field on the provider
// and a package variable so a test can point the whole thing at an
// httptest server; no test in this repo makes a real network call, and
// the way that stays true is that there is exactly one place to
// redirect.
var ITunesBaseURL = "https://itunes.apple.com"

// ITunes searches the iTunes Search API.
//
//	GET /search?term=<artist album>&media=music&entity=album&limit=5
//
// No key and no documented rate limit (roughly 20 requests a minute is
// the number Apple's support notes have circulated); the interesting
// field is artworkUrl100, a thumbnail URL whose last path segment is
// the size. Rewriting that segment is how the 1200 px original is
// reached — there is no documented endpoint for it, but the pattern has
// held for a decade and the 600 and the untouched 100 are the fallbacks
// when it has not.
type ITunes struct {
	BaseURL   string
	HTTP      *http.Client
	UserAgent string
	// Limit is the number of results asked for (the plan's 5).
	Limit int
}

// Name implements Provider.
func (i *ITunes) Name() string { return "itunes" }

func (i *ITunes) base() string {
	if i.BaseURL != "" {
		return strings.TrimSuffix(i.BaseURL, "/")
	}
	return strings.TrimSuffix(ITunesBaseURL, "/")
}

func (i *ITunes) limit() int {
	if i.Limit > 0 {
		return i.Limit
	}
	return 5
}

// itunesResult is the subset of one search result this reads.
type itunesResult struct {
	CollectionID   int64  `json:"collectionId"`
	ArtistName     string `json:"artistName"`
	CollectionName string `json:"collectionName"`
	ArtworkURL100  string `json:"artworkUrl100"`
}

// Search implements Provider.
func (i *ITunes) Search(ctx context.Context, q Query) ([]Candidate, error) {
	term := strings.TrimSpace(q.Artist + " " + q.Album)
	if term == "" {
		return nil, fmt.Errorf("itunes: nothing to search for")
	}
	v := url.Values{}
	v.Set("term", term)
	v.Set("media", "music")
	v.Set("entity", "album")
	v.Set("limit", strconv.Itoa(i.limit()))
	u := i.base() + "/search?" + v.Encode()

	c := &Client{HTTP: i.HTTP, UserAgent: i.UserAgent}
	body, err := c.do(ctx, u, "application/json", 4<<20)
	if err != nil {
		return nil, err
	}
	var doc struct {
		Results []itunesResult `json:"results"`
	}
	if err := json.Unmarshal(body, &doc); err != nil {
		return nil, fmt.Errorf("itunes: %s did not return a search document: %w", u, err)
	}
	out := make([]Candidate, 0, len(doc.Results))
	for n, r := range doc.Results {
		if r.ArtworkURL100 == "" {
			continue
		}
		full, fallbacks := ITunesSizes(r.ArtworkURL100)
		out = append(out, Candidate{
			Provider:  i.Name(),
			Artist:    r.ArtistName,
			Album:     r.CollectionName,
			ID:        strconv.FormatInt(r.CollectionID, 10),
			ThumbURL:  rewriteArtwork(r.ArtworkURL100, 200),
			FullURL:   full,
			Fallbacks: fallbacks,
			// iTunes returns no relevance score, only an order. Turn
			// the position into a rank so a name mismatch still sorts
			// sensibly; it can never reach --yes either way.
			Rank: 1 - float64(n)/float64(max(len(doc.Results), 1)),
		})
	}
	return out, nil
}

// ITunesSizes turns an artworkUrl100 into the URL to download and the
// smaller ones to fall back to.
func ITunesSizes(artwork100 string) (full string, fallbacks []string) {
	full = rewriteArtwork(artwork100, 1200)
	if six := rewriteArtwork(artwork100, 600); six != full {
		fallbacks = append(fallbacks, six)
	}
	if artwork100 != full {
		fallbacks = append(fallbacks, artwork100)
	}
	return full, fallbacks
}

// artworkSize matches the "100x100bb.jpg" last segment. The suffix
// letters vary ("bb", "bf", none) and the extension can be .png, so
// only the numbers are replaced and the rest is kept exactly.
var artworkSize = regexp.MustCompile(`/\d+x\d+([a-z]*\.(?:jpg|png))$`)

func rewriteArtwork(u string, px int) string {
	if !artworkSize.MatchString(u) {
		return u
	}
	return artworkSize.ReplaceAllString(u, fmt.Sprintf("/%dx%d$1", px, px))
}
