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
	if uint64(len(data)) < hdr.ArtOff+hdr.ArtLen {
		return nil, fmt.Errorf("tagcache: file truncated (have %d, need %d)", len(data), hdr.ArtOff+hdr.ArtLen)
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
