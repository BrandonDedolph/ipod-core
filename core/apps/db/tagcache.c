/*
 * core/apps/db/tagcache.c — synthetic library data with optional
 * runtime "library mode" for the Songs list.
 *
 * Default state: hardcoded artist / album / song / genre / composer
 * lists drawn from the example library in design_handoff_rockbox_theme/
 * menus.jsx + collection-detail.jsx (Aphex Twin / Drukqs / Avril 14th,
 * etc). Lets the Music sub-menu show real-looking content while we
 * build the Go-side indexer + on-disk binary tagcache format.
 *
 * After tagcache_library_load(dir): the Songs list is replaced with
 * .flac/.mp3 files discovered in `dir`, sorted by filename. Each row
 * maps to a real filesystem path via tagcache_song_path(idx). Other
 * categories (artists / albums / etc.) stay synthetic until proper
 * tag-driven categorization lands.
 *
 * When the binary tagcache + Go indexer arrive, this becomes a thin
 * reader over the mmap'd binary; the public API in tagcache.h does
 * not change.
 */

#include "tagcache.h"

#include <ctype.h>
#include <dirent.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */

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

/* ---------- Library mode (runtime-loaded songs) ------------------- */

/*
 * When tagcache_library_load() succeeds (even with zero matches),
 * LIBRARY.loaded is set; titles[]/paths[] are heap-allocated parallel
 * arrays. The synthetic SONGS array is shadowed (not freed — read-only).
 *
 * `loaded` is what flips song_count / song_title from the synthetic
 * fallback to the library — we explicitly do NOT use count > 0, so
 * `--music <empty-dir>` shows an empty Songs list rather than silently
 * reverting to the example data.
 */
typedef struct {
    char **titles;       /* basename without extension */
    char **paths;        /* full filesystem path */
    int    count;
    int    cap;
    int    loaded;
} library_t;

static library_t LIBRARY = { NULL, NULL, 0, 0, 0 };

static void library_clear(void) {
    for (int i = 0; i < LIBRARY.count; i++) {
        free(LIBRARY.titles[i]);
        free(LIBRARY.paths[i]);
    }
    free(LIBRARY.titles);
    free(LIBRARY.paths);
    LIBRARY.titles = NULL;
    LIBRARY.paths  = NULL;
    LIBRARY.count  = 0;
    LIBRARY.cap    = 0;
    LIBRARY.loaded = 0;
}

/*
 * Returns true if `name` ends with a recognized audio extension
 * (case-insensitive). Sets *out_title_len to the length of the basename
 * minus the extension so the caller can build a title without it.
 */
static int has_audio_ext(const char *name, size_t *out_title_len) {
    size_t n = strlen(name);
    if (n < 5) return 0;
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name) return 0;
    char ext[8];
    size_t ext_len = strlen(dot + 1);
    if (ext_len >= sizeof(ext)) return 0;
    for (size_t i = 0; i < ext_len; i++) ext[i] = (char)tolower((unsigned char)dot[1 + i]);
    ext[ext_len] = 0;
    if (strcmp(ext, "flac") == 0 || strcmp(ext, "mp3") == 0) {
        *out_title_len = (size_t)(dot - name);
        return 1;
    }
    return 0;
}

/* Sort indices into LIBRARY.titles via strcasecmp, so we can keep
 * titles[] / paths[] aligned without copying pairs. */
static char **CMP_TITLES;
static int qsort_cmp_idx(const void *a, const void *b) {
    int ia = *(const int *)a;
    int ib = *(const int *)b;
    return strcasecmp(CMP_TITLES[ia], CMP_TITLES[ib]);
}

int tagcache_library_load(const char *dir) {
    if (!dir) return -1;
    DIR *d = opendir(dir);
    if (!d) return -1;

    /* Scan into a fresh staging buffer so a partial failure leaves
     * the previous LIBRARY untouched. */
    library_t staging = { NULL, NULL, 0, 0, 0 };

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t title_len;
        if (!has_audio_ext(ent->d_name, &title_len)) continue;

        /* Grow staging in place. */
        if (staging.cap <= staging.count) {
            int new_cap = staging.cap ? staging.cap * 2 : 32;
            char **t = (char **)realloc(staging.titles, (size_t)new_cap * sizeof(char *));
            char **p = (char **)realloc(staging.paths,  (size_t)new_cap * sizeof(char *));
            if (!t || !p) {
                free(t); free(p);
                /* Drop everything in staging. */
                for (int i = 0; i < staging.count; i++) {
                    free(staging.titles[i]); free(staging.paths[i]);
                }
                free(staging.titles); free(staging.paths);
                closedir(d);
                return -1;
            }
            staging.titles = t;
            staging.paths  = p;
            staging.cap    = new_cap;
        }

        /* title = basename without extension */
        char *title = (char *)malloc(title_len + 1);
        if (!title) break;
        memcpy(title, ent->d_name, title_len);
        title[title_len] = 0;

        /* path = dir + '/' + name */
        size_t dir_len  = strlen(dir);
        size_t name_len = strlen(ent->d_name);
        size_t need     = dir_len + 1 + name_len + 1;
        char *path = (char *)malloc(need);
        if (!path) { free(title); break; }
        int strip_slash = (dir_len > 0 && dir[dir_len - 1] == '/') ? 1 : 0;
        snprintf(path, need, "%s%s%s",
                 dir,
                 strip_slash ? "" : "/",
                 ent->d_name);

        staging.titles[staging.count] = title;
        staging.paths [staging.count] = path;
        staging.count++;
    }
    closedir(d);

    /* Sort by title (case-insensitive). Build an index permutation
     * to keep titles[] / paths[] aligned. */
    if (staging.count > 1) {
        int *idx = (int *)malloc((size_t)staging.count * sizeof(int));
        if (idx) {
            for (int i = 0; i < staging.count; i++) idx[i] = i;
            CMP_TITLES = staging.titles;
            qsort(idx, (size_t)staging.count, sizeof(int), qsort_cmp_idx);

            char **sorted_titles = (char **)malloc((size_t)staging.count * sizeof(char *));
            char **sorted_paths  = (char **)malloc((size_t)staging.count * sizeof(char *));
            if (sorted_titles && sorted_paths) {
                for (int i = 0; i < staging.count; i++) {
                    sorted_titles[i] = staging.titles[idx[i]];
                    sorted_paths [i] = staging.paths [idx[i]];
                }
                free(staging.titles);
                free(staging.paths);
                staging.titles = sorted_titles;
                staging.paths  = sorted_paths;
            } else {
                free(sorted_titles);
                free(sorted_paths);
            }
            free(idx);
        }
    }

    /* Commit: drop any previous library, install staging. */
    library_clear();
    LIBRARY.titles = staging.titles;
    LIBRARY.paths  = staging.paths;
    LIBRARY.count  = staging.count;
    LIBRARY.cap    = staging.cap;
    LIBRARY.loaded = 1;
    return LIBRARY.count;
}

const char *tagcache_song_path(int idx) {
    if (!LIBRARY.loaded) return NULL;
    if (idx < 0 || idx >= LIBRARY.count) return NULL;
    return LIBRARY.paths[idx];
}

/* ---------- API impl --------------------------------------------- */

#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

int tagcache_artist_count(void)   { return ARRAY_LEN(ARTISTS); }
int tagcache_album_count(void)    { return ARRAY_LEN(ALBUMS); }
int tagcache_song_count(void) {
    return LIBRARY.loaded ? LIBRARY.count : ARRAY_LEN(SONGS);
}
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
    if (LIBRARY.loaded) {
        if (idx < 0 || idx >= LIBRARY.count) return "(?)";
        return LIBRARY.titles[idx];
    }
    return bounded(SONGS, ARRAY_LEN(SONGS), idx);
}
const char *tagcache_genre_name(int idx) {
    return bounded(GENRES, ARRAY_LEN(GENRES), idx);
}
const char *tagcache_composer_name(int idx) {
    return bounded(COMPOSERS, ARRAY_LEN(COMPOSERS), idx);
}
