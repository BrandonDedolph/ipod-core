package artistart

import (
	"context"
	"os"
	"testing"
	"time"
)

// TestFetchSmoke runs the full chain against a real artist that's
// expected to be in MusicBrainz / Wikipedia / Commons. Skipped by
// default — opt in with CORE_ARTISTART_SMOKE=1 — because it hits the
// public internet, takes seconds, and is rate-limit-sensitive.
//
// Intent: catch silent-breakage regressions in the API hop chain
// (e.g. MB renaming a JSON field, Commons changing its imageinfo
// shape) without making CI flaky on every PR.
func TestFetchSmoke(t *testing.T) {
	if os.Getenv("CORE_ARTISTART_SMOKE") == "" {
		t.Skip("set CORE_ARTISTART_SMOKE=1 to run network-backed smoke test")
	}
	f := NewFetcher()
	defer f.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()

	bytes, srcURL, err := f.Fetch(ctx, "Aphex Twin")
	if err != nil {
		t.Fatalf("fetch: %v", err)
	}
	if len(bytes) < 1024 {
		t.Errorf("suspiciously small image: %d bytes from %s", len(bytes), srcURL)
	}
	if srcURL == "" {
		t.Errorf("empty source URL")
	}
	t.Logf("got %d bytes from %s", len(bytes), srcURL)
}
