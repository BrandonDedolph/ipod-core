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

#include "../../codecs/dr_flac/tag_flac.h"
#include "../../codecs/dr_mp3/tag_mp3.h"
#include "../../codecs/tags.h"

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
    char **titles;       /* tag TITLE if present, else basename-without-ext */
    char **paths;        /* full filesystem path */
    char **artists;      /* tag ARTIST or NULL if missing */
    char **albums;       /* tag ALBUM  or NULL if missing */
    int    count;
    int    cap;
    int    loaded;

    /* Derived indexes built after the scan: sorted unique sets of
     * the artists and albums seen in the songs list. Used by the
     * Artists / Albums menus when a library is loaded. Songs without
     * a given tag are skipped (a song with no ARTIST tag doesn't
     * contribute a "(?)" entry to the artist list). */
    char **uniq_artists;
    int    uniq_artist_count;
    char **uniq_albums;
    int    uniq_album_count;
} library_t;

static library_t LIBRARY = { NULL, NULL, NULL, NULL, 0, 0, 0,
                             NULL, 0, NULL, 0 };

static void library_clear(void) {
    for (int i = 0; i < LIBRARY.count; i++) {
        free(LIBRARY.titles[i]);
        free(LIBRARY.paths[i]);
        free(LIBRARY.artists[i]);
        free(LIBRARY.albums[i]);
    }
    free(LIBRARY.titles);
    free(LIBRARY.paths);
    free(LIBRARY.artists);
    free(LIBRARY.albums);

    for (int i = 0; i < LIBRARY.uniq_artist_count; i++) free(LIBRARY.uniq_artists[i]);
    for (int i = 0; i < LIBRARY.uniq_album_count;  i++) free(LIBRARY.uniq_albums [i]);
    free(LIBRARY.uniq_artists);
    free(LIBRARY.uniq_albums);

    LIBRARY.titles            = NULL;
    LIBRARY.paths             = NULL;
    LIBRARY.artists           = NULL;
    LIBRARY.albums            = NULL;
    LIBRARY.count             = 0;
    LIBRARY.cap               = 0;
    LIBRARY.loaded            = 0;
    LIBRARY.uniq_artists      = NULL;
    LIBRARY.uniq_artist_count = 0;
    LIBRARY.uniq_albums       = NULL;
    LIBRARY.uniq_album_count  = 0;
}

/*
 * Free the heap contents of a (staging or live) library_t. Used during
 * scan teardown on partial-failure paths. Doesn't reset the bookkeeping
 * — the struct is about to be discarded.
 */
static void library_free_contents(library_t *lib) {
    for (int i = 0; i < lib->count; i++) {
        free(lib->titles[i]);
        free(lib->paths[i]);
        free(lib->artists[i]);
        free(lib->albums[i]);
    }
    free(lib->titles);
    free(lib->paths);
    free(lib->artists);
    free(lib->albums);
    for (int i = 0; i < lib->uniq_artist_count; i++) free(lib->uniq_artists[i]);
    for (int i = 0; i < lib->uniq_album_count;  i++) free(lib->uniq_albums [i]);
    free(lib->uniq_artists);
    free(lib->uniq_albums);
}

/*
 * Grow `lib`'s capacity to at least `min_cap`. Each realloc is
 * committed back to lib before the next is attempted, so a failure
 * never leaves a freed-old + orphaned-new pair behind. Returns 0 on
 * success, -1 on allocation failure.
 */
static int library_reserve(library_t *lib, int min_cap) {
    if (lib->cap >= min_cap) return 0;
    size_t sz = (size_t)min_cap * sizeof(char *);

    char **t = (char **)realloc(lib->titles, sz);
    if (!t) return -1;
    lib->titles = t;

    char **p = (char **)realloc(lib->paths, sz);
    if (!p) return -1;
    lib->paths = p;

    char **ar = (char **)realloc(lib->artists, sz);
    if (!ar) return -1;
    lib->artists = ar;

    char **al = (char **)realloc(lib->albums, sz);
    if (!al) return -1;
    lib->albums = al;

    lib->cap = min_cap;
    return 0;
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

/* qsort comparator for char ** arrays sorted alphabetically (case-
 * insensitive). Used for the unique-artist / unique-album indexes. */
static int qsort_cmp_strcase(const void *a, const void *b) {
    const char *const *sa = (const char *const *)a;
    const char *const *sb = (const char *const *)b;
    return strcasecmp(*sa, *sb);
}

/*
 * Build a sorted, unique, NULL-skipping copy of `values[0..count)`.
 *
 * Allocates a new char** array (caller owns) and strdup's each unique
 * value. Returns the array via *out_arr and the count via *out_count.
 * On allocation failure, *out_arr is NULL and *out_count is 0 (degraded
 * but not fatal — the caller treats it as "no derived index").
 *
 * O(n log n + n²) worst case (one strdup per row + a linear unique
 * scan). For library sizes reachable on a 5G iPod (~10K songs) the
 * n² term is the bottleneck but still finishes well under a second.
 * If that becomes the limit, swap for a hash-set.
 */
static void build_unique_index(char **values, int count,
                               char ***out_arr, int *out_count) {
    *out_arr = NULL;
    *out_count = 0;

    if (count <= 0) return;

    /* Copy non-NULL values into a working buffer. */
    char **work = (char **)malloc((size_t)count * sizeof(char *));
    if (!work) return;
    int n = 0;
    for (int i = 0; i < count; i++) {
        if (values[i]) work[n++] = values[i];   /* shallow copy */
    }
    if (n == 0) { free(work); return; }

    qsort(work, (size_t)n, sizeof(char *), qsort_cmp_strcase);

    /* Allocate the result; we'll reallocate to exact size after dedup. */
    char **uniq = (char **)malloc((size_t)n * sizeof(char *));
    if (!uniq) { free(work); return; }
    int u = 0;
    for (int i = 0; i < n; i++) {
        if (i == 0 || strcasecmp(work[i], work[i - 1]) != 0) {
            char *dup = strdup(work[i]);
            if (!dup) {
                /* Free what we have and bail. */
                for (int k = 0; k < u; k++) free(uniq[k]);
                free(uniq);
                free(work);
                return;
            }
            uniq[u++] = dup;
        }
    }
    free(work);

    *out_arr = uniq;
    *out_count = u;
}

/*
 * Read `path` into a freshly malloc'd buffer. *out_buf and *out_len
 * are written on success. Returns 0 on success, -1 on any I/O error
 * or if the file exceeds SLURP_MAX (caller treats either as "no tags"
 * and continues).
 *
 * The cap defends against a user accidentally pointing --music at a
 * directory containing huge non-audio files (a disk image, a video).
 * Real audio files top out under a few hundred MB even for hi-res
 * FLAC; 64 MiB is well past that for the codecs we ship.
 */
#define SLURP_MAX (64u * 1024u * 1024u)

static int slurp(const char *path, void **out_buf, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return -1; }
    long n = ftell(fp);
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return -1; }
    if (n <= 0 || (size_t)n > SLURP_MAX) { fclose(fp); return -1; }
    void *buf = malloc((size_t)n);
    if (!buf) { fclose(fp); return -1; }
    if (fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        free(buf); fclose(fp); return -1;
    }
    fclose(fp);
    *out_buf = buf;
    *out_len = (size_t)n;
    return 0;
}

/*
 * Determine codec from extension and call the matching tag reader.
 * On success populates *out (zero-initialized first by the readers
 * themselves). Caller passes a NULL-or-empty *out for "no tags found".
 */
static void read_tags_for(const char *path, audio_tags_t *out) {
    memset(out, 0, sizeof(*out));

    void *bytes = NULL;
    size_t len = 0;
    if (slurp(path, &bytes, &len) != 0) return;

    const char *dot = strrchr(path, '.');
    if (dot) {
        char ext[8];
        size_t ext_len = strlen(dot + 1);
        if (ext_len < sizeof(ext)) {
            for (size_t i = 0; i < ext_len; i++)
                ext[i] = (char)tolower((unsigned char)dot[1 + i]);
            ext[ext_len] = 0;
            if (strcmp(ext, "flac") == 0)      (void)tag_flac_read(bytes, len, out);
            else if (strcmp(ext, "mp3") == 0)  (void)tag_mp3_read(bytes, len, out);
        }
    }
    free(bytes);
}

int tagcache_library_load(const char *dir) {
    if (!dir) return -1;
    DIR *d = opendir(dir);
    if (!d) return -1;

    /* Scan into a fresh staging buffer so a partial failure leaves
     * the previous LIBRARY untouched. */
    library_t staging = { NULL, NULL, NULL, NULL, 0, 0, 0,
                          NULL, 0, NULL, 0 };

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t basename_title_len;
        if (!has_audio_ext(ent->d_name, &basename_title_len)) continue;

        if (staging.cap <= staging.count) {
            int new_cap = staging.cap ? staging.cap * 2 : 32;
            if (library_reserve(&staging, new_cap) != 0) {
                library_free_contents(&staging);
                closedir(d);
                return -1;
            }
        }

        /* path = dir + '/' + name (built first so we can read tags). */
        size_t dir_len  = strlen(dir);
        size_t name_len = strlen(ent->d_name);
        size_t need     = dir_len + 1 + name_len + 1;
        char *path = (char *)malloc(need);
        if (!path) {
            library_free_contents(&staging);
            closedir(d);
            return -1;
        }
        int strip_slash = (dir_len > 0 && dir[dir_len - 1] == '/') ? 1 : 0;
        snprintf(path, need, "%s%s%s",
                 dir,
                 strip_slash ? "" : "/",
                 ent->d_name);

        /* Read tags. Failure is silent — we just fall back to filename
         * for the title and leave artist/album NULL. */
        audio_tags_t tags;
        read_tags_for(path, &tags);

        /* title = TITLE tag if present, else basename-without-ext. */
        char *title;
        if (tags.found_title) {
            title = strdup(tags.title);
        } else {
            title = (char *)malloc(basename_title_len + 1);
            if (title) {
                memcpy(title, ent->d_name, basename_title_len);
                title[basename_title_len] = 0;
            }
        }
        if (!title) {
            free(path);
            library_free_contents(&staging);
            closedir(d);
            return -1;
        }

        /* artist / album: strdup if present, NULL if not. NULL doesn't
         * count as an OOM failure since the tag itself is optional. */
        char *artist = tags.found_artist ? strdup(tags.artist) : NULL;
        char *album  = tags.found_album  ? strdup(tags.album)  : NULL;

        staging.titles [staging.count] = title;
        staging.paths  [staging.count] = path;
        staging.artists[staging.count] = artist;
        staging.albums [staging.count] = album;
        staging.count++;
    }
    closedir(d);

    /* Sort by title (case-insensitive). Build an index permutation
     * and apply it to all four parallel arrays. If any sort allocation
     * fails we keep the unsorted order — degraded but correct. */
    if (staging.count > 1) {
        int *idx = (int *)malloc((size_t)staging.count * sizeof(int));
        if (idx) {
            for (int i = 0; i < staging.count; i++) idx[i] = i;
            CMP_TITLES = staging.titles;
            qsort(idx, (size_t)staging.count, sizeof(int), qsort_cmp_idx);

            size_t sz = (size_t)staging.count * sizeof(char *);
            char **sorted_titles  = (char **)malloc(sz);
            char **sorted_paths   = (char **)malloc(sz);
            char **sorted_artists = (char **)malloc(sz);
            char **sorted_albums  = (char **)malloc(sz);
            if (sorted_titles && sorted_paths && sorted_artists && sorted_albums) {
                for (int i = 0; i < staging.count; i++) {
                    sorted_titles [i] = staging.titles [idx[i]];
                    sorted_paths  [i] = staging.paths  [idx[i]];
                    sorted_artists[i] = staging.artists[idx[i]];
                    sorted_albums [i] = staging.albums [idx[i]];
                }
                free(staging.titles);
                free(staging.paths);
                free(staging.artists);
                free(staging.albums);
                staging.titles  = sorted_titles;
                staging.paths   = sorted_paths;
                staging.artists = sorted_artists;
                staging.albums  = sorted_albums;
            } else {
                free(sorted_titles);
                free(sorted_paths);
                free(sorted_artists);
                free(sorted_albums);
            }
            free(idx);
        }
    }

    /* Build the derived indexes (sorted unique artists / albums).
     * Failures here are non-fatal — we just commit the library
     * without the index, and the Artists / Albums menus fall back to
     * synthetic data. */
    build_unique_index(staging.artists, staging.count,
                       &staging.uniq_artists, &staging.uniq_artist_count);
    build_unique_index(staging.albums, staging.count,
                       &staging.uniq_albums, &staging.uniq_album_count);

    /* Commit: drop any previous library, install staging. */
    library_clear();
    LIBRARY.titles            = staging.titles;
    LIBRARY.paths             = staging.paths;
    LIBRARY.artists           = staging.artists;
    LIBRARY.albums            = staging.albums;
    LIBRARY.count             = staging.count;
    LIBRARY.cap               = staging.cap;
    LIBRARY.loaded            = 1;
    LIBRARY.uniq_artists      = staging.uniq_artists;
    LIBRARY.uniq_artist_count = staging.uniq_artist_count;
    LIBRARY.uniq_albums       = staging.uniq_albums;
    LIBRARY.uniq_album_count  = staging.uniq_album_count;
    return LIBRARY.count;
}

const char *tagcache_song_path(int idx) {
    if (!LIBRARY.loaded) return NULL;
    if (idx < 0 || idx >= LIBRARY.count) return NULL;
    return LIBRARY.paths[idx];
}

const char *tagcache_song_artist(int idx) {
    if (!LIBRARY.loaded) return NULL;
    if (idx < 0 || idx >= LIBRARY.count) return NULL;
    return LIBRARY.artists[idx];   /* may be NULL if no ARTIST tag */
}

const char *tagcache_song_album(int idx) {
    if (!LIBRARY.loaded) return NULL;
    if (idx < 0 || idx >= LIBRARY.count) return NULL;
    return LIBRARY.albums[idx];    /* may be NULL if no ALBUM tag */
}

/* ---------- API impl --------------------------------------------- */

#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

int tagcache_artist_count(void) {
    return LIBRARY.loaded ? LIBRARY.uniq_artist_count : ARRAY_LEN(ARTISTS);
}
int tagcache_album_count(void) {
    return LIBRARY.loaded ? LIBRARY.uniq_album_count : ARRAY_LEN(ALBUMS);
}
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
    if (LIBRARY.loaded) {
        if (idx < 0 || idx >= LIBRARY.uniq_artist_count) return "(?)";
        return LIBRARY.uniq_artists[idx];
    }
    return bounded(ARTISTS, ARRAY_LEN(ARTISTS), idx);
}
const char *tagcache_album_name(int idx) {
    if (LIBRARY.loaded) {
        if (idx < 0 || idx >= LIBRARY.uniq_album_count) return "(?)";
        return LIBRARY.uniq_albums[idx];
    }
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
