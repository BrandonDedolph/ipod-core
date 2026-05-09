package artistart

import (
	"context"
	"crypto/sha1"
	"encoding/hex"
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
)

// DefaultCacheDir returns ~/.cache/core/artist-art (or platform
// equivalent), creating the directory tree if necessary. Returns "" +
// no error if the user has no determinable home dir, in which case the
// caller can fall back to a CWD-relative cache or skip caching.
func DefaultCacheDir() (string, error) {
	root, err := os.UserCacheDir()
	if err != nil {
		return "", nil
	}
	dir := filepath.Join(root, "core", "artist-art")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return "", fmt.Errorf("create %s: %w", dir, err)
	}
	return dir, nil
}

// CachedFetch reads cached bytes for `artistName` from `cacheDir`, or
// runs the full Fetch+Process chain and writes the result on a miss.
// The cache key is a SHA1 of the lowercase trimmed artist name —
// avoids filesystem-illegal characters and case-collisions ("Beach
// House" vs "beach house" share a slot, which matches the tagcache's
// case-insensitive dedup).
//
// On miss-and-not-found, writes a sentinel ".missing" file so future
// rebuilds skip the network round-trip for artists Wikipedia doesn't
// know about. Sentinel TTL is the user's responsibility — delete the
// cache dir to force a refresh.
//
// Returns:
//
//   - bytes, true, nil  on hit (real image, served from cache)
//   - bytes, false, nil on miss-then-fetch (real image, network fetched)
//   - nil, *, ErrNotFound on miss-or-cached-miss (no usable image)
//   - nil, *, err on any other failure
func CachedFetch(ctx context.Context, f *Fetcher, cacheDir, artistName string,
	maxDim, quality int) ([]byte, bool, error) {
	key := cacheKey(artistName)
	imgPath := filepath.Join(cacheDir, key+".jpg")
	missPath := filepath.Join(cacheDir, key+".missing")

	if b, err := os.ReadFile(imgPath); err == nil {
		return b, true, nil
	} else if !errors.Is(err, fs.ErrNotExist) {
		return nil, false, fmt.Errorf("read cache: %w", err)
	}
	if _, err := os.Stat(missPath); err == nil {
		return nil, true, ErrNotFound
	}

	raw, _, err := f.Fetch(ctx, artistName)
	if err != nil {
		if errors.Is(err, ErrNotFound) {
			/* Persist the negative result; rebuilds shouldn't pay
			 * the network cost for artists the chain can't resolve. */
			_ = os.WriteFile(missPath, nil, 0o644)
			return nil, false, ErrNotFound
		}
		return nil, false, err
	}
	processed, err := Process(raw, maxDim, quality)
	if err != nil {
		return nil, false, fmt.Errorf("process: %w", err)
	}
	if err := os.WriteFile(imgPath, processed, 0o644); err != nil {
		return nil, false, fmt.Errorf("write cache: %w", err)
	}
	return processed, false, nil
}

func cacheKey(s string) string {
	h := sha1.Sum([]byte(strings.ToLower(strings.TrimSpace(s))))
	return hex.EncodeToString(h[:])
}
