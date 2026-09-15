package flac

import (
	"io/fs"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
	"sync"
	"testing"
)

// TestParityDurationsAgainstFFprobe walks the real library named by
// CORE_PARITY_SRC and requires this parser to agree with ffprobe on every
// single file: every FLAC must parse, and DurationSeconds() must equal
// int(ffprobe's format.duration).
//
// ffprobe computes that duration for FLAC as total_samples / sample_rate out of
// STREAMINFO — the same two numbers we read — so agreement should be exact and
// any mismatch is a parser bug, not a rounding question. The one case where
// ffprobe does something else is total_samples == 0 (unknown length), where it
// estimates from the bitrate; such files are reported by name and excluded from
// the comparison rather than papered over with a tolerance.
func TestParityDurationsAgainstFFprobe(t *testing.T) {
	src := os.Getenv("CORE_PARITY_SRC")
	if src == "" {
		t.Skip("CORE_PARITY_SRC not set")
	}
	if st, err := os.Stat(src); err != nil || !st.IsDir() {
		t.Skipf("CORE_PARITY_SRC=%q is not a directory", src)
	}
	ffprobe := lookTool("ffprobe")
	if ffprobe == "" {
		t.Skip("ffprobe not installed")
	}

	var files []string
	err := filepath.WalkDir(src, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if !d.IsDir() && strings.EqualFold(filepath.Ext(path), ".flac") {
			files = append(files, path)
		}
		return nil
	})
	if err != nil {
		t.Fatalf("walking %s: %v", src, err)
	}
	if len(files) == 0 {
		t.Skipf("no FLAC files under %s", src)
	}
	sort.Strings(files)
	t.Logf("%d FLAC files under %s", len(files), src)

	type result struct {
		path     string
		parseErr error
		probeErr error
		ours     uint32
		theirs   float64
		unknown  bool // STREAMINFO total_samples == 0
	}
	results := make([]result, len(files))

	workers := runtime.NumCPU()
	if workers > 8 {
		workers = 8
	}
	var wg sync.WaitGroup
	jobs := make(chan int)
	for w := 0; w < workers; w++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for i := range jobs {
				r := result{path: files[i]}
				m, err := ReadFile(files[i])
				if err != nil {
					r.parseErr = err
				} else {
					r.ours = m.DurationSeconds()
					r.unknown = m.Info.TotalSamples == 0 || m.Info.SampleRate == 0
					d, perr := probeDuration(ffprobe, files[i])
					r.probeErr, r.theirs = perr, d
				}
				results[i] = r
			}
		}()
	}
	for i := range files {
		jobs <- i
	}
	close(jobs)
	wg.Wait()

	var parseFails, probeFails, mismatches, unknownLen int
	for _, r := range results {
		rel, _ := filepath.Rel(src, r.path)
		switch {
		case r.parseErr != nil:
			parseFails++
			t.Errorf("parse failed: %s: %v", rel, r.parseErr)
		case r.unknown:
			unknownLen++
			t.Errorf("STREAMINFO total_samples/sample_rate is 0, so ffprobe estimates from the bitrate: %s (ffprobe says %v)", rel, r.theirs)
		case r.probeErr != nil:
			probeFails++
			t.Errorf("ffprobe failed: %s: %v", rel, r.probeErr)
		case uint32(r.theirs) != r.ours:
			mismatches++
			t.Errorf("duration mismatch: %s: ours %d, ffprobe %v (int %d)", rel, r.ours, r.theirs, uint32(r.theirs))
		}
	}
	t.Logf("parity: %d files, %d parse failures, %d ffprobe failures, %d unknown-length, %d duration mismatches",
		len(files), parseFails, probeFails, unknownLen, mismatches)
}
