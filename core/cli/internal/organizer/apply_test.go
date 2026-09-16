package organizer

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// snapshot is the whole tree as a map: every directory and every file, with
// the file's content hash and size. Two equal snapshots mean the tree came
// back exactly — which is the only useful definition of "undo".
func snapshot(t *testing.T, root string) map[string]string {
	t.Helper()
	out := map[string]string{}
	err := filepath.WalkDir(root, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		r := rel(root, p)
		if d.IsDir() {
			out[r+"/"] = "dir"
			return nil
		}
		b, err := os.ReadFile(p)
		if err != nil {
			return err
		}
		sum := sha256.Sum256(b)
		out[r] = hex.EncodeToString(sum[:8])
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	return out
}

func diff(t *testing.T, before, after map[string]string) {
	t.Helper()
	for k, v := range before {
		if a, ok := after[k]; !ok {
			t.Errorf("missing after undo: %s", k)
		} else if a != v {
			t.Errorf("changed after undo: %s (%s -> %s)", k, v, a)
		}
	}
	for k := range after {
		if _, ok := before[k]; !ok {
			t.Errorf("left behind by undo: %s", k)
		}
	}
}

// messyTree is a source tree with something wrong in every direction: junk
// names, a nested album, a flat two-disc album and sidecars.
func messyTree(t *testing.T) string {
	t.Helper()
	root := t.TempDir()
	junk := filepath.Join(root, "wallen dump")
	writeTrack(t, filepath.Join(junk, "b.flac"), tagsFor("If I Know Me", "Morgan Wallen", "Up Down", "2"))
	writeTrack(t, filepath.Join(junk, "a.flac"), tagsFor("If I Know Me", "Morgan Wallen", "Whiskey Glasses", "1"))
	if err := os.WriteFile(filepath.Join(junk, "folder.art"), []byte("art bytes"), 0o644); err != nil {
		t.Fatal(err)
	}
	nested := filepath.Join(root, "Downloads", "doa")
	writeTrack(t, filepath.Join(nested, "1.flac"), tagsFor("DOA", "ericdoa", "sad4whatever", "1"))
	flat := filepath.Join(root, "two disc")
	for i, tc := range []struct{ title, track, disc string }{
		{"Alpha", "1", "1"}, {"Gamma", "1", "2"},
	} {
		tg := tagsFor("Double", "Band", tc.title, tc.track)
		tg["discnumber"] = tc.disc
		writeTrack(t, filepath.Join(flat, string(rune('a'+i))+".flac"), tg)
	}
	return root
}

// TestApplyThenUndoRestoresTheTree. The journal is the promise this slice
// makes; this is the test that it is kept.
func TestApplyThenUndoRestoresTheTree(t *testing.T) {
	root := messyTree(t)
	jdir := t.TempDir()
	before := snapshot(t, root)

	p := scan(t, root)
	if len(p.Moves) != 6 {
		t.Fatalf("moves = %v, want 5 tracks + the folder art", moveMap(t, p))
	}
	j, err := Apply(context.Background(), p, Options{Root: root, JournalDir: jdir})
	if err != nil {
		t.Fatalf("Apply: %v", err)
	}
	if j.Path == "" {
		t.Fatal("Apply wrote no journal")
	}
	if p.Journal != j.Path {
		t.Errorf("plan journal %q != %q", p.Journal, j.Path)
	}
	// The emptied source folders are gone.
	for _, d := range []string{"wallen dump", "two disc", filepath.Join("Downloads", "doa"), "Downloads"} {
		if _, err := os.Stat(filepath.Join(root, d)); err == nil {
			t.Errorf("%s was emptied but not removed", d)
		}
	}

	rep, err := Undo(context.Background(), j.Path, Options{Root: root, JournalDir: jdir})
	if err != nil {
		t.Fatalf("Undo: %v", err)
	}
	if rep.Undone != len(j.Ops) || len(rep.Skipped) != 0 {
		t.Errorf("undo did %d of %d, skipped %v", rep.Undone, len(j.Ops), rep.Skipped)
	}
	diff(t, before, snapshot(t, root))
}

// TestUndoRefusesAFileThatChanged: an undo that clobbers a newer file is worse
// than no undo, so the ones that changed are left alone and named.
func TestUndoRefusesAFileThatChanged(t *testing.T) {
	root := messyTree(t)
	jdir := t.TempDir()
	p := scan(t, root)
	j, err := Apply(context.Background(), p, Options{Root: root, JournalDir: jdir})
	if err != nil {
		t.Fatalf("Apply: %v", err)
	}
	victim := j.Ops[0].To
	if err := os.WriteFile(victim, []byte("the user re-tagged this after organizing"), 0o644); err != nil {
		t.Fatal(err)
	}

	rep, err := Undo(context.Background(), j.Path, Options{Root: root, JournalDir: jdir})
	if err != nil {
		t.Fatalf("Undo: %v", err)
	}
	if len(rep.Skipped) != 1 || rep.Skipped[0].Op.To != victim {
		t.Fatalf("skipped = %v, want just the changed file", rep.Skipped)
	}
	if !strings.Contains(rep.Skipped[0].Reason, "changed") {
		t.Errorf("reason = %q", rep.Skipped[0].Reason)
	}
	if b, err := os.ReadFile(victim); err != nil || !strings.HasPrefix(string(b), "the user") {
		t.Errorf("the changed file was moved anyway (%v)", err)
	}
	if _, err := os.Stat(j.Ops[0].From); err == nil {
		t.Error("the old name came back under a file that had changed")
	}
	if rep.Undone != len(j.Ops)-1 {
		t.Errorf("undone = %d, want the other %d", rep.Undone, len(j.Ops)-1)
	}
}

// TestUndoAlsoRefusesAFileTouchedInPlace: same size, different mtime. Size
// alone would let a re-tagged file through.
func TestUndoAlsoRefusesAFileTouchedInPlace(t *testing.T) {
	root := messyTree(t)
	jdir := t.TempDir()
	p := scan(t, root)
	j, err := Apply(context.Background(), p, Options{Root: root, JournalDir: jdir})
	if err != nil {
		t.Fatalf("Apply: %v", err)
	}
	later := time.Now().Add(2 * time.Hour)
	if err := os.Chtimes(j.Ops[0].To, later, later); err != nil {
		t.Fatal(err)
	}
	rep, err := Undo(context.Background(), j.Path, Options{Root: root, JournalDir: jdir})
	if err != nil {
		t.Fatalf("Undo: %v", err)
	}
	if len(rep.Skipped) != 1 {
		t.Fatalf("skipped = %v, want the touched file", rep.Skipped)
	}
}

// TestJournalIsWrittenBeforeTheFirstRename is the reviewer-checklist line. The
// second move is made to fail by putting a stranger on its target between the
// scan and the apply; what must survive is a journal that describes the WHOLE
// plan, so the part that did happen can still be undone.
func TestJournalIsWrittenBeforeTheFirstRename(t *testing.T) {
	// One folder, three junk names, so the blocked move needs no new
	// directory and the tree can be compared to the byte afterwards.
	root := t.TempDir()
	dir := filepath.Join(root, "Album - Artist")
	for i, title := range []string{"One", "Two", "Three"} {
		writeTrack(t, filepath.Join(dir, fmt.Sprintf("junk%d.flac", i+1)),
			tagsFor("Album", "Artist", title, fmt.Sprintf("%d", i+1)))
	}
	jdir := t.TempDir()
	before := snapshot(t, root)
	p := scan(t, root)
	if len(p.Moves) != 3 {
		t.Fatalf("moves = %v, want three", moveMap(t, p))
	}

	blocker := p.Moves[1].To
	if err := os.MkdirAll(filepath.Dir(blocker), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(blocker, []byte("in the way"), 0o644); err != nil {
		t.Fatal(err)
	}

	j, err := Apply(context.Background(), p, Options{Root: root, JournalDir: jdir})
	if err == nil {
		t.Fatal("Apply did not fail on the blocked target")
	}
	if !strings.Contains(err.Error(), "already exists") {
		t.Errorf("error = %v", err)
	}
	if j == nil || j.Path == "" {
		t.Fatal("no journal after the failure")
	}
	on, err := ReadJournal(j.Path)
	if err != nil {
		t.Fatalf("the journal on disk is unreadable: %v", err)
	}
	if len(on.Ops) != len(p.Moves) {
		t.Errorf("journal has %d ops, the plan had %d — it was not written in full up front",
			len(on.Ops), len(p.Moves))
	}
	if !on.Ops[0].Done || on.Ops[1].Done {
		t.Errorf("done flags = %v/%v, want the first only", on.Ops[0].Done, on.Ops[1].Done)
	}
	// Nothing after the failure was moved.
	for _, op := range on.Ops[2:] {
		if _, err := os.Stat(op.From); err != nil {
			t.Errorf("%s moved even though the run had already failed", op.From)
		}
	}
	// And the half that happened undoes cleanly: take the stranger away, undo,
	// and the tree is the one we started with, empty directories and all.
	if err := os.Remove(blocker); err != nil {
		t.Fatal(err)
	}
	if _, err := Undo(context.Background(), j.Path, Options{Root: root, JournalDir: jdir}); err != nil {
		t.Fatalf("Undo: %v", err)
	}
	diff(t, before, snapshot(t, root))
}

// TestApplyRefusesToLeaveTheRoot. Nothing outside Root is ever touched — the
// first line of the reviewer checklist.
func TestApplyRefusesToLeaveTheRoot(t *testing.T) {
	root := t.TempDir()
	outside := t.TempDir()
	writeTrack(t, filepath.Join(root, "a.flac"), tagsFor("A", "B", "C", "1"))
	jdir := t.TempDir()
	p := &Plan{Root: root, Moves: []Move{{
		From: filepath.Join(root, "a.flac"), To: filepath.Join(outside, "a.flac"), Reason: "move"}}}
	if _, err := Apply(context.Background(), p, Options{Root: root, JournalDir: jdir}); err == nil {
		t.Fatal("Apply followed a move out of the tree")
	}
	if _, err := os.Stat(filepath.Join(root, "a.flac")); err != nil {
		t.Error("the file moved anyway")
	}
	if ents, _ := os.ReadDir(jdir); len(ents) != 0 {
		t.Error("a journal was written for a plan that was refused")
	}
}

// TestListJournals: newest first, and each line says how far it got.
func TestListJournals(t *testing.T) {
	root := messyTree(t)
	jdir := t.TempDir()
	p := scan(t, root)
	j, err := Apply(context.Background(), p, Options{Root: root, JournalDir: jdir})
	if err != nil {
		t.Fatalf("Apply: %v", err)
	}
	js, err := ListJournals(jdir)
	if err != nil {
		t.Fatal(err)
	}
	if len(js) != 1 || js[0].Path != j.Path {
		t.Fatalf("ListJournals = %v", js)
	}
	if js[0].Completed() != len(j.Ops) || js[0].Root != root {
		t.Errorf("journal summary: %d/%d ops, root %s", js[0].Completed(), len(j.Ops), js[0].Root)
	}
	if _, err := ListJournals(filepath.Join(jdir, "nope")); err != nil {
		t.Errorf("a missing journal directory is not an error: %v", err)
	}
}

// TestApplyOnAnEmptyPlanWritesNothing: organizing a tree that is already right
// must not leave a journal file behind for the user to wonder about.
func TestApplyOnAnEmptyPlanWritesNothing(t *testing.T) {
	root := t.TempDir()
	dir := filepath.Join(root, "Album - Artist")
	writeTrack(t, filepath.Join(dir, "01 - Artist - Song.flac"), tagsFor("Album", "Artist", "Song", "1"))
	jdir := t.TempDir()
	p := scan(t, root)
	j, err := Apply(context.Background(), p, Options{Root: root, JournalDir: jdir})
	if err != nil {
		t.Fatal(err)
	}
	if j.Path != "" {
		t.Errorf("journal %q for an empty plan", j.Path)
	}
	if ents, _ := os.ReadDir(jdir); len(ents) != 0 {
		t.Errorf("the journal directory has %d file(s)", len(ents))
	}
}
