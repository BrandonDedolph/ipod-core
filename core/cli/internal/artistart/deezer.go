package artistart

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/url"
)

// fetchDeezer queries Deezer's public Search API for `name` and
// returns the largest reasonable artist-photo URL from the top match.
//
// Deezer is the primary art source because it covers Spotify-grade
// modern catalog (including indie / niche artists like Pink Sweat$,
// Rex Orange County, Solon Holt that the MusicBrainz + Wikipedia
// chain misses) and — uniquely among the major sources — requires no
// API key, no OAuth flow, no signup. One free GET, JSON response,
// pick a picture URL.
//
// Image sizes Deezer offers per artist:
//
//	picture_small    56 ×  56
//	picture_medium  250 × 250
//	picture_big     500 × 500     ← we use this one
//	picture_xl     1000 × 1000
//
// 500×500 is the sweet spot: small enough that the download is fast
// (~30 KB per artist), large enough that the box-average downscale to
// 128×128 still has detail to work with.
func (f *Fetcher) fetchDeezer(ctx context.Context, name string) (string, error) {
	q := url.Values{}
	q.Set("q", name)
	q.Set("limit", "1")
	u := "https://api.deezer.com/search/artist?" + q.Encode()

	req, err := http.NewRequestWithContext(ctx, "GET", u, nil)
	if err != nil {
		return "", err
	}
	req.Header.Set("User-Agent", userAgent)

	body, err := f.do(req)
	if err != nil {
		return "", err
	}
	var resp struct {
		Data []struct {
			Name       string `json:"name"`
			PictureBig string `json:"picture_big"`
			PictureXL  string `json:"picture_xl"`
		} `json:"data"`
	}
	if err := json.Unmarshal(body, &resp); err != nil {
		return "", fmt.Errorf("parse: %w", err)
	}
	if len(resp.Data) == 0 {
		return "", ErrNotFound
	}
	if resp.Data[0].PictureBig != "" {
		return resp.Data[0].PictureBig, nil
	}
	if resp.Data[0].PictureXL != "" {
		return resp.Data[0].PictureXL, nil
	}
	/* The API sometimes returns a "missing artist" silhouette (1000×1000
	 * with a generic icon) rather than nothing. We don't filter for it
	 * here — the generic image is technically a valid response and
	 * filtering by URL/hash would be brittle. If a song's row ends up
	 * showing the silhouette, the fix is upstream (in Deezer's data). */
	return "", ErrNotFound
}
