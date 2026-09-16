package library

import (
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flac"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/id3"
)

// AudioExts are the extensions the device can play, which is what the
// firmware's classify_ext() admits (core/library/names.c) and what
// tools/build_index.py globs. Lower-case on purpose: see audioIn.
var AudioExts = []string{".flac", ".mp3"}

// Track is one source file and everything the index record needs from it.
//
// THE FILENAME IS A LOCATOR, THE TRACK NUMBER IS METADATA. DeviceName is the
// name the file has (or will have) on the device, and its hash is what binds
// the record to the file: it is the position in the importer's enumeration,
// never the track number, and it is never renumbered — change it and every
// album whose source files are not already numbered re-hashes to names that
// are not there, and its tracks stop resolving. Disc/Track are what the device
// SHOWS and sorts by, and come from the tags first.
type Track struct {
	SrcPath string // absolute path of the source file
	Pos     int    // enumeration position, 1-based (= the NN in DeviceName)

	DeviceName   string // "NN. Title.flac" / "NN. Title.mp3" — THE locator
	DeviceFolder string // the album's FAT-safe folder, for the drift report

	Title     string
	Artist    string
	Genre     string
	Disc      int
	Track     int
	DurationS uint32

	// NumberFrom is where Track came from: "tag", "filename" or "position".
	NumberFrom string
}

// Album is one source folder ("Album - Artist") and its tracks, in the order
// the records are written.
type Album struct {
	SrcDir        string
	FolderArtist  string
	FolderAlbum   string
	DeviceFolder  string  // FatSafe("Artist - Album") — the locator
	DisplayFolder string  // "Artist - <album_disp>" — what the record carries
	Tracks        []Track // sorted (Disc, Track, Pos) = record order
}

// Failure is a file whose metadata could not be read. Its record is still
// written — duration 0 and a filename-derived title, which is exactly what a
// healthy record looks like from the device's side, so this list is the only
// warning there is.
type Failure struct {
	Path   string
	Reason string
}

// Scan is the whole source tree, ready to be encoded.
type Scan struct {
	Albums   []Album
	Skipped  []string  // source folders that produced nothing, and why
	Failures []Failure // metadata reads that failed
	Drift    []Track   // tracks whose number is not their enumeration position
	Warnings []string  // things worth saying that are not failures
}

// Options configure ScanTree. The zero value is usable: no genre map (tag
// genres only), flac.ReadFile for FLACs and id3.ReadFile for MP3s.
type Options struct {
	// GenreMap is the per-artist primary genre, keyed by the FOLDER artist.
	GenreMap map[string]string
	// Meta reads one FLAC's metadata. Defaults to flac.ReadFile; tests
	// substitute their own.
	Meta func(path string) (*flac.Meta, error)
	// MP3Meta reads one MP3's tags and duration. Defaults to id3.ReadFile.
	// Separate from Meta rather than one interface because the two readers
	// return different types and a test that stubs one usually wants the
	// other left alone.
	MP3Meta func(path string) (*id3.Meta, error)
}

// SongCount is the number of records the scan will produce.
func (s *Scan) SongCount() int {
	n := 0
	for i := range s.Albums {
		n += len(s.Albums[i].Tracks)
	}
	return n
}

// GenreCount is the number of distinct non-empty genres in the scan — the
// device holds at most MaxGenres of them.
func (s *Scan) GenreCount() int {
	seen := map[string]bool{}
	for i := range s.Albums {
		for _, t := range s.Albums[i].Tracks {
			if t.Genre != "" {
				seen[t.Genre] = true
			}
		}
	}
	return len(seen)
}

// discDirRe reads the disc number out of a "Disc N" folder name.
var discDirRe = regexp.MustCompile(`Disc[` + pySpace + `]+(\d+)`)

// ScanTree walks a source tree of "Album - Artist" folders and returns the
// albums and tracks in index order. It mirrors the body of build_index.py's
// main() exactly; the order of the result IS the order of the records.
func ScanTree(src string, o Options) (*Scan, error) {
	if o.Meta == nil {
		o.Meta = flac.ReadFile
	}
	if o.MP3Meta == nil {
		o.MP3Meta = id3.ReadFile
	}
	st, err := os.Stat(src)
	if err != nil || !st.IsDir() {
		return nil, fmt.Errorf("source tree not found: %s  (pass --src)", src)
	}

	names, err := readDirNames(src)
	if err != nil {
		return nil, err
	}

	scan := &Scan{}
	for _, folder := range names {
		adir := filepath.Join(src, folder)
		if fi, err := os.Stat(adir); err != nil || !fi.IsDir() {
			continue
		}
		artistF, albumF := SplitAlbumArtist(folder)
		if artistF == "" {
			// No " - ": not an album folder. build_index.py skips it silently.
			scan.Skipped = append(scan.Skipped, folder+" (no \" - \" in the folder name)")
			continue
		}
		files, warns, err := discTracks(adir)
		if err != nil {
			return nil, err
		}
		scan.Warnings = append(scan.Warnings, warns...)
		if len(files) == 0 {
			scan.Skipped = append(scan.Skipped, folder+" (no .flac or .mp3 files)")
			continue
		}

		album, err := scanAlbum(adir, artistF, albumF, files, o, scan)
		if err != nil {
			return nil, err
		}
		scan.Albums = append(scan.Albums, album)
	}
	return scan, nil
}

// srcFile is one source file in the importer's enumeration order.
type srcFile struct {
	path       string
	folderDisc int // disc from the "Disc N" folder, 0 for a flat album
}

func scanAlbum(adir, artistF, albumF string, files []srcFile, o Options, scan *Scan) (Album, error) {
	dest := FatSafe(artistF + " - " + albumF) // FAT-safe: hash + device match

	album := Album{
		SrcDir:       adir,
		FolderArtist: artistF,
		FolderAlbum:  albumF,
		DeviceFolder: dest,
	}

	type keyed struct {
		disc, track, pos int
		t                Track
	}
	var rows []keyed
	seen := map[string]bool{}
	haveDisp := false

	for i, f := range files {
		pos := i + 1
		base := filepath.Base(f.path)
		stem, _ := splitExt(base)
		ftitle := FatSafe(TrackTitle(base))

		// Destination filename convention: continuous "NN. Title.ext", NN =
		// position in the importer's enumeration. Deliberately NOT the track
		// number. The SOURCE extension is kept: nothing transcodes, so an MP3
		// goes across as an MP3 and the firmware picks its decoder from the
		// name. The de-duplication below cannot actually fire (NN is unique
		// within the album), but it is what the reference tool does, so it
		// stays — bounded, because the reference loop is not.
		ext := strings.ToLower(filepath.Ext(base))
		fname := fmt.Sprintf("%02d. %s%s", pos, ftitle, ext)
		for tries := 0; seen[strings.ToLower(fname)]; tries++ {
			if tries > 99 {
				return Album{}, fmt.Errorf("%s: cannot find a unique device name for %q", dest, base)
			}
			fname = fmt.Sprintf("%02d. %s (%d)%s", pos, ftitle, len(seen), ext)
		}
		seen[strings.ToLower(fname)] = true

		p := probeFile(f.path, o, scan)

		disc, trk := TrackNumber(stem, p.track, p.disc, f.folderDisc, artistF)
		numberFrom := "position"
		switch {
		case p.track != 0:
			numberFrom = "tag"
		case trk != 0:
			numberFrom = "filename"
		}
		if trk == 0 {
			trk = pos // unnumbered: the enumeration position
		}

		title := p.title
		if title == "" {
			title = TrackTitle(base)
		}
		artist := artistF
		if artist == "" {
			artist = p.artist
		}

		// Genre: prefer a clean per-artist genre; only keep the tag genre if
		// the artist isn't mapped AND the tag is a single (non-comma) value.
		genre := p.genre
		if mapped := o.GenreMap[artistF]; mapped != "" {
			genre = mapped
		} else if c := strings.Index(genre, ","); c >= 0 {
			genre = pyStrip(genre[:c])
		}

		// DISPLAY folder = the real "Artist - Album": use the tag album when
		// it only differs from the folder by FAT sanitization, so ? * : /
		// come back on screen. The locator stays FatSafe via the folder hash,
		// so matching is unaffected. The FIRST track decides, for the album.
		if !haveDisp {
			haveDisp = true
			if p.album != "" && Straighten(FatSafe(p.album)) == Straighten(albumF) {
				album.DisplayFolder = artistF + " - " + Straighten(p.album)
			} else {
				album.DisplayFolder = artistF + " - " + albumF
			}
		}

		t := Track{
			SrcPath:      f.path,
			Pos:          pos,
			DeviceName:   fname,
			DeviceFolder: dest,
			Title:        title,
			Artist:       artist,
			Genre:        genre,
			Disc:         disc,
			Track:        trk,
			DurationS:    p.duration,
			NumberFrom:   numberFrom,
		}
		if trk != pos {
			scan.Drift = append(scan.Drift, t)
		}
		rows = append(rows, keyed{disc, trk, pos, t})
	}

	// The index is the device's playing order: (disc, track), with the
	// enumeration position as the tie-break so two tracks carrying the same
	// number keep a stable, explicable order rather than an accidental one.
	sort.SliceStable(rows, func(a, b int) bool {
		x, y := rows[a], rows[b]
		if x.disc != y.disc {
			return x.disc < y.disc
		}
		if x.track != y.track {
			return x.track < y.track
		}
		return x.pos < y.pos
	})
	album.Tracks = make([]Track, len(rows))
	for i, r := range rows {
		album.Tracks[i] = r.t
	}
	return album, nil
}

// probed is what the reference tool's probe() returns.
type probed struct {
	title, artist, album, genre string
	duration                    uint32
	track, disc                 int
}

// probeFile reads one file's tags and duration, picking the reader from the
// extension. Both readers key their tags by ffprobe's names, so everything
// below this point is format-agnostic — which is the whole reason the ID3
// reader normalises its frame ids rather than handing back TIT2 and TPE1.
func probeFile(path string, o Options, scan *Scan) probed {
	var (
		tags map[string]string
		dur  uint32
		err  error
	)
	if strings.EqualFold(filepath.Ext(path), ".mp3") {
		var m *id3.Meta
		if m, err = o.MP3Meta(path); err == nil {
			tags, dur = m.Tags, m.DurationSeconds()
		}
	} else {
		var m *flac.Meta
		if m, err = o.Meta(path); err == nil {
			tags, dur = m.Tags, m.DurationSeconds()
		}
	}
	if err != nil {
		// A single unreadable file must not abort a 1000-track build, but the
		// failure is RECORDED: its record carries duration 0 and a
		// filename-derived title, which looks exactly like a healthy one.
		scan.Failures = append(scan.Failures, Failure{Path: path, Reason: err.Error()})
		return probed{}
	}
	get := func(keys ...string) string {
		for _, k := range keys {
			if v, ok := tags[k]; ok {
				return v
			}
		}
		return ""
	}
	return probed{
		title: get("title"),
		// ffprobe renames ALBUMARTIST / "ALBUM ARTIST" to album_artist; the
		// raw Vorbis key is what this reader hands back, so all the spellings
		// are tried. build_index.py falls back on key PRESENCE, not on a
		// non-empty value, which `get` reproduces.
		artist:   get("artist", "albumartist", "album_artist", "album artist"),
		album:    get("album"),
		genre:    get("genre"),
		duration: dur,
		// ffprobe renames TRACKNUMBER -> track and DISCNUMBER -> disc.
		track: LeadInt(get("tracknumber", "track")),
		disc:  LeadInt(get("discnumber", "disc")),
	}
}

// discTracks returns the source files in the IMPORTER'S ENUMERATION ORDER —
// "Disc N" folders ascending, files sorted within each — with the disc from
// the folder (ground truth) or 0 for a flat album (the tag decides then).
//
// The order is the filename contract: the position in this list is the NN in
// "NN. Title.ext" on the device. Do not sort it any other way.
func discTracks(adir string) ([]srcFile, []string, error) {
	names, err := readDirNames(adir)
	if err != nil {
		return nil, nil, err
	}
	var warns []string
	var discs []string
	for _, n := range names {
		if !strings.HasPrefix(n, "Disc ") {
			continue
		}
		if fi, err := os.Stat(filepath.Join(adir, n)); err == nil && fi.IsDir() {
			discs = append(discs, n)
		}
	}
	if len(discs) > 0 {
		var out []srcFile
		for _, d := range discs {
			dn := 1
			if m := discDirRe.FindStringSubmatch(d); m != nil {
				dn = atoi(m[1])
			}
			sub, w, err := audioIn(filepath.Join(adir, d))
			if err != nil {
				return nil, nil, err
			}
			warns = append(warns, w...)
			for _, p := range sub {
				out = append(out, srcFile{path: p, folderDisc: dn})
			}
		}
		return out, warns, nil
	}
	flat, w, err := audioIn(adir)
	if err != nil {
		return nil, nil, err
	}
	warns = append(warns, w...)
	out := make([]srcFile, 0, len(flat))
	for _, p := range flat {
		out = append(out, srcFile{path: p, folderDisc: 0})
	}
	return out, warns, nil
}

// audioIn lists the playable entries of one directory, sorted, mirroring
// build_index.py's audio_in(): a leading "." hides an entry from a wildcard
// pattern, and the extension match is CASE-SENSITIVE. A ".FLAC" or ".MP3"
// file is therefore not in the index — the reference tool would not have put
// it there either, and a track that silently is not in the library is worth a
// word.
//
// ONE sorted list across both extensions, not FLACs then MP3s: the position in
// it is the NN the file gets on the device, so grouping by extension would
// renumber a mixed album the moment a track changed format.
func audioIn(dir string) ([]string, []string, error) {
	names, err := readDirNames(dir)
	if err != nil {
		return nil, nil, err
	}
	var out, warns []string
	for _, n := range names {
		if strings.HasPrefix(n, ".") {
			continue
		}
		ext := filepath.Ext(n)
		matched := false
		for _, want := range AudioExts {
			switch {
			case ext == want:
				out = append(out, filepath.Join(dir, n))
				matched = true
			case strings.EqualFold(ext, want):
				warns = append(warns, fmt.Sprintf("%s: extension is not lower-case %s; skipped (the reference tool skips it too, so the track would be missing from the device either way)", filepath.Join(dir, n), want))
				matched = true
			}
			if matched {
				break
			}
		}
	}
	// readDirNames already sorted the names, so appending in that order gives
	// the single lexicographic list the enumeration contract asks for.
	return out, warns, nil
}

// readDirNames lists a directory sorted by byte order, which is what
// sorted(os.listdir()) and sorted(glob.glob()) give for UTF-8 names: UTF-8
// sorts bytewise in code-point order.
func readDirNames(dir string) ([]string, error) {
	ents, err := os.ReadDir(dir)
	if err != nil {
		return nil, err
	}
	names := make([]string, 0, len(ents))
	for _, e := range ents {
		names = append(names, e.Name())
	}
	sort.Strings(names)
	return names, nil
}
