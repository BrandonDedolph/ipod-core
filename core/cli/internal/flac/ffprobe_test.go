package flac

import (
	"bytes"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
)

// lookTool finds ffprobe/ffmpeg on PATH, falling back to /usr/sbin (where this
// host keeps them). Returns "" when the tool is absent, so callers can skip.
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

// probeDuration returns ffprobe's format.duration for path, in seconds.
func probeDuration(ffprobe, path string) (float64, error) {
	out, err := exec.Command(ffprobe, "-v", "error",
		"-show_entries", "format=duration", "-of", "csv=p=0", path).Output()
	if err != nil {
		var ee *exec.ExitError
		if ok := asExitError(err, &ee); ok {
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

func asExitError(err error, dst **exec.ExitError) bool {
	if ee, ok := err.(*exec.ExitError); ok {
		*dst = ee
		return true
	}
	return false
}

// TestBuildFileIsRealFLAC is the load-bearing check on the synthetic-file
// helper: ffprobe must report exactly the STREAMINFO duration and ffmpeg must
// decode every frame without an error, otherwise the fixtures S2/S3 build on
// this helper would be testing against a file real tools reject.
func TestBuildFileIsRealFLAC(t *testing.T) {
	ffprobe := lookTool("ffprobe")
	ffmpeg := lookTool("ffmpeg")
	if ffprobe == "" || ffmpeg == "" {
		t.Skip("ffprobe/ffmpeg not installed")
	}

	cases := []StreamInfo{
		{44100, 2, 16, 44100 * 3},        // exact seconds
		{44100, 2, 16, 44100*3 + 1234},   // partial final frame
		{44100, 1, 16, 4096},             // exactly one full block
		{44100, 1, 16, 100},              // shorter than one block
		{44100, 2, 24, 44100*61 + 44099}, // truncation boundary
		{48000, 1, 16, 48000 * 5},        //
		{48000, 2, 24, 48000*7 + 1},      //
		{96000, 2, 24, 96000 * 2},        //
		{96000, 1, 16, 96000*11 + 95999}, //
		{22050, 2, 16, 22050 * 4},        //
	}
	dir := t.TempDir()
	for i, info := range cases {
		name := fmt.Sprintf("%dHz_%dch_%dbit_%dsamples", info.SampleRate, info.Channels, info.BitsPerSample, info.TotalSamples)
		t.Run(name, func(t *testing.T) {
			path := filepath.Join(dir, fmt.Sprintf("%02d.flac", i))
			data := BuildFile(info, map[string]string{"TITLE": name, "ARTIST": "Test"}, nil)
			if err := os.WriteFile(path, data, 0o644); err != nil {
				t.Fatal(err)
			}

			got, err := probeDuration(ffprobe, path)
			if err != nil {
				t.Fatalf("%v", err)
			}
			want := float64(info.TotalSamples) / float64(info.SampleRate)
			if diff := got - want; diff > 1e-6 || diff < -1e-6 {
				t.Errorf("ffprobe duration %v, want %v", got, want)
			}
			m := mustRead(t, data)
			if uint32(got) != m.DurationSeconds() {
				t.Errorf("int(ffprobe duration) = %d, DurationSeconds() = %d", uint32(got), m.DurationSeconds())
			}

			// Full decode: catches a bad CRC, a mis-sized frame header or a
			// sample count that does not add up.
			cmd := exec.Command(ffmpeg, "-v", "error", "-i", path, "-f", "null", "-")
			var stderr bytes.Buffer
			cmd.Stderr = &stderr
			if err := cmd.Run(); err != nil {
				t.Fatalf("ffmpeg decode failed: %v\n%s", err, stderr.String())
			}
			if stderr.Len() != 0 {
				t.Errorf("ffmpeg wrote to stderr at -v error:\n%s", stderr.String())
			}
		})
	}
}

// TestBuildFileTagsAndPictureSurviveFFprobe confirms the metadata blocks are
// well-formed to a third party, not just to this package's own parser.
func TestBuildFileTagsAndPictureSurviveFFprobe(t *testing.T) {
	ffprobe := lookTool("ffprobe")
	if ffprobe == "" {
		t.Skip("ffprobe not installed")
	}
	path := filepath.Join(t.TempDir(), "tagged.flac")
	// A 1x1 PNG, so ffprobe sees a decodable attached picture.
	png := []byte{
		0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a,
		0, 0, 0, 0x0d, 'I', 'H', 'D', 'R', 0, 0, 0, 1, 0, 0, 0, 1, 8, 2, 0, 0, 0, 0x90, 0x77, 0x53, 0xde,
		0, 0, 0, 0x0c, 'I', 'D', 'A', 'T', 0x08, 0xd7, 0x63, 0xf8, 0xcf, 0xc0, 0x00, 0x00, 0x03, 0x01, 0x01, 0x00,
		0x18, 0xdd, 0x8d, 0xb0,
		0, 0, 0, 0, 'I', 'E', 'N', 'D', 0xae, 0x42, 0x60, 0x82,
	}
	data := BuildFile(
		StreamInfo{44100, 2, 16, 44100},
		map[string]string{"TITLE": "Le Café", "ARTIST": "Band", "TRACKNUMBER": "7"},
		[]Picture{{Type: PictureTypeFrontCover, MIME: "image/png", Data: png}},
	)
	if err := os.WriteFile(path, data, 0o644); err != nil {
		t.Fatal(err)
	}
	out, err := exec.Command(ffprobe, "-v", "error", "-show_entries",
		"format_tags=title,artist,track", "-of", "default=nw=1", path).CombinedOutput()
	if err != nil {
		t.Fatalf("ffprobe: %v\n%s", err, out)
	}
	// ffprobe echoes the field name with the case it found in the file, so
	// compare case-insensitively.
	got := strings.ToLower(string(out))
	for _, want := range []string{"tag:title=le café", "tag:artist=band", "tag:track=7"} {
		if !strings.Contains(got, want) {
			t.Errorf("ffprobe output missing %q:\n%s", want, out)
		}
	}
	// The picture shows up as a second (video) stream.
	out, err = exec.Command(ffprobe, "-v", "error", "-select_streams", "v",
		"-show_entries", "stream=codec_name", "-of", "csv=p=0", path).CombinedOutput()
	if err != nil {
		t.Fatalf("ffprobe: %v\n%s", err, out)
	}
	if !strings.Contains(string(out), "png") {
		t.Errorf("attached picture not seen by ffprobe: %q", out)
	}
}
