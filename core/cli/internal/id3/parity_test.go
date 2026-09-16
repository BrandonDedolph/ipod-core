package id3

import (
	"bytes"
	"fmt"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"sync"
	"testing"
)

func lookTool(name string) string {
	if p, err := exec.LookPath(name); err == nil {
		return p
	}
	for _, dir := range []string{"/usr/sbin", "/usr/bin", "/usr/local/bin", "/opt/homebrew/bin"} {
		p := filepath.Join(dir, name)
		if st, err := os.Stat(p); err == nil && !st.IsDir() {
			return p
		}
	}
	return ""
}

func probeDuration(ffprobe, path string) (float64, error) {
	out, err := exec.Command(ffprobe, "-v", "error",
		"-show_entries", "format=duration", "-of", "csv=p=0", path).Output()
	if err != nil {
		if ee, ok := err.(*exec.ExitError); ok {
			return 0, fmt.Errorf("ffprobe: %v: %s", err, bytes.TrimSpace(ee.Stderr))
		}
		return 0, err
	}
	s := strings.TrimSpace(string(out))
	if s == "" || s == "N/A" {
		return 0, fmt.Errorf("ffprobe reported no duration (%q)", s)
	}
	return strconv.ParseFloat(s, 64)
}

// TestParityDurationsAgainstFFprobe walks the real library named by
// CORE_PARITY_SRC and requires this reader to agree with ffprobe on every MP3
// in it: every file must parse, and DurationSeconds() must equal
// int(ffprobe's format.duration).
//
// This is the gate on the byte-identical-index guarantee. `core sync` and
// tools/build_index.py must produce the same CORELIB.IDX for the same tree;
// build_index.py's duration IS int(ffprobe's), so any disagreement here is a
// difference in the stamped index, not a rounding preference.
//
// Both tools use the same rule for a tagged file: the Xing/VBRI frame count
// times the samples per frame, less the LAME encoder delay and end padding.
// Those are asserted exactly.
//
// A file with NO Xing/VBRI header has no stated length, so both sides
// estimate — and the two estimates are only the same answer when the file
// really is constant bitrate. This test therefore splits the untagged files
// by walking their frames:
//
//	untagged CBR  every frame the same bitrate: our estimate is exact and
//	              ffprobe's is too, so it is asserted like a tagged file;
//	untagged VBR  the estimates diverge by construction (we extrapolate the
//	              first frame's bitrate over the whole file, ffmpeg samples).
//	              SKIPPED, loudly, by name — not quietly tolerated. It is the
//	              one hole in the byte-identical-index guarantee, and it is
//	              written down in `core index`'s help, STATUS.md and
//	              core/docs/design/mp3-playback.md.
//
// LAME writes a Xing header unless you ask it not to (`-t`), so an untagged
// VBR file is rare in a real library; it is not impossible, which is why this
// says so rather than asserting something untrue.
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
		if !d.IsDir() && strings.EqualFold(filepath.Ext(path), ".mp3") {
			files = append(files, path)
		}
		return nil
	})
	if err != nil {
		t.Fatalf("walking %s: %v", src, err)
	}
	if len(files) == 0 {
		t.Skipf("no MP3 files under %s", src)
	}
	sort.Strings(files)
	t.Logf("%d MP3 files under %s", len(files), src)

	type result struct {
		path     string
		parseErr error
		probeErr error
		ours     uint32
		theirs   float64
		untagged bool
		variable bool // untagged AND the frame bitrates differ
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
					r.untagged = !m.Tagged
					if r.untagged {
						r.variable = !constantBitrate(files[i])
					}
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

	var parseFails, probeFails, mismatches, skipped, untaggedCBR int
	for _, r := range results {
		rel, _ := filepath.Rel(src, r.path)
		switch {
		case r.parseErr != nil:
			parseFails++
			t.Errorf("parse failed: %s: %v", rel, r.parseErr)
		case r.probeErr != nil:
			probeFails++
			t.Errorf("ffprobe failed: %s: %v", rel, r.probeErr)
		case r.untagged && r.variable:
			// The documented hole. Named, counted, and not asserted.
			skipped++
			t.Logf("SKIPPED (untagged VBR — no stated length, so both sides "+
				"estimate and the estimates differ): %s (ours %d s, ffprobe %v s)",
				rel, r.ours, r.theirs)
		case uint32(r.theirs) != r.ours:
			mismatches++
			t.Errorf("duration mismatch: %s: ours %d, ffprobe %v (int %d)", rel, r.ours, r.theirs, uint32(r.theirs))
		default:
			if r.untagged {
				untaggedCBR++
			}
		}
	}
	t.Logf("parity: %d files, %d parse failures, %d ffprobe failures, "+
		"%d asserted-untagged-CBR, %d skipped (untagged VBR), %d duration mismatches",
		len(files), parseFails, probeFails, untaggedCBR, skipped, mismatches)
	if len(files)-skipped == 0 {
		t.Errorf("every file was skipped; the comparison asserted nothing")
	}
}

// constantBitrate walks a file's frames and reports whether they all carry the
// same bitrate. It is what tells an untagged CBR file (where our estimate and
// ffprobe's agree exactly) from an untagged VBR one (where they cannot).
func constantBitrate(path string) bool {
	b, err := os.ReadFile(path)
	if err != nil {
		return false
	}
	pos := id3v2Len(b)
	end := len(b) - tailTagLen(b[pos:])
	rate := 0
	for n := 0; pos+4 <= end && n < 20000; n++ {
		h, ok := parseHeader(b[pos:])
		if !ok {
			pos++ // junk: resync
			continue
		}
		if rate == 0 {
			rate = h.bitrate
		} else if h.bitrate != rate {
			return false
		}
		pos += h.frameLen
	}
	return rate != 0
}

// TestParityVectors is the same comparison against the committed vectors, so
// the rule is checked on every run and not only when a real library is to
// hand. The vectors deliberately include an untagged MPEG-2 mono file, whose
// duration both sides estimate from the bitrate.
func TestParityVectors(t *testing.T) {
	ffprobe := lookTool("ffprobe")
	if ffprobe == "" {
		t.Skip("ffprobe not installed")
	}
	dir := filepath.Join("..", "..", "..", "tests", "codec-vectors")
	names, err := filepath.Glob(filepath.Join(dir, "*.mp3"))
	if err != nil || len(names) == 0 {
		t.Skipf("no vectors under %s", dir)
	}
	sort.Strings(names)
	for _, p := range names {
		m, err := ReadFile(p)
		if err != nil {
			t.Errorf("%s: %v", filepath.Base(p), err)
			continue
		}
		d, err := probeDuration(ffprobe, p)
		if err != nil {
			t.Errorf("%s: ffprobe: %v", filepath.Base(p), err)
			continue
		}
		if uint32(d) != m.DurationSeconds() {
			t.Errorf("%s: ours %d s, ffprobe %v (int %d)",
				filepath.Base(p), m.DurationSeconds(), d, uint32(d))
		}
	}
}
