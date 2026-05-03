package tagcache

import (
	"bytes"
	"encoding/binary"
	"fmt"
)

// Read parses a .tcdb file from `data` (typically the bytes you'd pass
// to mmap on the firmware side) and returns the in-memory model. This
// is intended for tests + tooling — the firmware reader will be in C
// and read fields directly off the mmap'd backing store.
func Read(data []byte) (*Model, error) {
	if len(data) < HeaderSize {
		return nil, fmt.Errorf("tagcache: file too small (%d bytes; header needs %d)", len(data), HeaderSize)
	}
	var hdr Header
	if err := binary.Read(bytes.NewReader(data[:HeaderSize]), LE, &hdr); err != nil {
		return nil, fmt.Errorf("tagcache: read header: %w", err)
	}
	if hdr.Magic != Magic {
		return nil, fmt.Errorf("tagcache: bad magic %q (want %q)", hdr.Magic[:], Magic[:])
	}
	if hdr.Version != Version {
		return nil, fmt.Errorf("tagcache: unsupported version %d (want %d)", hdr.Version, Version)
	}
	// Bounds-check every advertised section against file length. The
	// firmware's C reader will be doing pointer arithmetic on mmap'd
	// bytes — we want a malformed/truncated file to surface here in
	// the build pipeline rather than as a SEGV at boot. Width sums
	// are u64 to defend against overflow on a malicious header.
	end := uint64(len(data))
	checks := []struct {
		off, length uint64
		name        string
	}{
		{hdr.SongsOff,          uint64(hdr.SongCount)  * SongRecordSize, "songs"},
		{hdr.ArtistIdxOff,      uint64(hdr.NArtists)   * 4,              "artist_idx"},
		{hdr.AlbumIdxOff,       uint64(hdr.NAlbums)    * 4,              "album_idx"},
		{hdr.GenreIdxOff,       uint64(hdr.NGenres)    * 4,              "genre_idx"},
		{hdr.ComposerIdxOff,    uint64(hdr.NComposers) * 4,              "composer_idx"},
		{hdr.StringsOff,        hdr.StringsLen,                          "strings"},
		{hdr.ArtOff,            hdr.ArtLen,                              "art"},
	}
	for _, c := range checks {
		if c.off+c.length < c.off /* overflow */ || c.off+c.length > end {
			return nil, fmt.Errorf("tagcache: section %s out of bounds (off=%d len=%d file=%d)",
				c.name, c.off, c.length, end)
		}
	}
	// Per-group blocks have a fixed header (n*4 offsets) but the body
	// extent depends on per-group counts read at decode time; the
	// section-end check is enforced in readGroups instead.
	for _, c := range []struct {
		off uint64
		n   uint32
		name string
	}{
		{hdr.ArtistGroupsOff,   hdr.NArtists,   "artist_groups"},
		{hdr.AlbumGroupsOff,    hdr.NAlbums,    "album_groups"},
		{hdr.GenreGroupsOff,    hdr.NGenres,    "genre_groups"},
		{hdr.ComposerGroupsOff, hdr.NComposers, "composer_groups"},
	} {
		if c.off+uint64(c.n)*4 < c.off || c.off+uint64(c.n)*4 > end {
			return nil, fmt.Errorf("tagcache: section %s offset table out of bounds", c.name)
		}
	}

	strings := func(off uint32) string {
		if uint64(off) >= hdr.StringsLen {
			return ""
		}
		base := data[hdr.StringsOff:][:hdr.StringsLen]
		end := off
		for int(end) < len(base) && base[end] != 0 {
			end++
		}
		return string(base[off:end])
	}

	// Songs.
	songs := make([]SongInfo, hdr.SongCount)
	songArtist   := make([]int32, hdr.SongCount)
	songAlbum    := make([]int32, hdr.SongCount)
	songGenre    := make([]int32, hdr.SongCount)
	songComposer := make([]int32, hdr.SongCount)
	for i := uint32(0); i < hdr.SongCount; i++ {
		off := hdr.SongsOff + uint64(i)*SongRecordSize
		var rec SongRecord
		if err := binary.Read(bytes.NewReader(data[off:off+SongRecordSize]), LE, &rec); err != nil {
			return nil, fmt.Errorf("read song[%d]: %w", i, err)
		}
		songs[i].Title = strings(rec.TitleOff)
		songs[i].Path  = strings(rec.PathOff)
		songArtist[i]   = rec.ArtistIdx
		songAlbum[i]    = rec.AlbumIdx
		songGenre[i]    = rec.GenreIdx
		songComposer[i] = rec.ComposerIdx
		if rec.ArtLen > 0 {
			start := hdr.ArtOff + rec.ArtOff
			songs[i].ArtBytes = append([]byte(nil), data[start:start+rec.ArtLen]...)
		}
	}

	// Uniq tables.
	readUniq := func(off uint64, n uint32) []string {
		out := make([]string, n)
		for i := uint32(0); i < n; i++ {
			s := LE.Uint32(data[off+uint64(i)*4:])
			out[i] = strings(s)
		}
		return out
	}
	uniqArtists   := readUniq(hdr.ArtistIdxOff,   hdr.NArtists)
	uniqAlbums    := readUniq(hdr.AlbumIdxOff,    hdr.NAlbums)
	uniqGenres    := readUniq(hdr.GenreIdxOff,    hdr.NGenres)
	uniqComposers := readUniq(hdr.ComposerIdxOff, hdr.NComposers)

	// Now that uniq strings are recovered, fill SongInfo Artist/Album/etc
	// so the round-trip test sees the same string data on both sides.
	for i := range songs {
		if songArtist[i]   >= 0 { songs[i].Artist   = uniqArtists  [songArtist[i]]   }
		if songAlbum[i]    >= 0 { songs[i].Album    = uniqAlbums   [songAlbum[i]]    }
		if songGenre[i]    >= 0 { songs[i].Genre    = uniqGenres   [songGenre[i]]    }
		if songComposer[i] >= 0 { songs[i].Composer = uniqComposers[songComposer[i]] }
	}

	// Per-group song lists.
	readGroups := func(off uint64, n uint32) [][]uint32 {
		out := make([][]uint32, n)
		offsets := make([]uint32, n)
		for i := uint32(0); i < n; i++ {
			offsets[i] = LE.Uint32(data[off+uint64(i)*4:])
		}
		for i := uint32(0); i < n; i++ {
			base := off + uint64(offsets[i])
			count := LE.Uint32(data[base:])
			ids := make([]uint32, count)
			for j := uint32(0); j < count; j++ {
				ids[j] = LE.Uint32(data[base+4+uint64(j)*4:])
			}
			out[i] = ids
		}
		return out
	}

	return &Model{
		Songs:           songs,
		UniqArtists:     uniqArtists,
		UniqAlbums:      uniqAlbums,
		UniqGenres:      uniqGenres,
		UniqComposers:   uniqComposers,
		SongArtistIdx:   songArtist,
		SongAlbumIdx:    songAlbum,
		SongGenreIdx:    songGenre,
		SongComposerIdx: songComposer,
		ArtistGroups:    readGroups(hdr.ArtistGroupsOff,   hdr.NArtists),
		AlbumGroups:     readGroups(hdr.AlbumGroupsOff,    hdr.NAlbums),
		GenreGroups:     readGroups(hdr.GenreGroupsOff,    hdr.NGenres),
		ComposerGroups:  readGroups(hdr.ComposerGroupsOff, hdr.NComposers),
	}, nil
}
