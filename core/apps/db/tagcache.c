/*
 * core/apps/db/tagcache.c — synthetic library data.
 *
 * Hardcoded artist / album / song / genre / composer lists drawn from
 * the example library in design_handoff_rockbox_theme/menus.jsx +
 * collection-detail.jsx (Aphex Twin / Drukqs / Avril 14th, etc).
 *
 * This stand-in lets the Music sub-menu show real-looking content
 * while we build the Go-side indexer + on-disk binary tagcache
 * format. When that lands, this file becomes a thin reader over the
 * mmap'd binary; the public API in tagcache.h doesn't change.
 */

#include "tagcache.h"

#include <stddef.h>

/* ---------- Synthetic data --------------------------------------- */

static const char *const ARTISTS[] = {
    "Aphex Twin",
    "Beach House",
    "Boards of Canada",
    "Bon Iver",
    "Brian Eno",
    "Caribou",
    "Cocteau Twins",
    "DJ Shadow",
    "Four Tet",
    "Fleet Foxes",
    "Grouper",
    "Jon Hopkins",
    "Khruangbin",
    "Mount Kimbie",
    "My Bloody Valentine",
    "Nicolas Jaar",
    "Nils Frahm",
    "Oneohtrix Point Never",
    "Radiohead",
    "Sigur Rós",
    "Slowdive",
    "The National",
    "Thom Yorke",
    "Tycho",
    "Warpaint",
    "William Basinski",
};

static const char *const ALBUMS[] = {
    "Drukqs",
    "Selected Ambient Works 85-92",
    "Bloom",
    "Music Has the Right to Children",
    "Music for Airports",
    "Andorra",
    "Treasure",
    "Endtroducing.....",
    "Rounds",
    "Helplessness Blues",
    "Dragging a Dead Deer Up a Hill",
    "Immunity",
    "Mordechai",
};

static const char *const SONGS[] = {
    "Vordhosbn",
    "Kladfvgbung Micshk",
    "Omgyjya Switch7",
    "Avril 14th",
    "Mt Saint Michel + Saint Michael's Mount",
    "Gwely Mernans",
    "Bbydhyonchord",
    "Cock/Ver10",
    "Strotha Tynhe",
    "Gwarek2",
    "Orban Eq Trx4",
    "Btoum-Roumada",
    "Lornaderek",
    "QKThr",
    "Meltphace 6",
    "Bit 4",
    "Prep Gwarlek 3b",
    "Father",
    "Taking Control",
    "Petiatil Cx Htdui",
    "Ruglen Holon",
    "Afx237 v.7",
    "Ziggomatic 17",
    "Beskhu3epnm",
    "Nanou2",
};

static const char *const GENRES[] = {
    "Ambient",
    "Electronic",
    "Folk",
    "Indie",
    "IDM",
    "Post-Rock",
    "Shoegaze",
    "Soundtrack",
};

static const char *const COMPOSERS[] = {
    "Aphex Twin",
    "Brian Eno",
    "Jon Hopkins",
    "Nils Frahm",
    "Tycho",
    "William Basinski",
};

/* ---------- API impl --------------------------------------------- */

#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

int tagcache_artist_count(void)   { return ARRAY_LEN(ARTISTS); }
int tagcache_album_count(void)    { return ARRAY_LEN(ALBUMS); }
int tagcache_song_count(void)     { return ARRAY_LEN(SONGS); }
int tagcache_genre_count(void)    { return ARRAY_LEN(GENRES); }
int tagcache_composer_count(void) { return ARRAY_LEN(COMPOSERS); }

static const char *bounded(const char *const *table, int count, int idx) {
    if (idx < 0 || idx >= count) return "(?)";
    return table[idx];
}

const char *tagcache_artist_name(int idx) {
    return bounded(ARTISTS, ARRAY_LEN(ARTISTS), idx);
}
const char *tagcache_album_name(int idx) {
    return bounded(ALBUMS, ARRAY_LEN(ALBUMS), idx);
}
const char *tagcache_song_title(int idx) {
    return bounded(SONGS, ARRAY_LEN(SONGS), idx);
}
const char *tagcache_genre_name(int idx) {
    return bounded(GENRES, ARRAY_LEN(GENRES), idx);
}
const char *tagcache_composer_name(int idx) {
    return bounded(COMPOSERS, ARRAY_LEN(COMPOSERS), idx);
}
