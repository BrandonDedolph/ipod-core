// Package organizer renames and moves the files of a source music tree into
// the ONE convention the device locator is derived from:
//
//	<Root>/Album - Artist/[Disc N/]NN - Artist - Title.flac
//
// That is not a new convention: it is the one the existing library already
// uses, and the reason it is load-bearing is internal/library. The device
// filename is fmt("%02d. %s.flac", pos, FatSafe(TrackTitle(base))) where pos is
// the file's position in the BYTE-SORTED listing of its folder, and the record
// that names it hashes that name. So:
//
//   - zero-padded NN in byte order is track order, so pos lines up with the
//     track number and the gutter on the device matches the filename;
//   - TrackTitle() strips "NN - Artist - " and returns the tag title verbatim,
//     so the device filename spells the title the tags do;
//   - the folder is "Album - Artist" because SplitAlbumArtist splits a source
//     folder on its LAST " - " (the device folder is "Artist - Album", built
//     from that split).
//
// Two rules fall out of TrackTitle splitting on " - ". A title containing
// " - " survives untouched (parts[2:] are re-joined with " - "), so it is left
// alone; an ARTIST containing " - " would eat the title, so in the FILENAME
// ONLY the artist's " - " becomes " – " (en dash). Tags are never rewritten:
// what the device DISPLAYS comes from the tags, and this package does not
// touch them.
//
// Nothing is guessed. A file whose tags do not carry title, artist, album and a
// usable track number is reported as needing attention and is not moved, and a
// target that already holds a different file is never overwritten.
//
// Every move is journaled before the first rename, so Undo can put the tree
// back; Undo verifies size and mtime and refuses anything that changed since.
package organizer

import (
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"regexp"
	"runtime"
	"sort"
	"strings"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flac"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/library"
)

// Options configure a scan and the apply that follows it. The zero value is
// usable once Root is set: metadata comes from flac.ReadFile and the journal
// goes to the user config dir.
type Options struct {
	// Root is the source tree. Nothing outside it is ever read or written.
	Root string
	// Meta reads one file's metadata. Defaults to flac.ReadFile; tests
	// substitute their own.
	Meta func(path string) (*flac.Meta, error)
	// JournalDir overrides <UserConfigDir>/core/journal.
	JournalDir string
	// Progress, when set, is called as albums are scanned and moves applied.
	Progress func(Event)
	// DiscFolders splits a FLAT album whose tags carry more than one disc
	// number into "Disc N" subfolders. It is OFF by default, and that is a
	// decision, not an oversight: the device takes disc and track from the
	// TAGS, so a flat multi-disc album already indexes and sorts correctly,
	// and splitting it renames every one of its files — 43 of them for the
	// one album in the real library — for nothing the user can see. When it
	// is off, the album stays flat, keeps the leading numbers it has (which
	// is what keeps its device names stable) and the split it would get is
	// reported in Plan.DiscSplit instead.
	//
	// An album that is ALREADY in "Disc N" folders keeps them either way:
	// this option is about splitting a flat one, not about flattening.
	DiscFolders bool
}

// Event is one step of a scan, an apply or an undo.
type Event struct {
	Phase       string // "scan" | "apply" | "undo"
	Album       string // the album folder being worked on
	Done, Total int
}

func (o Options) emit(e Event) {
	if o.Progress != nil {
		o.Progress(e)
	}
}

func (o Options) meta() func(string) (*flac.Meta, error) {
	if o.Meta != nil {
		return o.Meta
	}
	return flac.ReadFile
}

// Move is one rename. From and To are absolute and both are under Root.
type Move struct {
	From, To string
	// Reason is why: "rename" (same folder, new name), "move" (a new album
	// folder), "disc" (into a Disc N subfolder), "art" (a sidecar following
	// its album) or "cycle" (the temporary hop that breaks a rename cycle).
	Reason string
	Bytes  int64
}

// Attention is a file the organizer will not touch, and why. These are the
// only files a human has to look at; everything else is decided by the tags.
type Attention struct {
	Path, Reason string
}

// Plan is the whole proposed change. It is produced by Scan, which writes
// nothing, and consumed by Apply.
type Plan struct {
	Root          string
	Moves         []Move
	Attention     []Attention
	Warnings      []string
	AlbumsTouched int
	// TracksRecopied is how many tracks already on the iPod will be copied
	// again by the next sync because their device name (the locator) changed.
	TracksRecopied int
	// TracksNew is how many tracks the library scan cannot see today (they
	// sit in a folder with no " - ") and will be copied for the first time.
	TracksNew int
	// DiscSplit is the moves that would turn the flat multi-disc albums
	// into "Disc N" folders — what Options.DiscFolders would have produced.
	// It is NOT part of Moves and Apply never touches it: it is there so the
	// report can offer the split rather than silently decide it. Empty when
	// DiscFolders is on (the moves are in Moves then, carrying Reason
	// "disc").
	DiscSplit []Move
	// Journal is the path Apply wrote, filled in by Apply.
	Journal string
}

// Bytes is the total size of the files that will be moved.
func (p *Plan) Bytes() int64 {
	var n int64
	for _, m := range p.Moves {
		n += m.Bytes
	}
	return n
}

// Empty reports whether there is nothing to do.
func (p *Plan) Empty() bool { return len(p.Moves) == 0 }

// caseFoldPaths is whether two paths differing only in case name the same
// file. Windows and macOS say yes by default. Tests flip it to exercise the
// Windows behaviour on Linux.
var caseFoldPaths = runtime.GOOS == "windows" || runtime.GOOS == "darwin"

func foldKey(p string) string {
	p = filepath.Clean(p)
	if caseFoldPaths {
		return strings.ToLower(p)
	}
	return p
}

// enDash is the substitution that keeps TrackTitle's " - " split honest. It is
// applied to the ARTIST in a filename and to both halves of a folder name; it
// never reaches a tag.
func enDash(s string) string { return strings.ReplaceAll(s, " - ", " – ") }

// typoEqual reports whether an existing name already says what the canonical
// one says, up to the difference that is NOT worth a rename: a curly quote or
// an en/em dash against its ASCII shape.
//
// The rule comes straight off the locator. The device finds a file by
// NameHash(NormKey(name)), and NormKey folds exactly that typographic set, so
// two names equal under it resolve to the SAME file on the device — while the
// FAT name on disk would change, and a changed FAT name is a re-copy of the
// whole album for no visible gain. (The real library's "our little angel - EP
// - ROLE MODEL" against a tag album spelt with U+2013 is this case, nine files
// of it.)
//
// Case is the one part of NormKey that is deliberately NOT folded here: the
// device displays the folder's own spelling, so correcting it is worth the
// re-copy. library.FoldTypography is NormKey minus the lower-casing, which is
// precisely that rule, and it is reused rather than re-tabulated so the two
// cannot drift apart.
func typoEqual(existing, canonical string) bool {
	return library.FoldTypography(existing) == library.FoldTypography(canonical)
}

// CanonicalFolder is the album folder name: FatSafe("Album - Artist").
func CanonicalFolder(album, artist string) string {
	return library.FatSafe(enDash(album) + " - " + enDash(artist))
}

// CanonicalFile is the file name: "NN - Artist - Title.flac". The title keeps
// its " - " (TrackTitle re-joins everything after the second separator, so it
// round-trips); the artist cannot.
func CanonicalFile(artist, title string, track, width int) string {
	return canonicalFile(trackText(track, width), artist, title)
}

// canonicalFile is CanonicalFile with the number already rendered — a flat
// multi-disc album numbers its files "D-NN" rather than "NN".
func canonicalFile(number, artist, title string) string {
	return fmt.Sprintf("%s - %s - %s.flac", number,
		library.FatSafe(enDash(artist)), library.FatSafe(title))
}

func trackText(track, width int) string {
	if width < 2 {
		width = 2
	}
	return fmt.Sprintf("%0*d", width, track)
}

// CanonicalDisc is the disc subfolder name, or "" for a single-disc album.
func CanonicalDisc(disc int, multiDisc bool) string {
	if !multiDisc {
		return ""
	}
	if disc < 1 {
		disc = 1
	}
	return fmt.Sprintf("Disc %d", disc)
}

// Canonical is the naming rule for one file, from its tags alone: the album
// folder, the disc subfolder ("" when single-disc) and the file name. ok is
// false when a tag the name needs is missing — the caller must then send the
// file to Attention rather than guess.
//
// NOTE: the organizer does not call this directly. An album's artist is
// decided ONCE for the whole album (the majority albumartist, else the
// majority artist), because library gives every track in a folder the FOLDER's
// artist; per file, this function can only see the file's own tags. Use it for
// a single file (the librarian's preview of one row); use Scan for a tree.
func Canonical(m *flac.Meta, multiDisc bool, width int) (folder, disc, file string, ok bool) {
	if m == nil {
		return "", "", "", false
	}
	album := strings.TrimSpace(m.Tag("album"))
	artist := strings.TrimSpace(m.Tag("albumartist", "album_artist", "album artist", "artist"))
	title := strings.TrimSpace(m.Tag("title"))
	track := library.LeadInt(m.Tag("tracknumber", "track"))
	discNo := library.LeadInt(m.Tag("discnumber", "disc"))
	if album == "" || artist == "" || title == "" || track <= 0 {
		return "", "", "", false
	}
	return CanonicalFolder(album, artist), CanonicalDisc(discNo, multiDisc),
		CanonicalFile(artist, title, track, width), true
}

// ---------------------------------------------------------------- the scan

// node is one directory of the source tree as the walk found it.
type node struct {
	flacs    []string // "*.flac", byte-sorted — the enumeration library uses
	sidecars []string // folder.art / folder.thm / cover.* / folder.* pictures
	discDirs []string // immediate subdirectories named "Disc …"
}

// sidecar names that follow their album when the whole folder moves.
var sidecarNames = map[string]bool{
	"folder.art": true, "folder.thm": true,
	"cover.jpg": true, "cover.jpeg": true, "cover.png": true, "folder.jpg": true,
}

// isPlaylist is an album-local .m3u/.m3u8. The MC tree keeps one per album
// folder; it moves with the album the way the pictures do (review of L2).
func isPlaylist(base string) bool {
	ext := strings.ToLower(filepath.Ext(base))
	return ext == ".m3u" || ext == ".m3u8"
}

// discFolderRe reads the disc number out of a canonical "Disc N" folder. The
// enumeration in library keys off the "Disc " PREFIX, so that is what the walk
// matches; this one is only for reading the number back.
var discFolderRe = regexp.MustCompile(`^Disc[ \t]+(\d+)$`)

// track is one source FLAC and everything the plan needs from it.
type track struct {
	path     string // absolute
	dir      string // the directory it is in
	albumDir string // dir, or its parent when dir is a "Disc N" folder
	base     string
	size     int64

	album, artist, title string
	track, disc          int
	folderDisc           int

	target string // absolute canonical path, "" when it stays put
	moved  bool
}

// Scan reads the tree and returns the plan. It opens files for their metadata
// and never writes anything.
func Scan(o Options) (*Plan, error) {
	if o.Root == "" {
		return nil, fmt.Errorf("organize: no source folder (pass --src)")
	}
	root, err := filepath.Abs(o.Root)
	if err != nil {
		return nil, err
	}
	if st, err := os.Stat(root); err != nil || !st.IsDir() {
		return nil, fmt.Errorf("source tree not found: %s  (pass --src)", o.Root)
	}
	nodes, warns, err := walkTree(root)
	if err != nil {
		return nil, err
	}
	p := &Plan{Root: root, Warnings: warns}

	// One pass over every FLAC: read the tags, decide which album it belongs
	// to. Grouping is (source album folder, album tag): a folder is never
	// merged with another folder, and a folder holding two albums is split.
	read := o.meta()
	groups := map[string]*group{}
	var order []string
	dirs := sortedKeys(nodes)
	total := 0
	for _, d := range dirs {
		total += len(nodes[d].flacs)
	}
	done := 0
	for _, d := range dirs {
		n := nodes[d]
		albumDir, folderDisc := albumDirOf(root, d)
		for _, base := range n.flacs {
			path := filepath.Join(d, base)
			done++
			o.emit(Event{Phase: "scan", Album: rel(root, albumDir), Done: done, Total: total})
			t := &track{path: path, dir: d, albumDir: albumDir, base: base, folderDisc: folderDisc}
			if fi, err := os.Lstat(path); err == nil {
				t.size = fi.Size()
			}
			m, err := read(path)
			if err != nil {
				p.Attention = append(p.Attention, Attention{path, "cannot be read: " + err.Error()})
				continue
			}
			t.album = strings.TrimSpace(m.Tag("album"))
			t.artist = strings.TrimSpace(m.Tag("artist"))
			t.title = strings.TrimSpace(m.Tag("title"))
			albumArtist := strings.TrimSpace(m.Tag("albumartist", "album_artist", "album artist"))
			t.track = library.LeadInt(m.Tag("tracknumber", "track"))
			t.disc = library.LeadInt(m.Tag("discnumber", "disc"))
			switch {
			case t.album == "":
				p.Attention = append(p.Attention, Attention{path, "no album tag"})
				continue
			case t.artist == "" && albumArtist == "":
				p.Attention = append(p.Attention, Attention{path, "no artist tag"})
				continue
			case t.title == "":
				p.Attention = append(p.Attention, Attention{path, "no title tag"})
				continue
			case t.track <= 0:
				p.Attention = append(p.Attention, Attention{path,
					"no usable track number (the tag says " + quote(m.Tag("tracknumber", "track")) + ")"})
				continue
			}
			key := albumDir + "\x00" + library.NormKey(library.Straighten(t.album))
			g := groups[key]
			if g == nil {
				g = &group{albumDir: albumDir, album: t.album}
				groups[key] = g
				order = append(order, key)
			}
			g.tracks = append(g.tracks, t)
			if albumArtist != "" {
				g.albumArtists = append(g.albumArtists, albumArtist)
			}
			g.artists = append(g.artists, t.artist)
		}
	}
	sort.Strings(order)

	// Decide each album's name and each track's target.
	var moves []Move
	for _, key := range order {
		g := groups[key]
		artist, ok := g.decideArtist()
		if !ok {
			for _, t := range g.tracks {
				p.Attention = append(p.Attention, Attention{t.path,
					"the album's artist tag is split (" + strings.Join(g.distinctArtists(), ", ") + ")"})
			}
			continue
		}
		g.artist = artist
		if !o.DiscFolders && g.flatMultiDisc() {
			// What --disc-folders WOULD do with this album, for the report.
			// It is planned on a copy so the throwaway pass cannot say the
			// same thing to Attention twice.
			alt := g.clone()
			alt.plan(nil, root, true)
			p.DiscSplit = append(p.DiscSplit, alt.moves()...)
		}
		g.plan(p, root, o.DiscFolders)
	}

	// Collisions, then an order no rename can clobber.
	for _, key := range order {
		moves = append(moves, groups[key].moves()...)
	}
	moves = append(moves, sidecarMoves(root, nodes, groups, order, p)...)
	moves, att := resolveCollisions(root, moves)
	p.Attention = append(p.Attention, att...)
	p.Moves = orderMoves(moves)
	p.AlbumsTouched = countAlbums(p.Moves)
	p.TracksRecopied, p.TracksNew = recopyCost(root, nodes, p.Moves)
	sort.Slice(p.Attention, func(i, j int) bool { return p.Attention[i].Path < p.Attention[j].Path })
	return p, nil
}

// group is one album: the tracks of one album tag inside one source folder.
type group struct {
	albumDir     string
	album        string
	artist       string
	folder       string // the album folder this group's tracks end up in
	albumArtists []string
	artists      []string
	tracks       []*track
	// splitting is set when this album's moves exist only because a flat
	// multi-disc album is being split into "Disc N" folders. Every one of
	// them is then reported as Reason "disc", the folder rename included:
	// half a split is not a layout anybody asked for.
	splitting bool
}

// clone is a copy that can be planned without disturbing the original.
func (g *group) clone() *group {
	c := *g
	c.tracks = make([]*track, len(g.tracks))
	for i, t := range g.tracks {
		cp := *t
		c.tracks[i] = &cp
	}
	return &c
}

// moves is the group's planned renames.
func (g *group) moves() []Move {
	var out []Move
	for _, t := range g.tracks {
		if t.target == "" || !t.moved {
			continue
		}
		out = append(out, Move{From: t.path, To: t.target, Reason: g.moveReason(t), Bytes: t.size})
	}
	return out
}

// discOf is the disc a track is on: the "Disc N" folder it sits in is ground
// truth, then the tag, then 1.
func (g *group) discOf(t *track) int {
	switch {
	case t.folderDisc > 0:
		return t.folderDisc
	case t.disc > 0:
		return t.disc
	default:
		return 1
	}
}

// flatMultiDisc reports whether this album's tags span more than one disc
// while its files all sit in one folder — the album Options.DiscFolders is
// about.
func (g *group) flatMultiDisc() bool {
	multi, first := false, 0
	for i, t := range g.tracks {
		if t.folderDisc > 0 {
			return false
		}
		d := g.discOf(t)
		if i == 0 {
			first = d
		} else if d != first {
			multi = true
		}
	}
	return multi
}

// decideArtist is the album's artist: the majority albumartist, else the
// majority artist. A tie is a split vote, and a split vote is a question for a
// human — library gives every track in the folder ONE artist, so guessing here
// would put the wrong name on every track of the album.
func (g *group) decideArtist() (string, bool) {
	pick := func(vals []string) (string, bool) {
		if len(vals) == 0 {
			return "", false
		}
		count := map[string]int{}
		first := map[string]string{}
		for _, v := range vals {
			k := library.NormKey(library.Straighten(v))
			count[k]++
			if _, ok := first[k]; !ok {
				first[k] = v
			}
		}
		best, bestN, ties := "", 0, 0
		for k, n := range count {
			switch {
			case n > bestN:
				best, bestN, ties = k, n, 1
			case n == bestN:
				ties++
			}
		}
		if ties > 1 {
			return "", false
		}
		return first[best], true
	}
	if a, ok := pick(g.albumArtists); ok {
		return a, true
	} else if len(g.albumArtists) > 0 {
		return "", false
	}
	return pick(g.artists)
}

func (g *group) distinctArtists() []string {
	vals := g.albumArtists
	if len(vals) == 0 {
		vals = g.artists
	}
	seen := map[string]bool{}
	var out []string
	for _, v := range vals {
		if k := library.NormKey(v); !seen[k] {
			seen[k] = true
			out = append(out, v)
		}
	}
	sort.Strings(out)
	return out
}

// plan fills in every track's target. Duplicated track numbers are the one
// per-album hazard: two files claiming (disc 1, track 5) are both reported and
// neither moves, because the pair would land on two names whose byte order no
// longer matches the numbering.
//
// discFolders is Options.DiscFolders: with it off, a flat multi-disc album
// stays flat. p may be nil, which plans without reporting anything — that is
// the throwaway pass that works out what the split WOULD be.
func (g *group) plan(p *Plan, root string, discFolders bool) {
	discOf := g.discOf
	multi := false
	hasDiscDirs := false
	width := 2
	first := 0
	for i, t := range g.tracks {
		d := discOf(t)
		if i == 0 {
			first = d
		} else if d != first {
			multi = true
		}
		if t.folderDisc > 0 {
			hasDiscDirs = true
		}
		if t.track >= 100 {
			width = 3
		}
	}
	// An album already in "Disc N" folders keeps them; a FLAT one is only
	// split when it was asked for.
	useDisc := multi && (discFolders || hasDiscDirs)
	g.splitting = multi && !hasDiscDirs && discFolders
	flatMulti := multi && !useDisc
	dup := map[[2]int][]*track{}
	for _, t := range g.tracks {
		k := [2]int{discOf(t), t.track}
		dup[k] = append(dup[k], t)
	}
	folder := CanonicalFolder(g.album, g.artist)
	// A folder that already says this, give or take a curly quote or a dash,
	// stays as it is: renaming it moves every track on the iPod for nothing.
	if filepath.Dir(g.albumDir) == root && typoEqual(filepath.Base(g.albumDir), folder) {
		folder = filepath.Base(g.albumDir)
	}
	g.folder = folder
	for _, t := range g.tracks {
		k := [2]int{discOf(t), t.track}
		if len(dup[k]) > 1 {
			if p != nil {
				others := make([]string, 0, len(dup[k])-1)
				for _, o := range dup[k] {
					if o != t {
						others = append(others, filepath.Base(o.path))
					}
				}
				sort.Strings(others)
				p.Attention = append(p.Attention, Attention{t.path,
					fmt.Sprintf("track %d is claimed by this file and by %s", t.track, strings.Join(others, ", "))})
			}
			continue
		}
		dir := filepath.Join(root, folder, CanonicalDisc(discOf(t), useDisc))
		name := canonicalFile(g.number(t, discOf(t), width, flatMulti), g.artist, t.title)
		if t.dir == dir && typoEqual(t.base, name) {
			name = t.base
		}
		t.target = filepath.Join(dir, name)
		t.moved = t.target != t.path
	}
}

// number is the "NN" a file's canonical name starts with.
//
// For a flat multi-disc album (the split is off) it is NOT the tag's track
// number: disc 1 track 5 and disc 2 track 5 would both be "05", and the device
// name is the file's POSITION in the byte-sorted folder, so renumbering the
// album reshuffles every one of those positions and re-copies the lot. The
// number the file already carries is the only one that keeps them, so that is
// what is kept — library.LeadTrack reads it back, "D-NN" and all — and the
// disc and track the device SHOWS come from the tags either way. A flat file
// with no leading number at all gets the "D-NN" form library documents for a
// flattened multi-disc album, which sorts into playing order.
func (g *group) number(t *track, disc, width int, flatMulti bool) string {
	if !flatMulti {
		return trackText(t.track, width)
	}
	stem := strings.TrimSuffix(t.base, filepath.Ext(t.base))
	if d, n := library.LeadTrack(stem, g.artist); n > 0 {
		if d > 0 {
			return fmt.Sprintf("%d-%s", d, trackText(n, width))
		}
		return trackText(n, width)
	}
	return fmt.Sprintf("%d-%s", disc, trackText(t.track, width))
}

func (g *group) moveReason(t *track) string {
	if g.splitting {
		return "disc"
	}
	switch {
	case filepath.Dir(t.target) == t.dir:
		return "rename"
	case strings.HasPrefix(filepath.Base(filepath.Dir(t.target)), "Disc ") &&
		filepath.Dir(filepath.Dir(t.target)) == t.albumDir:
		return "disc"
	default:
		return "move"
	}
}

// sidecarMoves takes folder.art, folder.thm and the cover pictures along when
// a whole source folder is going somewhere else. They only move when every
// FLAC under that folder moves to ONE new album folder: a folder that is only
// half organized keeps its art, because it still has tracks in it.
func sidecarMoves(root string, nodes map[string]*node, groups map[string]*group, order []string, p *Plan) []Move {
	byDir := map[string][]*group{}
	for _, key := range order {
		g := groups[key]
		byDir[g.albumDir] = append(byDir[g.albumDir], g)
	}
	var out []Move
	for _, dir := range sortedKeys(byDir) {
		gs := byDir[dir]
		if dir == root || len(gs) != 1 {
			continue
		}
		g := gs[0]
		n := nodes[dir]
		if n == nil {
			continue
		}
		// The art follows only when the folder is emptied of tracks: a file
		// that stayed behind — one that needs attention, one of another
		// album — means this folder is still an album, and an album keeps
		// its cover.
		left := len(n.flacs)
		for _, sub := range n.discDirs {
			if dn := nodes[filepath.Join(dir, sub)]; dn != nil {
				left += len(dn.flacs)
			}
		}
		moving := 0
		for _, t := range g.tracks {
			if t.target != "" {
				moving++
			}
		}
		if g.folder == "" || moving == 0 || moving != left {
			continue
		}
		dest := filepath.Join(root, g.folder)
		if foldKey(dest) == foldKey(dir) {
			continue
		}
		for _, s := range n.sidecars {
			from := filepath.Join(dir, s)
			fi, err := os.Lstat(from)
			if err != nil {
				continue
			}
			reason := "art"
			if isPlaylist(s) {
				reason = "playlist"
			}
			if g.splitting {
				reason = "disc"
			}
			out = append(out, Move{From: from, To: filepath.Join(dest, s), Reason: reason, Bytes: fi.Size()})
		}
	}
	return out
}

// resolveCollisions drops any move that would overwrite something: two moves
// onto one target, or a target that already exists and is not itself moving
// out of the way. Dropping a move can take a name out of the "moves away"
// set, which can turn another move into a collision, so it runs to a fixpoint.
func resolveCollisions(root string, moves []Move) ([]Move, []Attention) {
	var att []Attention
	for {
		// Only a move that SURVIVES frees its source name. A file that needs
		// attention is staying where it is, whatever else wants its name.
		vacated := make(map[string]bool, len(moves))
		byTarget := map[string][]int{}
		for i, m := range moves {
			vacated[foldKey(m.From)] = true
			byTarget[foldKey(m.To)] = append(byTarget[foldKey(m.To)], i)
		}
		drop := map[int]bool{}
		for _, key := range sortedKeys(byTarget) {
			idx := byTarget[key]
			if len(idx) > 1 {
				for _, i := range idx {
					drop[i] = true
					var others []string
					for _, j := range idx {
						if j != i {
							others = append(others, rel(root, moves[j].From))
						}
					}
					sort.Strings(others)
					att = append(att, Attention{moves[i].From,
						fmt.Sprintf("would land on %s, and so would %s", rel(root, moves[i].To), strings.Join(others, ", "))})
				}
				continue
			}
			i := idx[0]
			if _, err := os.Lstat(moves[i].To); err != nil {
				continue // nothing there
			}
			// The target exists. Fine if it is a file this plan moves away,
			// or the source itself under a case-insensitive filesystem.
			if vacated[foldKey(moves[i].To)] || foldKey(moves[i].To) == foldKey(moves[i].From) {
				continue
			}
			drop[i] = true
			att = append(att, Attention{moves[i].From,
				"cannot become " + rel(root, moves[i].To) + ": a different file is already there"})
		}
		if len(drop) == 0 {
			return moves, att
		}
		out := moves[:0:0]
		for i, m := range moves {
			if !drop[i] {
				out = append(out, m)
			}
		}
		moves = out
	}
}

// orderMoves puts the moves in an order where no rename lands on a file that
// has not moved out of the way yet, breaking a cycle (two files swapping
// names) with a hop through "<target>.core-tmp".
func orderMoves(moves []Move) []Move {
	pending := make(map[string]int, len(moves))
	for i, m := range moves {
		pending[foldKey(m.From)] = i
	}
	left := make([]bool, len(moves))
	for i := range left {
		left[i] = true
	}
	var out []Move
	remaining := len(moves)
	for remaining > 0 {
		progress := false
		for i, m := range moves {
			if !left[i] {
				continue
			}
			if j, blocked := pending[foldKey(m.To)]; blocked && j != i {
				continue
			}
			out = append(out, m)
			left[i] = false
			delete(pending, foldKey(m.From))
			remaining--
			progress = true
		}
		if progress {
			continue
		}
		// A cycle: send one member through a temporary name, which frees its
		// source for whoever wanted it.
		for i, m := range moves {
			if !left[i] {
				continue
			}
			tmp := m.To + ".core-tmp"
			out = append(out, Move{From: m.From, To: tmp, Reason: "cycle", Bytes: m.Bytes})
			moves[i] = Move{From: tmp, To: m.To, Reason: m.Reason, Bytes: m.Bytes}
			delete(pending, foldKey(m.From))
			pending[foldKey(tmp)] = i
			break
		}
	}
	return out
}

func countAlbums(moves []Move) int {
	seen := map[string]bool{}
	for _, m := range moves {
		seen[foldKey(filepath.Dir(m.From))] = true
	}
	return len(seen)
}

// ------------------------------------------------------- the re-copy cost

// recopyCost is the number of tracks the next sync will copy again because
// their locator changed, and the number it will copy for the first time
// because their folder was invisible to library.ScanTree before.
//
// The locator is (device folder, device name) and the device name carries the
// file's POSITION in its folder, so a file that does not move can still change
// name when a neighbour leaves. That is why this is computed from the whole
// listing on both sides rather than from the moves.
func recopyCost(root string, nodes map[string]*node, moves []Move) (recopied, added int) {
	before := map[string][]string{}
	for dir, n := range nodes {
		before[dir] = append([]string(nil), n.flacs...)
	}
	after := map[string][]string{}
	for dir, f := range before {
		after[dir] = append([]string(nil), f...)
	}
	for _, m := range moves {
		if !strings.HasSuffix(m.From, ".flac") || !strings.HasSuffix(m.To, ".flac") {
			continue
		}
		fd, fb := filepath.Dir(m.From), filepath.Base(m.From)
		after[fd] = remove(after[fd], fb)
		td := filepath.Dir(m.To)
		after[td] = append(after[td], filepath.Base(m.To))
	}
	for d := range after {
		sort.Strings(after[d])
	}
	// Where each file ENDS UP. The ops are followed in order rather than
	// chained by name: a swap goes through a temporary name, so a file can
	// occupy a name another file had, and chasing names would go round for
	// ever.
	at := map[string]string{}    // current path -> the path it started at
	final := map[string]string{} // started at -> current path
	for _, m := range moves {
		orig, ok := at[m.From]
		if !ok {
			orig = m.From
		}
		delete(at, m.From)
		at[m.To] = orig
		final[orig] = m.To
	}
	b := locators(root, before)
	a := locators(root, after)
	for _, dir := range sortedKeys(before) {
		for _, base := range before[dir] {
			from := filepath.Join(dir, base)
			to := from
			if end, ok := final[from]; ok {
				to = end
			}
			switch {
			case strings.EqualFold(b[from], a[to]): // FAT does not see case
			case b[from] == "":
				added++
			default:
				recopied++
			}
		}
	}
	return recopied, added
}

// locators computes, for every FLAC in a tree listing, the "<device
// folder>/<device name>" library.ScanTree would give it — or "" when the scan
// would not see the file at all. It is library's enumeration, rule for rule:
// only immediate subfolders of the root are albums, a folder with "Disc …"
// subfolders contributes ONLY those subfolders' files, and the position in the
// byte-sorted listing is the NN.
func locators(root string, dirs map[string][]string) map[string]string {
	children := map[string][]string{}
	for d := range dirs {
		if d == root {
			continue
		}
		parent := filepath.Dir(d)
		children[parent] = append(children[parent], filepath.Base(d))
	}
	out := map[string]string{}
	var albums []string
	for _, name := range children[root] {
		albums = append(albums, name)
	}
	sort.Strings(albums)
	for _, name := range albums {
		adir := filepath.Join(root, name)
		artist, album := library.SplitAlbumArtist(name)
		if artist == "" {
			continue // library skips a folder with no " - "
		}
		folder := library.FatSafe(artist + " - " + album)
		var discs []string
		for _, c := range children[adir] {
			if strings.HasPrefix(c, "Disc ") {
				discs = append(discs, c)
			}
		}
		sort.Strings(discs)
		var files []string
		if len(discs) > 0 {
			for _, d := range discs {
				sub := append([]string(nil), dirs[filepath.Join(adir, d)]...)
				sort.Strings(sub)
				for _, f := range sub {
					files = append(files, filepath.Join(adir, d, f))
				}
			}
		} else {
			sub := append([]string(nil), dirs[adir]...)
			sort.Strings(sub)
			for _, f := range sub {
				files = append(files, filepath.Join(adir, f))
			}
		}
		for i, f := range files {
			out[f] = folder + "/" + fmt.Sprintf("%02d. %s.flac", i+1,
				library.FatSafe(library.TrackTitle(filepath.Base(f))))
		}
	}
	return out
}

// ------------------------------------------------------------- the walk

func walkTree(root string) (map[string]*node, []string, error) {
	nodes := map[string]*node{root: {}}
	var warns []string
	err := filepath.WalkDir(root, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if d.IsDir() {
			if p != root {
				nodes[p] = &node{}
			}
			return nil
		}
		if d.Type()&fs.ModeSymlink != 0 {
			return nil
		}
		n := nodes[filepath.Dir(p)]
		if n == nil {
			return nil
		}
		base := d.Name()
		switch {
		case strings.HasPrefix(base, "."):
			// A leading dot hides a file from library's glob; leave it alone.
		case strings.HasSuffix(base, ".flac"):
			n.flacs = append(n.flacs, base)
		case strings.EqualFold(filepath.Ext(base), ".flac"):
			warns = append(warns, fmt.Sprintf("%s: extension is not lower-case .flac; left where it is (library skips it too, so the track is not on the device either way)", p))
		case sidecarNames[strings.ToLower(base)], isPlaylist(base):
			// Pictures and an album-local playlist belong to the album: a
			// playlist left behind in an emptied folder names files that
			// are no longer beside it.
			n.sidecars = append(n.sidecars, base)
		}
		return nil
	})
	if err != nil {
		return nil, nil, err
	}
	for dir, n := range nodes {
		sort.Strings(n.flacs)
		sort.Strings(n.sidecars)
		if dir == root {
			continue
		}
		if parent := nodes[filepath.Dir(dir)]; parent != nil && strings.HasPrefix(filepath.Base(dir), "Disc ") {
			parent.discDirs = append(parent.discDirs, filepath.Base(dir))
		}
	}
	sort.Strings(warns)
	return nodes, warns, nil
}

// albumDirOf is the folder an album is keyed by: the file's own folder, or its
// parent when the folder is a "Disc N" one (which is how library reads a
// multi-disc album). The root itself is never a "Disc N" parent.
func albumDirOf(root, dir string) (albumDir string, folderDisc int) {
	if dir == root {
		return dir, 0
	}
	base := filepath.Base(dir)
	if !strings.HasPrefix(base, "Disc ") {
		return dir, 0
	}
	n := 1
	if m := discFolderRe.FindStringSubmatch(base); m != nil {
		n = library.LeadInt(m[1])
	}
	return filepath.Dir(dir), n
}

// --------------------------------------------------------------- helpers

func sortedKeys[V any](m map[string]V) []string {
	out := make([]string, 0, len(m))
	for k := range m {
		out = append(out, k)
	}
	sort.Strings(out)
	return out
}

func remove(list []string, s string) []string {
	out := list[:0:0]
	for _, v := range list {
		if v != s {
			out = append(out, v)
		}
	}
	return out
}

func rel(root, p string) string {
	if r, err := filepath.Rel(root, p); err == nil && !strings.HasPrefix(r, "..") {
		return filepath.ToSlash(r)
	}
	return p
}

func quote(s string) string {
	if s == "" {
		return "nothing"
	}
	return fmt.Sprintf("%q", s)
}
