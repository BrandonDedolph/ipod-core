package cli

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flac"
)

// organizeFixture is one album whose names say nothing and whose tags say
// everything, plus one file that cannot be named from its tags. The tests in
// this file are about the COMMAND — flags, what lands on stdout, what is on
// disk afterwards; internal/organizer holds the rules.
func organizeFixture(t *testing.T) string {
	t.Helper()
	src := t.TempDir()
	dir := filepath.Join(src, "wallen dump")
	write := func(name string, tags map[string]string) {
		t.Helper()
		if err := os.MkdirAll(dir, 0o755); err != nil {
			t.Fatal(err)
		}
		raw := flac.BuildFile(
			flac.StreamInfo{SampleRate: 44100, Channels: 2, BitsPerSample: 16, TotalSamples: 4410},
			tags, nil)
		if err := os.WriteFile(filepath.Join(dir, name), raw, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	write("b.flac", map[string]string{
		"album": "If I Know Me", "artist": "Morgan Wallen", "title": "Up Down", "tracknumber": "2"})
	write("a.flac", map[string]string{
		"album": "If I Know Me", "artist": "Morgan Wallen", "title": "Whiskey Glasses", "tracknumber": "1"})
	write("mystery.flac", map[string]string{
		"album": "If I Know Me", "artist": "Morgan Wallen", "tracknumber": "3"})
	return src
}

// TestOrganizeDryRunChangesNothing: the default is a preview, and the preview
// ends with the sentence that stops a 2 GB re-copy from reading as a bug.
func TestOrganizeDryRunChangesNothing(t *testing.T) {
	src := organizeFixture(t)
	out, _, err := runCore(t, "organize", "--src", src)
	if err != nil {
		t.Fatalf("organize: %v", err)
	}
	for _, want := range []string{
		"wallen dump",
		"->",
		"If I Know Me - Morgan Wallen/01 - Morgan Wallen - Whiskey Glasses.flac",
		"Needs attention",
		"no title tag",
		"will be re-copied to the iPod on the next sync",
		"--apply",
	} {
		if !strings.Contains(out, want) {
			t.Errorf("the preview does not mention %q:\n%s", want, out)
		}
	}
	if _, err := os.Stat(filepath.Join(src, "wallen dump", "a.flac")); err != nil {
		t.Errorf("the dry run moved a file: %v", err)
	}
}

// TestOrganizeApplyThenUndo walks the whole command: apply, then undo by the
// journal the apply printed.
func TestOrganizeApplyThenUndo(t *testing.T) {
	src := organizeFixture(t)
	jdir := t.TempDir()

	out, _, err := runCore(t, "organize", "--src", src, "--apply", "--journal-dir", jdir)
	if err != nil {
		t.Fatalf("organize --apply: %v", err)
	}
	if !strings.Contains(out, "undo journal:") {
		t.Fatalf("no journal path in the output:\n%s", out)
	}
	moved := filepath.Join(src, "If I Know Me - Morgan Wallen", "01 - Morgan Wallen - Whiskey Glasses.flac")
	if _, err := os.Stat(moved); err != nil {
		t.Fatalf("after --apply: %v", err)
	}
	// The file that needs attention stayed where it was.
	if _, err := os.Stat(filepath.Join(src, "wallen dump", "mystery.flac")); err != nil {
		t.Errorf("the untouchable file was touched: %v", err)
	}

	list, _, err := runCore(t, "organize", "--list-journals", "--journal-dir", jdir)
	if err != nil {
		t.Fatalf("--list-journals: %v", err)
	}
	if !strings.Contains(list, "2 move(s)") || !strings.Contains(list, "--undo") {
		t.Errorf("journal listing:\n%s", list)
	}

	ents, err := os.ReadDir(jdir)
	if err != nil || len(ents) != 1 {
		t.Fatalf("journal directory has %v (%v)", ents, err)
	}
	journal := filepath.Join(jdir, ents[0].Name())

	undo, _, err := runCore(t, "organize", "--undo", journal)
	if err != nil {
		t.Fatalf("--undo: %v", err)
	}
	if !strings.Contains(undo, "put 2 file(s) back") {
		t.Errorf("undo output:\n%s", undo)
	}
	if _, err := os.Stat(filepath.Join(src, "wallen dump", "a.flac")); err != nil {
		t.Errorf("undo did not put a.flac back: %v", err)
	}
	if _, err := os.Stat(moved); err == nil {
		t.Error("the organized name is still there after the undo")
	}
}

// TestOrganizeOnACleanTreeSaysSo. The most common answer, and the one a user
// most needs to be able to trust.
func TestOrganizeOnACleanTreeSaysSo(t *testing.T) {
	src := t.TempDir()
	dir := filepath.Join(src, "If I Know Me - Morgan Wallen")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	raw := flac.BuildFile(
		flac.StreamInfo{SampleRate: 44100, Channels: 2, BitsPerSample: 16, TotalSamples: 4410},
		map[string]string{"album": "If I Know Me", "artist": "Morgan Wallen",
			"title": "Whiskey Glasses", "tracknumber": "1"}, nil)
	if err := os.WriteFile(filepath.Join(dir, "01 - Morgan Wallen - Whiskey Glasses.flac"), raw, 0o644); err != nil {
		t.Fatal(err)
	}
	out, _, err := runCore(t, "organize", "--src", src)
	if err != nil {
		t.Fatalf("organize: %v", err)
	}
	if !strings.Contains(out, "already named from its tags") {
		t.Errorf("output on a clean tree:\n%s", out)
	}
	if strings.Contains(out, "--apply") {
		t.Errorf("a clean tree should not be offered --apply:\n%s", out)
	}
}

// TestOrganizeNeedsASource: no --src and no CORELIB_SRC is a usage error, not
// a walk of the working directory.
func TestOrganizeNeedsASource(t *testing.T) {
	t.Setenv("CORELIB_SRC", "")
	if _, _, err := runCore(t, "organize"); err == nil {
		t.Fatal("organize with no source did not fail")
	}
}

// TestOrganizeJSONIsMachineReadable: the app reads this, not the table.
func TestOrganizeJSONIsMachineReadable(t *testing.T) {
	src := organizeFixture(t)
	out, _, err := runCore(t, "organize", "--src", src, "--json")
	if err != nil {
		t.Fatalf("organize --json: %v", err)
	}
	for _, want := range []string{`"Moves"`, `"Attention"`, `"TracksRecopied"`, `"Reason"`} {
		if !strings.Contains(out, want) {
			t.Errorf("--json output has no %s:\n%s", want, out)
		}
	}
}

// TestOrganizeMultiDiscIsOffByDefault: `core organize` leaves a flat
// multi-disc album flat and says the split is available, and --disc-folders
// makes it.
func TestOrganizeMultiDiscIsOffByDefault(t *testing.T) {
	src := t.TempDir()
	dir := filepath.Join(src, "Double - Band")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	for _, tc := range []struct{ name, title, track, disc string }{
		{"01 - Band - Alpha.flac", "Alpha", "1", "1"},
		{"02 - Band - Gamma.flac", "Gamma", "1", "2"},
	} {
		raw := flac.BuildFile(
			flac.StreamInfo{SampleRate: 44100, Channels: 2, BitsPerSample: 16, TotalSamples: 4410},
			map[string]string{"album": "Double", "artist": "Band", "title": tc.title,
				"tracknumber": tc.track, "discnumber": tc.disc}, nil)
		if err := os.WriteFile(filepath.Join(dir, tc.name), raw, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	out, _, err := runCore(t, "organize", "--src", src)
	if err != nil {
		t.Fatalf("organize: %v", err)
	}
	if !strings.Contains(out, "Multi-disc albums (2 files) — off by default") {
		t.Errorf("the preview does not offer the split:\n%s", out)
	}
	if !strings.Contains(out, "every file is already named from its tags") {
		t.Errorf("the flat album was not left alone:\n%s", out)
	}

	out, _, err = runCore(t, "organize", "--src", src, "--disc-folders")
	if err != nil {
		t.Fatalf("organize --disc-folders: %v", err)
	}
	for _, want := range []string{"Disc 1/01 - Band - Alpha.flac", "Disc 2/01 - Band - Gamma.flac", "(disc)"} {
		if !strings.Contains(out, want) {
			t.Errorf("--disc-folders does not say %q:\n%s", want, out)
		}
	}
}
