// Package tagcache builds and reads the binary on-disk music index
// (the .tcdb file) that the firmware mmaps at startup.
//
// On real hardware the iPod can't afford a scan-at-startup pass — even
// a small library on a USB disk takes seconds-to-minutes to walk and
// re-parse every tag. Instead, the user runs `core tagcache build`
// once on the host (after re-syncing music), and the firmware then
// loads the precomputed binary at boot.
//
// File layout (all integers little-endian, the iPod ARM is LE):
//
//	+-------- Header (104 bytes) --------+
//	|  magic[4]      = "TCDB"             |
//	|  version u32   = 1                  |
//	|  song_count u32                     |
//	|  n_artists u32   (uniq)             |
//	|  n_albums u32                       |
//	|  n_genres u32                       |
//	|  n_composers u32                    |
//	|  songs_off u64                      |
//	|  artist_idx_off u64                 |  array of u32 string-offsets
//	|  album_idx_off u64                  |
//	|  genre_idx_off u64                  |
//	|  composer_idx_off u64               |
//	|  artist_groups_off u64              |  per-group song-list arrays
//	|  album_groups_off u64               |
//	|  genre_groups_off u64               |
//	|  composer_groups_off u64            |
//	|  strings_off, strings_len u64       |
//	|  art_off, art_len u64               |
//	+-------- Song records ---------------+
//	|  song_count * SongRecord (40 B ea)  |
//	+-------- Uniq tables ---------------+
//	|  n_artists   * u32 string offsets   |  sorted (case-insensitive)
//	|  n_albums    * u32                  |
//	|  n_genres    * u32                  |
//	|  n_composers * u32                  |
//	+-------- Per-group song lists -------+
//	|  for each dimension (a/b/g/c):      |
//	|    n_X * u32 offsets (relative      |
//	|        to the dimension's _off)     |
//	|    [variable groups, each:          |
//	|       u32 count + count*u32 indices]|
//	+-------- String table ---------------+
//	|  null-terminated UTF-8 strings,     |
//	|  deduped. String 0 is "" (empty).   |
//	+-------- Art blob -------------------+
//	|  raw JPEG bytes, concatenated.      |
//	|  Per-song art_off/art_len point     |
//	|  into here (relative to art_off).   |
//	+-------------------------------------+
//
// Songs are sorted alphabetically by title (case-insensitive). Uniq
// tables are sorted alphabetically. Per-group song-list orders match
// the global song order, so the firmware can present the same drilldown
// rows whether it reads from the binary cache or scan-at-startup.
//
// Versioning: bump `Version` whenever the binary layout changes in a
// non-additive way. Readers must reject mismatched versions rather than
// try to interpret them.
package tagcache

import "encoding/binary"

// Magic is the four-byte file signature.
var Magic = [4]byte{'T', 'C', 'D', 'B'}

// Version is the on-disk format version. Bumped on incompatible layout
// changes; the firmware reader refuses to load mismatched versions.
const Version uint32 = 1

// HeaderSize is the byte size of the fixed header. The file's first
// byte after the header is the start of the song-record array.
//
// Layout (little-endian, no padding — encoding/binary packs verbatim):
//
//	[0..4)     magic                  4 B
//	[4..28)    6 * u32 (version + 5 counts) 24 B
//	[28..36)   songs_off u64           8 B
//	[36..68)   4 * u64 idx offsets    32 B
//	[68..100)  4 * u64 group offsets  32 B
//	[100..116) strings_off, _len u64  16 B
//	[116..132) art_off,     _len u64  16 B
const HeaderSize = 132

// SongRecordSize is the on-disk size of one song record. Fixed so the
// reader can index by global song idx in O(1).
const SongRecordSize = 40

// MissingTag is the sentinel index value (-1) used in song records when
// the file's tag for a given dimension is absent.
const MissingTag = int32(-1)

// LE is the byte order used throughout the file.
var LE = binary.LittleEndian

// Header mirrors the on-disk layout of the fixed header. Fields are
// populated by Builder.Write and verified by Reader.
type Header struct {
	Magic       [4]byte
	Version     uint32
	SongCount   uint32
	NArtists    uint32
	NAlbums     uint32
	NGenres     uint32
	NComposers  uint32
	SongsOff    uint64

	ArtistIdxOff   uint64
	AlbumIdxOff    uint64
	GenreIdxOff    uint64
	ComposerIdxOff uint64

	ArtistGroupsOff   uint64
	AlbumGroupsOff    uint64
	GenreGroupsOff    uint64
	ComposerGroupsOff uint64

	StringsOff uint64
	StringsLen uint64
	ArtOff     uint64
	ArtLen     uint64
}

// SongRecord mirrors the on-disk layout of one entry in the song-record
// array. ArtistIdx/AlbumIdx/GenreIdx/ComposerIdx are MissingTag (-1)
// when the file had no tag for that dimension. ArtOff/ArtLen are zero
// when the file had no embedded picture; otherwise ArtOff is relative
// to the file's ArtOff.
type SongRecord struct {
	TitleOff     uint32
	PathOff      uint32
	ArtistIdx    int32
	AlbumIdx     int32
	GenreIdx     int32
	ComposerIdx  int32
	ArtOff       uint64
	ArtLen       uint64
}
