package library

// The firmware's fixed library caps, from core/kernel/main.c. Past a cap the
// device silently drops: library_load_index() stops reading records at
// LIB_MAX_SONGS with no error and no log, so the songs past it simply are not
// in the library. Nothing on the device will ever tell the user that; the host
// has to check before it writes.
//
// These are duplicated from C, so caps_test.go greps main.c for the three
// #defines and fails if they have moved. (build_index.py reads main.c at run
// time instead; a Go binary ships without the repo beside it, so the numbers
// are constants here and the test is what keeps them honest.)
const (
	// MaxSongs is LIB_MAX_SONGS: index records the device will load.
	MaxSongs = 6000
	// MaxAlbums is LIB_MAX_ALBUMS: distinct album folders it can hold.
	MaxAlbums = 1024
	// MaxGenres is LIB_MAX_GENRES: distinct genre names it can hold.
	MaxGenres = 128
)
