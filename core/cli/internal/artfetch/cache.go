package artfetch

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"
)

// DefaultNegativeTTL is how long "this album has no art anywhere" is
// believed. A week: long enough that re-inspecting a library every day
// costs nothing, short enough that an album somebody uploaded a cover
// for last month is found without the user having to know a cache
// exists.
const DefaultNegativeTTL = 7 * 24 * time.Hour

// Cache is the on-disk store under <UserCacheDir>/core/art.
//
// Two things live in it. Images are kept as <sha of provider+url>.jpg
// or .png, so a cover fetched during an inspect is not fetched again
// when the user accepts it. Misses are kept as <sha of the query>.miss,
// a one-field JSON file whose timestamp expires after NegativeTTL,
// because "nobody has a cover for this bootleg" is an expensive answer
// to compute and a cheap one to remember.
//
// A nil *Cache is usable: every method is a no-op that reports a miss.
// That is what a machine with no writable cache directory gets, and it
// is not an error — it is the same program doing more work.
type Cache struct {
	Dir         string
	NegativeTTL time.Duration
	// Now is the clock, injectable so the TTL test does not sleep.
	Now func() time.Time
}

// DefaultCache is the shared cache: <UserCacheDir>/core/art. It sits
// beside ghrelease's <UserCacheDir>/core/<tag>/ download cache on
// purpose — one directory a user can delete to reclaim everything this
// program ever downloaded.
func DefaultCache() (*Cache, error) {
	dir, err := os.UserCacheDir()
	if err != nil {
		return nil, fmt.Errorf("artfetch: locating the user cache directory: %w", err)
	}
	return &Cache{Dir: filepath.Join(dir, "core", "art")}, nil
}

func (c *Cache) now() time.Time {
	if c != nil && c.Now != nil {
		return c.Now()
	}
	return time.Now()
}

func (c *Cache) ttl() time.Duration {
	if c != nil && c.NegativeTTL > 0 {
		return c.NegativeTTL
	}
	return DefaultNegativeTTL
}

func (c *Cache) ok() bool { return c != nil && c.Dir != "" }

// Image returns a cached picture for provider+url.
func (c *Cache) Image(provider, url string) (data []byte, mime string, ok bool) {
	if !c.ok() {
		return nil, "", false
	}
	base := filepath.Join(c.Dir, key(provider, url))
	for _, e := range []struct{ ext, mime string }{
		{".jpg", "image/jpeg"},
		{".png", "image/png"},
	} {
		b, err := os.ReadFile(base + e.ext)
		if err == nil && len(b) > 0 {
			return b, e.mime, true
		}
	}
	return nil, "", false
}

// PutImage stores a fetched picture. The write is temp-then-rename, so
// a reader never sees a half-written cover: the cache is trusted
// unchecked on the next run, and a truncated file with the right name
// is exactly the input that trust must not be given to.
func (c *Cache) PutImage(provider, url string, data []byte, mime string) error {
	if !c.ok() || len(data) == 0 {
		return nil
	}
	ext := ".jpg"
	if strings.EqualFold(mime, "image/png") {
		ext = ".png"
	}
	return c.write(key(provider, url)+ext, data)
}

// Missed reports whether a fresh "no match" is on record for q.
func (c *Cache) Missed(q Query) bool {
	if !c.ok() {
		return false
	}
	b, err := os.ReadFile(c.missPath(q))
	if err != nil {
		return false
	}
	var m struct {
		When time.Time `json:"when"`
	}
	if err := json.Unmarshal(b, &m); err != nil || m.When.IsZero() {
		return false
	}
	return c.now().Sub(m.When) < c.ttl()
}

// PutMiss records that no provider had art for q.
func (c *Cache) PutMiss(q Query) error {
	if !c.ok() {
		return nil
	}
	b, err := json.Marshal(struct {
		When   time.Time `json:"when"`
		Artist string    `json:"artist"`
		Album  string    `json:"album"`
	}{c.now().UTC(), q.Artist, q.Album})
	if err != nil {
		return err
	}
	return c.write(filepath.Base(c.missPath(q)), b)
}

// Forget drops a remembered miss, so a user who has just fixed the tags
// is not told "no match (remembered)" for a week.
func (c *Cache) Forget(q Query) error {
	if !c.ok() {
		return nil
	}
	err := os.Remove(c.missPath(q))
	if os.IsNotExist(err) {
		return nil
	}
	return err
}

func (c *Cache) missPath(q Query) string {
	return filepath.Join(c.Dir, key("miss", NormKey(q.Artist)+"\x00"+NormKey(q.Album))+".miss")
}

func (c *Cache) write(name string, b []byte) error {
	if err := os.MkdirAll(c.Dir, 0o755); err != nil {
		return fmt.Errorf("artfetch: create the cache directory %s: %w", c.Dir, err)
	}
	f, err := os.CreateTemp(c.Dir, ".tmp-*")
	if err != nil {
		return err
	}
	tmp := f.Name()
	defer func() {
		f.Close()
		os.Remove(tmp)
	}()
	if _, err := f.Write(b); err != nil {
		return err
	}
	if err := f.Close(); err != nil {
		return err
	}
	return os.Rename(tmp, filepath.Join(c.Dir, name))
}
