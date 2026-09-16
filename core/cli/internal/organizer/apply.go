package organizer

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

// JournalVersion is the on-disk format of a journal file. Bump it only for a
// change Undo cannot read.
const JournalVersion = 1

// Op is one recorded rename. Size and MTime are the file's, taken BEFORE the
// move — rename preserves both, so they are also what Undo must find at To.
// Undo refuses an op whose target no longer matches them.
type Op struct {
	From    string `json:"from"`
	To      string `json:"to"`
	Reason  string `json:"reason,omitempty"`
	Size    int64  `json:"size"`
	MTimeNS int64  `json:"mtime_ns"`
	Done    bool   `json:"done"`
}

// Journal is the file Apply writes before it touches anything. It holds the
// WHOLE plan from the start: a crash halfway through leaves a journal that can
// still undo the half that happened, because each op carries what its file
// must look like for the reversal to be safe.
type Journal struct {
	Version int       `json:"version"`
	Root    string    `json:"root"`
	When    time.Time `json:"when"`
	Ops     []Op      `json:"ops"`
	// Created are directories Apply made; Undo removes them again if they
	// come out empty.
	Created []string `json:"created,omitempty"`
	// Removed are source directories Apply deleted because the last file
	// left them empty.
	Removed []string `json:"removed,omitempty"`

	Path string   `json:"-"`
	f    *os.File `json:"-"`
}

// DefaultJournalDir is <UserConfigDir>/core/journal — the same place the
// flasher keeps its backups, for the same reason: it is the user's own
// directory, it survives an uninstall, and a person can find it.
func DefaultJournalDir() (string, error) {
	dir, err := os.UserConfigDir()
	if err != nil {
		return "", fmt.Errorf("no user config directory to keep the undo journal in: %w", err)
	}
	return filepath.Join(dir, "core", "journal"), nil
}

func (o Options) journalDir() (string, error) {
	if o.JournalDir != "" {
		return o.JournalDir, nil
	}
	return DefaultJournalDir()
}

// Apply performs the plan's moves in order, after writing the journal.
//
// The order matters twice: the journal is fsynced BEFORE the first rename, and
// the first failure stops everything with the journal intact, so an interrupted
// run is always undoable. A target that unexpectedly exists is a failure, never
// an overwrite.
func Apply(ctx context.Context, p *Plan, o Options) (*Journal, error) {
	if p == nil {
		return nil, fmt.Errorf("organize: no plan")
	}
	root, err := filepath.Abs(p.Root)
	if err != nil {
		return nil, err
	}
	j := &Journal{Version: JournalVersion, Root: root, When: time.Now()}
	if len(p.Moves) == 0 {
		return j, nil
	}

	// Everything is checked before anything is written: no move leaves the
	// tree, every source is really there, and its size and mtime are recorded
	// so the undo can prove the file is untouched.
	type stamp struct{ size, mtime int64 }
	// A move whose source is an earlier move's target (the hop that breaks a
	// rename cycle) cannot be stat'ed yet; rename preserves size and mtime,
	// so the earlier op's numbers are that file's numbers.
	known := map[string]stamp{}
	for _, m := range p.Moves {
		if !under(root, m.From) || !under(root, m.To) {
			return nil, fmt.Errorf("organize: %s -> %s leaves the source tree %s", m.From, m.To, root)
		}
		if m.From == m.To {
			continue
		}
		st, ok := known[m.From]
		if !ok {
			fi, err := os.Lstat(m.From)
			if err != nil {
				return nil, fmt.Errorf("organize: %w", err)
			}
			st = stamp{fi.Size(), fi.ModTime().UnixNano()}
		}
		known[m.To] = st
		j.Ops = append(j.Ops, Op{
			From: m.From, To: m.To, Reason: m.Reason,
			Size: st.size, MTimeNS: st.mtime,
		})
	}

	dir, err := o.journalDir()
	if err != nil {
		return nil, err
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return nil, err
	}
	if err := j.create(dir); err != nil {
		return nil, err
	}
	defer j.close()
	p.Journal = j.Path

	for i := range j.Ops {
		if err := ctx.Err(); err != nil {
			return j, err
		}
		op := &j.Ops[i]
		o.emit(Event{Phase: "apply", Album: rel(root, filepath.Dir(op.To)), Done: i, Total: len(j.Ops)})
		made, err := mkdirAllTracked(filepath.Dir(op.To), root)
		j.Created = append(j.Created, made...)
		if err != nil {
			_ = j.write()
			return j, err
		}
		if err := safeRename(op.From, op.To); err != nil {
			_ = j.write()
			return j, fmt.Errorf("organize: %w (nothing after this was moved; undo with core organize --undo %s)", err, j.Path)
		}
		op.Done = true
		if err := j.write(); err != nil {
			return j, err
		}
	}
	o.emit(Event{Phase: "apply", Done: len(j.Ops), Total: len(j.Ops)})

	// A folder the last file just left is removed, so the tree does not fill
	// up with empty "Album (1)" shells. Only empty ones, only under Root.
	j.Removed = pruneEmptyDirs(root, j.Ops)
	if err := j.write(); err != nil {
		return j, err
	}
	return j, nil
}

// UndoReport is what an undo did and, more importantly, what it refused to do.
type UndoReport struct {
	Journal     string
	Undone      int
	Skipped     []Skip
	DirsRemoved []string
}

// Skip is one op Undo would not reverse.
type Skip struct {
	Op     Op
	Reason string
}

// Undo replays a journal backwards. Every file must still be where the journal
// put it, with the size and mtime it had; anything else is left alone and
// listed, because an undo that clobbers a newer file is worse than no undo.
func Undo(ctx context.Context, journalPath string, o Options) (*UndoReport, error) {
	j, err := ReadJournal(journalPath)
	if err != nil {
		return nil, err
	}
	rep := &UndoReport{Journal: journalPath}
	for i := len(j.Ops) - 1; i >= 0; i-- {
		if err := ctx.Err(); err != nil {
			return rep, err
		}
		op := j.Ops[i]
		o.emit(Event{Phase: "undo", Album: rel(j.Root, filepath.Dir(op.From)),
			Done: len(j.Ops) - i, Total: len(j.Ops)})
		fi, err := os.Lstat(op.To)
		if err != nil {
			rep.Skipped = append(rep.Skipped, Skip{op, "it is not there any more"})
			continue
		}
		if fi.Size() != op.Size || fi.ModTime().UnixNano() != op.MTimeNS {
			rep.Skipped = append(rep.Skipped, Skip{op, "it changed after the move; left alone"})
			continue
		}
		if ex, err := os.Lstat(op.From); err == nil && !sameTarget(op.To, op.From, fi, ex) {
			rep.Skipped = append(rep.Skipped, Skip{op, "something else is at the original name now"})
			continue
		}
		if _, err := mkdirAllTracked(filepath.Dir(op.From), j.Root); err != nil {
			return rep, err
		}
		if err := safeRename(op.To, op.From); err != nil {
			return rep, err
		}
		rep.Undone++
	}
	// Directories Apply created come out again, deepest first, if empty.
	dirs := append([]string(nil), j.Created...)
	sort.Sort(sort.Reverse(sort.StringSlice(dirs)))
	for _, d := range dirs {
		if d == j.Root || !under(j.Root, d) {
			continue
		}
		if err := os.Remove(d); err == nil {
			rep.DirsRemoved = append(rep.DirsRemoved, d)
		}
	}
	return rep, nil
}

// ReadJournal parses one journal file.
func ReadJournal(path string) (*Journal, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var j Journal
	if err := json.Unmarshal(b, &j); err != nil {
		return nil, fmt.Errorf("%s: not a journal file: %w", path, err)
	}
	if j.Version != JournalVersion {
		return nil, fmt.Errorf("%s: journal version %d, this build writes %d", path, j.Version, JournalVersion)
	}
	j.Path = path
	return &j, nil
}

// ListJournals returns the journals in dir, newest first.
func ListJournals(dir string) ([]*Journal, error) {
	ents, err := os.ReadDir(dir)
	if err != nil {
		if os.IsNotExist(err) {
			return nil, nil
		}
		return nil, err
	}
	var out []*Journal
	for _, e := range ents {
		if e.IsDir() || !strings.HasSuffix(e.Name(), ".json") {
			continue
		}
		j, err := ReadJournal(filepath.Join(dir, e.Name()))
		if err != nil {
			continue // a file we cannot read is not a journal; say nothing
		}
		out = append(out, j)
	}
	sort.SliceStable(out, func(a, b int) bool { return out[a].When.After(out[b].When) })
	return out, nil
}

// Completed is how many of the journal's ops are recorded as done. A journal
// from an interrupted run says fewer than it has ops.
func (j *Journal) Completed() int {
	n := 0
	for _, op := range j.Ops {
		if op.Done {
			n++
		}
	}
	return n
}

// create opens a new journal file and writes the whole plan into it. The file
// AND its directory are fsynced, so the record survives the power cut that the
// first rename might be interrupted by.
func (j *Journal) create(dir string) error {
	// Local time, because the person looking for this file knows when they
	// pressed the button, not what that was in UTC.
	stamp := j.When.Format("20060102-150405")
	for i := 0; ; i++ {
		name := stamp + ".json"
		if i > 0 {
			name = fmt.Sprintf("%s-%d.json", stamp, i+1)
		}
		p := filepath.Join(dir, name)
		f, err := os.OpenFile(p, os.O_RDWR|os.O_CREATE|os.O_EXCL, 0o644)
		if os.IsExist(err) {
			if i > 99 {
				return fmt.Errorf("cannot find an unused journal name in %s", dir)
			}
			continue
		}
		if err != nil {
			return err
		}
		j.Path, j.f = p, f
		break
	}
	if err := j.write(); err != nil {
		return err
	}
	if d, err := os.Open(dir); err == nil {
		_ = d.Sync()
		_ = d.Close()
	}
	return nil
}

// write rewrites the journal and fsyncs it. It is called once per completed
// op: the file is small, and the alternative — a summary written at the end —
// is exactly the record that is missing when it is needed.
func (j *Journal) write() error {
	if j.f == nil {
		return nil
	}
	b, err := json.MarshalIndent(j, "", "  ")
	if err != nil {
		return err
	}
	b = append(b, '\n')
	if _, err := j.f.Seek(0, 0); err != nil {
		return err
	}
	if err := j.f.Truncate(0); err != nil {
		return err
	}
	if _, err := j.f.Write(b); err != nil {
		return err
	}
	return j.f.Sync()
}

func (j *Journal) close() {
	if j.f != nil {
		_ = j.f.Close()
		j.f = nil
	}
}

// safeRename is os.Rename with the two things os.Rename gets wrong here: it
// never replaces an existing file, and it handles the rename that only changes
// case on a filesystem where the two names are the same file (Windows), by
// going through a temporary name.
func safeRename(from, to string) error {
	fromInfo, err := os.Lstat(from)
	if err != nil {
		return err
	}
	toInfo, err := os.Lstat(to)
	if err == nil {
		if !sameTarget(from, to, fromInfo, toInfo) {
			return fmt.Errorf("%s already exists", to)
		}
		tmp := to + ".core-tmp"
		if _, err := os.Lstat(tmp); err == nil {
			return fmt.Errorf("%s is in the way of the two-step rename of %s", tmp, from)
		}
		if err := os.Rename(from, tmp); err != nil {
			return err
		}
		if err := os.Rename(tmp, to); err != nil {
			_ = os.Rename(tmp, from) // put it back under its old name
			return err
		}
		return nil
	}
	return os.Rename(from, to)
}

// sameTarget reports whether two paths name the SAME file: os.SameFile, plus
// the case-only rename on a filesystem where case does not distinguish names
// (Windows), which is the whole reason a rename can find "itself" in the way.
// The second test is deliberately narrow — only two spellings of one path — so
// that two genuinely different files can never be taken for one.
func sameTarget(from, to string, a, b os.FileInfo) bool {
	if os.SameFile(a, b) {
		return true
	}
	return caseFoldPaths && from != to &&
		strings.EqualFold(filepath.Clean(from), filepath.Clean(to))
}

// mkdirAllTracked is os.MkdirAll that reports which directories it actually
// created, so an undo can take them away again.
func mkdirAllTracked(dir, root string) ([]string, error) {
	if _, err := os.Stat(dir); err == nil {
		return nil, nil
	}
	var missing []string
	for d := dir; under(root, d) && d != root; d = filepath.Dir(d) {
		if _, err := os.Stat(d); err == nil {
			break
		}
		missing = append(missing, d)
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return nil, err
	}
	return missing, nil
}

// pruneEmptyDirs removes the source folders the moves emptied, deepest first,
// and only under root. A folder with anything left in it — a stray text file,
// a track that needs attention — is kept.
func pruneEmptyDirs(root string, ops []Op) []string {
	seen := map[string]bool{}
	var dirs []string
	for _, op := range ops {
		for d := filepath.Dir(op.From); under(root, d) && d != root; d = filepath.Dir(d) {
			if !seen[d] {
				seen[d] = true
				dirs = append(dirs, d)
			}
		}
	}
	sort.Sort(sort.Reverse(sort.StringSlice(dirs)))
	var out []string
	for _, d := range dirs {
		ents, err := os.ReadDir(d)
		if err != nil || len(ents) != 0 {
			continue
		}
		if err := os.Remove(d); err == nil {
			out = append(out, d)
		}
	}
	return out
}

// under reports whether p is root or inside it.
func under(root, p string) bool {
	r, err := filepath.Rel(root, p)
	if err != nil {
		return false
	}
	return r == "." || (!strings.HasPrefix(r, ".."+string(filepath.Separator)) && r != "..")
}
