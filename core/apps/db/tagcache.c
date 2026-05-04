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
#include "tagcache_format.h"

#include "../../codecs/dr_flac/tag_flac.h"
#include "../../codecs/dr_mp3/tag_mp3.h"
#include "../../codecs/tags.h"

#include <ctype.h>
#include <dirent.h>
#include <stddef.h>
#include <stdint.h>
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
    char    **titles;       /* tag TITLE if present, else basename-without-ext */
    char    **paths;        /* full filesystem path */
    char    **artists;      /* tag ARTIST   or NULL if missing */
    char    **albums;       /* tag ALBUM    or NULL if missing */
    char    **genres;       /* tag GENRE    or NULL if missing */
    char    **composers;    /* tag COMPOSER or NULL if missing */
    void    **art_bytes;    /* heap-owned JPEG bytes, or NULL if no embedded art */
    size_t   *art_lens;     /* parallel array: byte counts (0 if no art) */
    int    count;
    int    cap;
    int    loaded;

    /* Derived indexes built after the scan: sorted unique sets of
     * the artists / albums / genres / composers seen in the songs list.
     * Used by the Artists / Albums / Genres / Composers menus when a
     * library is loaded. Songs without a given tag are skipped (a song
     * with no ARTIST tag doesn't contribute a "(?)" entry to the
     * artist list). */
    char **uniq_artists;
    int    uniq_artist_count;
    char **uniq_albums;
    int    uniq_album_count;
    char **uniq_genres;
    int    uniq_genre_count;
    char **uniq_composers;
    int    uniq_composer_count;

    /* Per-song lookup: which uniq_* does song i belong to? -1 means
     * the song had no tag for that dimension and doesn't appear in
     * any group's drilldown list. */
    int   *song_artist_idx;
    int   *song_album_idx;
    int   *song_genre_idx;
    int   *song_composer_idx;

    /* Filtered song lists, precomputed at library_load. For artist a,
     * `artist_songs[a]` is a heap array of song indices whose ARTIST
     * matches uniq_artists[a], in the same alphabetical-by-title order
     * as the global library. Same shape for the other three dimensions.
     *
     * Two parallel arrays per group (the int* per-group array + a
     * count). NULL/0 if a given group is empty (no songs match). */
    int  **artist_songs;
    int   *artist_song_counts;
    int  **album_songs;
    int   *album_song_counts;
    int  **genre_songs;
    int   *genre_song_counts;
    int  **composer_songs;
    int   *composer_song_counts;
} library_t;

/* Designated initializer keeps this readable as the struct grows. */
static library_t LIBRARY = { 0 };

static void library_clear(void) {
    for (int i = 0; i < LIBRARY.count; i++) {
        free(LIBRARY.titles[i]);
        free(LIBRARY.paths[i]);
        free(LIBRARY.artists[i]);
        free(LIBRARY.albums[i]);
        free(LIBRARY.genres[i]);
        free(LIBRARY.composers[i]);
        free(LIBRARY.art_bytes[i]);
    }
    free(LIBRARY.titles);
    free(LIBRARY.paths);
    free(LIBRARY.artists);
    free(LIBRARY.albums);
    free(LIBRARY.genres);
    free(LIBRARY.composers);
    free(LIBRARY.art_bytes);
    free(LIBRARY.art_lens);

    for (int i = 0; i < LIBRARY.uniq_artist_count;   i++) free(LIBRARY.uniq_artists  [i]);
    for (int i = 0; i < LIBRARY.uniq_album_count;    i++) free(LIBRARY.uniq_albums   [i]);
    for (int i = 0; i < LIBRARY.uniq_genre_count;    i++) free(LIBRARY.uniq_genres   [i]);
    for (int i = 0; i < LIBRARY.uniq_composer_count; i++) free(LIBRARY.uniq_composers[i]);
    free(LIBRARY.uniq_artists);
    free(LIBRARY.uniq_albums);
    free(LIBRARY.uniq_genres);
    free(LIBRARY.uniq_composers);

    free(LIBRARY.song_artist_idx);
    free(LIBRARY.song_album_idx);
    free(LIBRARY.song_genre_idx);
    free(LIBRARY.song_composer_idx);

    if (LIBRARY.artist_songs) {
        for (int i = 0; i < LIBRARY.uniq_artist_count; i++) free(LIBRARY.artist_songs[i]);
        free(LIBRARY.artist_songs);
    }
    free(LIBRARY.artist_song_counts);
    if (LIBRARY.album_songs) {
        for (int i = 0; i < LIBRARY.uniq_album_count; i++) free(LIBRARY.album_songs[i]);
        free(LIBRARY.album_songs);
    }
    free(LIBRARY.album_song_counts);
    if (LIBRARY.genre_songs) {
        for (int i = 0; i < LIBRARY.uniq_genre_count; i++) free(LIBRARY.genre_songs[i]);
        free(LIBRARY.genre_songs);
    }
    free(LIBRARY.genre_song_counts);
    if (LIBRARY.composer_songs) {
        for (int i = 0; i < LIBRARY.uniq_composer_count; i++) free(LIBRARY.composer_songs[i]);
        free(LIBRARY.composer_songs);
    }
    free(LIBRARY.composer_song_counts);

    /* Re-zero everything via the same designated-initializer pattern
     * used at static-init time, so the field list stays in one place. */
    LIBRARY = (library_t){ 0 };
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
        free(lib->genres[i]);
        free(lib->composers[i]);
        free(lib->art_bytes[i]);
    }
    free(lib->titles);
    free(lib->paths);
    free(lib->artists);
    free(lib->albums);
    free(lib->genres);
    free(lib->composers);
    free(lib->art_bytes);
    free(lib->art_lens);
    for (int i = 0; i < lib->uniq_artist_count;   i++) free(lib->uniq_artists  [i]);
    for (int i = 0; i < lib->uniq_album_count;    i++) free(lib->uniq_albums   [i]);
    for (int i = 0; i < lib->uniq_genre_count;    i++) free(lib->uniq_genres   [i]);
    for (int i = 0; i < lib->uniq_composer_count; i++) free(lib->uniq_composers[i]);
    free(lib->uniq_artists);
    free(lib->uniq_albums);
    free(lib->uniq_genres);
    free(lib->uniq_composers);
    free(lib->song_artist_idx);
    free(lib->song_album_idx);
    free(lib->song_genre_idx);
    free(lib->song_composer_idx);
    if (lib->artist_songs) {
        for (int i = 0; i < lib->uniq_artist_count; i++) free(lib->artist_songs[i]);
        free(lib->artist_songs);
    }
    free(lib->artist_song_counts);
    if (lib->album_songs) {
        for (int i = 0; i < lib->uniq_album_count; i++) free(lib->album_songs[i]);
        free(lib->album_songs);
    }
    free(lib->album_song_counts);
    if (lib->genre_songs) {
        for (int i = 0; i < lib->uniq_genre_count; i++) free(lib->genre_songs[i]);
        free(lib->genre_songs);
    }
    free(lib->genre_song_counts);
    if (lib->composer_songs) {
        for (int i = 0; i < lib->uniq_composer_count; i++) free(lib->composer_songs[i]);
        free(lib->composer_songs);
    }
    free(lib->composer_song_counts);
}

/*
 * Grow `lib`'s capacity to at least `min_cap`. Each realloc is
 * committed back to lib before the next is attempted, so a failure
 * never leaves a freed-old + orphaned-new pair behind. Returns 0 on
 * success, -1 on allocation failure.
 */
static int library_reserve(library_t *lib, int min_cap) {
    if (lib->cap >= min_cap) return 0;
    size_t sz_p  = (size_t)min_cap * sizeof(char *);
    size_t sz_v  = (size_t)min_cap * sizeof(void *);
    size_t sz_sz = (size_t)min_cap * sizeof(size_t);

    char **t = (char **)realloc(lib->titles, sz_p);
    if (!t) return -1;
    lib->titles = t;

    char **p = (char **)realloc(lib->paths, sz_p);
    if (!p) return -1;
    lib->paths = p;

    char **ar = (char **)realloc(lib->artists, sz_p);
    if (!ar) return -1;
    lib->artists = ar;

    char **al = (char **)realloc(lib->albums, sz_p);
    if (!al) return -1;
    lib->albums = al;

    char **gn = (char **)realloc(lib->genres, sz_p);
    if (!gn) return -1;
    lib->genres = gn;

    char **cm = (char **)realloc(lib->composers, sz_p);
    if (!cm) return -1;
    lib->composers = cm;

    void **ab = (void **)realloc(lib->art_bytes, sz_v);
    if (!ab) return -1;
    lib->art_bytes = ab;

    size_t *aln = (size_t *)realloc(lib->art_lens, sz_sz);
    if (!aln) return -1;
    lib->art_lens = aln;

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
 * O(n log n) — sort dominates; dedup is a single linear pass. Result
 * array is sized at the upper bound (`n` slots) and may be slightly
 * over-allocated when there are duplicates; for library sizes reachable
 * on a 5G iPod (~10K songs) the slack is negligible.
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

    /* Result is sized at n (upper bound). Acceptable slack — see
     * the function header on library-size assumptions. */
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
 * Find `needle` in `hay[0..n)` (case-insensitive). Returns the index,
 * or -1 if not present. Linear — small n (artist/album counts are tens
 * to a few thousand even on full libraries; binary search wouldn't pay
 * for the extra code).
 */
static int find_in_uniq(char **hay, int n, const char *needle) {
    if (!needle) return -1;
    for (int i = 0; i < n; i++) {
        if (strcasecmp(hay[i], needle) == 0) return i;
    }
    return -1;
}

/*
 * Build per-group song lists for one dimension (artist / album / etc).
 * `song_count` is the global library song count, `song_group_idx` is
 * the per-song -> uniq-idx table for this dimension, `uniq_count` is
 * the size of the uniq table. *out_lists / *out_counts receive the
 * group_count-sized parallel arrays (or stay NULL on alloc failure).
 */
static void build_group_song_lists(int song_count,
                                   const int *song_group_idx,
                                   int uniq_count,
                                   int ***out_lists,
                                   int **out_counts) {
    if (uniq_count <= 0) return;
    int **lists  = (int **)calloc((size_t)uniq_count, sizeof(int *));
    int  *counts = (int  *)calloc((size_t)uniq_count, sizeof(int));
    *out_lists  = lists;
    *out_counts = counts;
    if (!lists || !counts) return;
    for (int a = 0; a < uniq_count; a++) {
        int n = 0;
        for (int s = 0; s < song_count; s++)
            if (song_group_idx[s] == a) n++;
        if (n == 0) continue;
        int *list = (int *)malloc((size_t)n * sizeof(int));
        if (!list) continue;
        int j = 0;
        for (int s = 0; s < song_count; s++)
            if (song_group_idx[s] == a) list[j++] = s;
        lists[a]  = list;
        counts[a] = n;
    }
}

/*
 * Build the per-song uniq-idx tables and per-group song lists. Any
 * allocation failure leaves the affected structure NULL — caller
 * commits regardless. See library_t comments for the data layout.
 */
static void build_filter_indexes(library_t *lib) {
    if (lib->count <= 0) return;

    /* Per-song -> uniq lookup. All-or-nothing: if any of the four
     * tables fail to allocate, abandon every per-song idx and let the
     * downstream drilldown queries return 0 rows. The library still
     * commits — the user just can't drill into it. */
    lib->song_artist_idx   = (int *)malloc((size_t)lib->count * sizeof(int));
    lib->song_album_idx    = (int *)malloc((size_t)lib->count * sizeof(int));
    lib->song_genre_idx    = (int *)malloc((size_t)lib->count * sizeof(int));
    lib->song_composer_idx = (int *)malloc((size_t)lib->count * sizeof(int));
    if (!lib->song_artist_idx || !lib->song_album_idx ||
        !lib->song_genre_idx  || !lib->song_composer_idx) {
        free(lib->song_artist_idx);   lib->song_artist_idx   = NULL;
        free(lib->song_album_idx);    lib->song_album_idx    = NULL;
        free(lib->song_genre_idx);    lib->song_genre_idx    = NULL;
        free(lib->song_composer_idx); lib->song_composer_idx = NULL;
        return;
    }
    for (int s = 0; s < lib->count; s++) {
        lib->song_artist_idx  [s] = find_in_uniq(lib->uniq_artists,
                                                 lib->uniq_artist_count,
                                                 lib->artists[s]);
        lib->song_album_idx   [s] = find_in_uniq(lib->uniq_albums,
                                                 lib->uniq_album_count,
                                                 lib->albums[s]);
        lib->song_genre_idx   [s] = find_in_uniq(lib->uniq_genres,
                                                 lib->uniq_genre_count,
                                                 lib->genres[s]);
        lib->song_composer_idx[s] = find_in_uniq(lib->uniq_composers,
                                                 lib->uniq_composer_count,
                                                 lib->composers[s]);
    }

    /* Per-group song lists, in the song-array's existing alphabetical-
     * by-title order. Two passes per group inside the helper: count
     * first, then fill. Allocation failure leaves the per-group entry
     * NULL; downstream queries return 0 for that group.
     *
     * Asymmetric-NULL between *_songs and *_song_counts (one calloc'd,
     * the other NULL) is intentionally tolerated — the accessors below
     * explicitly NULL-check both before use. */
    build_group_song_lists(lib->count, lib->song_artist_idx,
                           lib->uniq_artist_count,
                           &lib->artist_songs, &lib->artist_song_counts);
    build_group_song_lists(lib->count, lib->song_album_idx,
                           lib->uniq_album_count,
                           &lib->album_songs, &lib->album_song_counts);
    build_group_song_lists(lib->count, lib->song_genre_idx,
                           lib->uniq_genre_count,
                           &lib->genre_songs, &lib->genre_song_counts);
    build_group_song_lists(lib->count, lib->song_composer_idx,
                           lib->uniq_composer_count,
                           &lib->composer_songs, &lib->composer_song_counts);
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
    library_t staging = { 0 };

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
            /* Half-row in flight: `path` has been malloc'd but not
             * stored in staging yet (staging.count not bumped until
             * the row is fully built below). Free it explicitly,
             * then library_free_contents drops the rest. */
            free(path);
            library_free_contents(&staging);
            closedir(d);
            return -1;
        }

        /* artist / album / genre / composer: strdup if present, NULL
         * if not. A NULL strdup result doesn't fail the load — a tag
         * is optional, and silently dropping one missing field beats
         * aborting the whole scan over an OOM on a non-essential
         * string. */
        char *artist   = tags.found_artist   ? strdup(tags.artist)   : NULL;
        char *album    = tags.found_album    ? strdup(tags.album)    : NULL;
        char *genre    = tags.found_genre    ? strdup(tags.genre)    : NULL;
        char *composer = tags.found_composer ? strdup(tags.composer) : NULL;

        /* art: transfer ownership of the heap copy that the tag
         * reader produced. Set tags.art_bytes = NULL so audio_tags_free
         * (called below) doesn't double-free. */
        void   *art_bytes = tags.art_bytes;
        size_t  art_len   = tags.art_len;
        tags.art_bytes = NULL;
        tags.art_len   = 0;

        staging.titles   [staging.count] = title;
        staging.paths    [staging.count] = path;
        staging.artists  [staging.count] = artist;
        staging.albums   [staging.count] = album;
        staging.genres   [staging.count] = genre;
        staging.composers[staging.count] = composer;
        staging.art_bytes[staging.count] = art_bytes;
        staging.art_lens [staging.count] = art_len;
        staging.count++;

        audio_tags_free(&tags);   /* releases nothing (art moved out) */
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

            size_t sz_p  = (size_t)staging.count * sizeof(char *);
            size_t sz_v  = (size_t)staging.count * sizeof(void *);
            size_t sz_sz = (size_t)staging.count * sizeof(size_t);
            char    **sorted_titles    = (char **)malloc(sz_p);
            char    **sorted_paths     = (char **)malloc(sz_p);
            char    **sorted_artists   = (char **)malloc(sz_p);
            char    **sorted_albums    = (char **)malloc(sz_p);
            char    **sorted_genres    = (char **)malloc(sz_p);
            char    **sorted_composers = (char **)malloc(sz_p);
            void    **sorted_art_bytes = (void **)malloc(sz_v);
            size_t   *sorted_art_lens  = (size_t *)malloc(sz_sz);
            if (sorted_titles && sorted_paths && sorted_artists &&
                sorted_albums && sorted_genres && sorted_composers &&
                sorted_art_bytes && sorted_art_lens) {
                for (int i = 0; i < staging.count; i++) {
                    sorted_titles    [i] = staging.titles    [idx[i]];
                    sorted_paths     [i] = staging.paths     [idx[i]];
                    sorted_artists   [i] = staging.artists   [idx[i]];
                    sorted_albums    [i] = staging.albums    [idx[i]];
                    sorted_genres    [i] = staging.genres    [idx[i]];
                    sorted_composers [i] = staging.composers [idx[i]];
                    sorted_art_bytes [i] = staging.art_bytes [idx[i]];
                    sorted_art_lens  [i] = staging.art_lens  [idx[i]];
                }
                free(staging.titles);
                free(staging.paths);
                free(staging.artists);
                free(staging.albums);
                free(staging.genres);
                free(staging.composers);
                free(staging.art_bytes);
                free(staging.art_lens);
                staging.titles    = sorted_titles;
                staging.paths     = sorted_paths;
                staging.artists   = sorted_artists;
                staging.albums    = sorted_albums;
                staging.genres    = sorted_genres;
                staging.composers = sorted_composers;
                staging.art_bytes = sorted_art_bytes;
                staging.art_lens  = sorted_art_lens;
            } else {
                free(sorted_titles);
                free(sorted_paths);
                free(sorted_artists);
                free(sorted_albums);
                free(sorted_genres);
                free(sorted_composers);
                free(sorted_art_bytes);
                free(sorted_art_lens);
            }
            free(idx);
        }
    }

    /* Build the derived indexes (sorted unique artists / albums).
     * Failures here are non-fatal — we just commit the library
     * without the index, and the Artists / Albums menus fall back to
     * synthetic data. */
    build_unique_index(staging.artists,   staging.count,
                       &staging.uniq_artists,   &staging.uniq_artist_count);
    build_unique_index(staging.albums,    staging.count,
                       &staging.uniq_albums,    &staging.uniq_album_count);
    build_unique_index(staging.genres,    staging.count,
                       &staging.uniq_genres,    &staging.uniq_genre_count);
    build_unique_index(staging.composers, staging.count,
                       &staging.uniq_composers, &staging.uniq_composer_count);

    /* Build the per-song lookup tables and the per-group song lists.
     * All optional — any malloc failure here just leaves the affected
     * structure NULL, and the corresponding drilldown query returns 0
     * rows. The library still commits, the user just doesn't get
     * drilldown for that group. */
    build_filter_indexes(&staging);

    /* Commit: drop any previous library, install staging. Wholesale
     * struct copy (instead of field-by-field) so adding a new field
     * to library_t doesn't require remembering to add a line here. */
    library_clear();
    LIBRARY = staging;
    LIBRARY.loaded = 1;
    return LIBRARY.count;
}

/* ---------- Binary tagcache (.tcdb) parser ----------------------- */

/* Little-endian readers. The on-disk format is LE; iPod ARM is also
 * LE, but we go byte-wise so the same code works on any host endian
 * (the meson host build runs on whatever the dev box is). */
static uint32_t tcdb_le32(const uint8_t *p) {
    return (uint32_t)p[0]        | ((uint32_t)p[1] <<  8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int32_t tcdb_le32s(const uint8_t *p) {
    return (int32_t)tcdb_le32(p);
}
static uint64_t tcdb_le64(const uint8_t *p) {
    return (uint64_t)tcdb_le32(p) | ((uint64_t)tcdb_le32(p + 4) << 32);
}

/*
 * Resolve string at `off` (byte offset within the strings section)
 * into a freshly malloc'd null-terminated copy. Returns NULL on OOM
 * or when off is out of bounds. The caller owns the returned pointer.
 */
static char *tcdb_strdup_at(const uint8_t *strings, uint64_t strings_len,
                            uint32_t off) {
    if ((uint64_t)off >= strings_len) return NULL;
    /* The strings section is null-terminated UTF-8; find the end
     * within the section bounds (a malformed file with a missing
     * terminator would otherwise read past). */
    size_t end = off;
    while (end < strings_len && strings[end] != 0) end++;
    size_t len = end - off;
    char *dup = (char *)malloc(len + 1);
    if (!dup) return NULL;
    memcpy(dup, strings + off, len);
    dup[len] = 0;
    return dup;
}

/*
 * Validate that section [off, off+length) lies entirely within
 * [0, file_len). Defends against an adversarial header where
 * off+length wraps around the u64 range.
 */
static int tcdb_section_in_bounds(uint64_t off, uint64_t length, uint64_t file_len) {
    uint64_t end = off + length;
    if (end < off) return 0;          /* overflow */
    if (end > file_len) return 0;
    return 1;
}

/*
 * Parse a .tcdb buffer into `staging`. Returns 0 on success. On
 * failure, `staging` may be partially populated; the caller MUST
 * call library_free_contents(staging) and discard.
 */
static int tcdb_parse(const void *bytes, size_t len, library_t *staging) {
    if (!bytes || len < TCDB_HEADER_SIZE) return -1;
    const uint8_t *p = (const uint8_t *)bytes;

    /* Magic + version. */
    if (p[TCDB_OFF_MAGIC + 0] != TCDB_MAGIC0 ||
        p[TCDB_OFF_MAGIC + 1] != TCDB_MAGIC1 ||
        p[TCDB_OFF_MAGIC + 2] != TCDB_MAGIC2 ||
        p[TCDB_OFF_MAGIC + 3] != TCDB_MAGIC3) return -1;
    if (tcdb_le32(p + TCDB_OFF_VERSION) != TCDB_VERSION) return -1;

    uint32_t song_count  = tcdb_le32(p + TCDB_OFF_SONG_COUNT);
    uint32_t n_artists   = tcdb_le32(p + TCDB_OFF_N_ARTISTS);
    uint32_t n_albums    = tcdb_le32(p + TCDB_OFF_N_ALBUMS);
    uint32_t n_genres    = tcdb_le32(p + TCDB_OFF_N_GENRES);
    uint32_t n_composers = tcdb_le32(p + TCDB_OFF_N_COMPOSERS);

    uint64_t songs_off          = tcdb_le64(p + TCDB_OFF_SONGS_OFF);
    uint64_t artist_idx_off     = tcdb_le64(p + TCDB_OFF_ARTIST_IDX_OFF);
    uint64_t album_idx_off      = tcdb_le64(p + TCDB_OFF_ALBUM_IDX_OFF);
    uint64_t genre_idx_off      = tcdb_le64(p + TCDB_OFF_GENRE_IDX_OFF);
    uint64_t composer_idx_off   = tcdb_le64(p + TCDB_OFF_COMPOSER_IDX_OFF);
    uint64_t artist_groups_off  = tcdb_le64(p + TCDB_OFF_ARTIST_GROUPS_OFF);
    uint64_t album_groups_off   = tcdb_le64(p + TCDB_OFF_ALBUM_GROUPS_OFF);
    uint64_t genre_groups_off   = tcdb_le64(p + TCDB_OFF_GENRE_GROUPS_OFF);
    uint64_t composer_groups_off= tcdb_le64(p + TCDB_OFF_COMPOSER_GROUPS_OFF);
    uint64_t strings_off        = tcdb_le64(p + TCDB_OFF_STRINGS_OFF);
    uint64_t strings_len        = tcdb_le64(p + TCDB_OFF_STRINGS_LEN);
    uint64_t art_off            = tcdb_le64(p + TCDB_OFF_ART_OFF);
    uint64_t art_len            = tcdb_le64(p + TCDB_OFF_ART_LEN);

    /* Bounds-check every fixed-size section against file length. The
     * group blocks have variable bodies; we only check their offset
     * tables here and verify body extents during the read. */
    if (!tcdb_section_in_bounds(songs_off,        (uint64_t)song_count * TCDB_SONG_RECORD_SIZE, len) ||
        !tcdb_section_in_bounds(artist_idx_off,   (uint64_t)n_artists   * 4, len) ||
        !tcdb_section_in_bounds(album_idx_off,    (uint64_t)n_albums    * 4, len) ||
        !tcdb_section_in_bounds(genre_idx_off,    (uint64_t)n_genres    * 4, len) ||
        !tcdb_section_in_bounds(composer_idx_off, (uint64_t)n_composers * 4, len) ||
        !tcdb_section_in_bounds(artist_groups_off,   (uint64_t)n_artists   * 4, len) ||
        !tcdb_section_in_bounds(album_groups_off,    (uint64_t)n_albums    * 4, len) ||
        !tcdb_section_in_bounds(genre_groups_off,    (uint64_t)n_genres    * 4, len) ||
        !tcdb_section_in_bounds(composer_groups_off, (uint64_t)n_composers * 4, len) ||
        !tcdb_section_in_bounds(strings_off, strings_len, len) ||
        !tcdb_section_in_bounds(art_off,     art_len,     len)) {
        return -1;
    }

    const uint8_t *strings = p + strings_off;

    /* Reserve the per-song parallel arrays. library_reserve uses
     * realloc, so the new slots come back uninitialized. We MUST
     * zero them before setting staging->count, because any later
     * allocation failure in this function bubbles up via
     * library_free_contents which walks [0..count) and free()s
     * every per-song pointer — undefined behavior on uninitialized
     * memory. The 8 parallel arrays are: titles, paths, artists,
     * albums, genres, composers, art_bytes, art_lens. */
    if (song_count > 0) {
        if (library_reserve(staging, (int)song_count) != 0) return -1;
        size_t pn = (size_t)song_count;
        memset(staging->titles,    0, pn * sizeof(char *));
        memset(staging->paths,     0, pn * sizeof(char *));
        memset(staging->artists,   0, pn * sizeof(char *));
        memset(staging->albums,    0, pn * sizeof(char *));
        memset(staging->genres,    0, pn * sizeof(char *));
        memset(staging->composers, 0, pn * sizeof(char *));
        memset(staging->art_bytes, 0, pn * sizeof(void *));
        memset(staging->art_lens,  0, pn * sizeof(size_t));
    }
    staging->count = (int)song_count;

    /* Read uniq tables. We need them up-front so we can populate the
     * per-song artist/album/genre/composer string fields by indexing. */
    char **uniq_artists   = NULL;
    char **uniq_albums    = NULL;
    char **uniq_genres    = NULL;
    char **uniq_composers = NULL;
    if (n_artists > 0) {
        uniq_artists = (char **)calloc(n_artists, sizeof(char *));
        if (!uniq_artists) return -1;
        for (uint32_t i = 0; i < n_artists; i++) {
            uint32_t off = tcdb_le32(p + artist_idx_off + (uint64_t)i * 4);
            uniq_artists[i] = tcdb_strdup_at(strings, strings_len, off);
            if (!uniq_artists[i]) { staging->uniq_artists = uniq_artists;
                                    staging->uniq_artist_count = (int)i; return -1; }
        }
    }
    staging->uniq_artists = uniq_artists;
    staging->uniq_artist_count = (int)n_artists;

    if (n_albums > 0) {
        uniq_albums = (char **)calloc(n_albums, sizeof(char *));
        if (!uniq_albums) return -1;
        for (uint32_t i = 0; i < n_albums; i++) {
            uint32_t off = tcdb_le32(p + album_idx_off + (uint64_t)i * 4);
            uniq_albums[i] = tcdb_strdup_at(strings, strings_len, off);
            if (!uniq_albums[i]) { staging->uniq_albums = uniq_albums;
                                   staging->uniq_album_count = (int)i; return -1; }
        }
    }
    staging->uniq_albums = uniq_albums;
    staging->uniq_album_count = (int)n_albums;

    if (n_genres > 0) {
        uniq_genres = (char **)calloc(n_genres, sizeof(char *));
        if (!uniq_genres) return -1;
        for (uint32_t i = 0; i < n_genres; i++) {
            uint32_t off = tcdb_le32(p + genre_idx_off + (uint64_t)i * 4);
            uniq_genres[i] = tcdb_strdup_at(strings, strings_len, off);
            if (!uniq_genres[i]) { staging->uniq_genres = uniq_genres;
                                   staging->uniq_genre_count = (int)i; return -1; }
        }
    }
    staging->uniq_genres = uniq_genres;
    staging->uniq_genre_count = (int)n_genres;

    if (n_composers > 0) {
        uniq_composers = (char **)calloc(n_composers, sizeof(char *));
        if (!uniq_composers) return -1;
        for (uint32_t i = 0; i < n_composers; i++) {
            uint32_t off = tcdb_le32(p + composer_idx_off + (uint64_t)i * 4);
            uniq_composers[i] = tcdb_strdup_at(strings, strings_len, off);
            if (!uniq_composers[i]) { staging->uniq_composers = uniq_composers;
                                      staging->uniq_composer_count = (int)i; return -1; }
        }
    }
    staging->uniq_composers = uniq_composers;
    staging->uniq_composer_count = (int)n_composers;

    /* Per-song lookup arrays. Allocate up front so the read loop
     * below can fill them; partial-allocation failure leaves them
     * NULL and library_free_contents handles cleanup. */
    if (song_count > 0) {
        staging->song_artist_idx   = (int *)malloc((size_t)song_count * sizeof(int));
        staging->song_album_idx    = (int *)malloc((size_t)song_count * sizeof(int));
        staging->song_genre_idx    = (int *)malloc((size_t)song_count * sizeof(int));
        staging->song_composer_idx = (int *)malloc((size_t)song_count * sizeof(int));
        if (!staging->song_artist_idx || !staging->song_album_idx ||
            !staging->song_genre_idx  || !staging->song_composer_idx) return -1;
    }

    /* Read song records. */
    for (uint32_t i = 0; i < song_count; i++) {
        const uint8_t *rec = p + songs_off + (uint64_t)i * TCDB_SONG_RECORD_SIZE;
        uint32_t title_off    = tcdb_le32 (rec + TCDB_REC_TITLE_OFF);
        uint32_t path_off     = tcdb_le32 (rec + TCDB_REC_PATH_OFF);
        int32_t  artist_idx   = tcdb_le32s(rec + TCDB_REC_ARTIST_IDX);
        int32_t  album_idx    = tcdb_le32s(rec + TCDB_REC_ALBUM_IDX);
        int32_t  genre_idx    = tcdb_le32s(rec + TCDB_REC_GENRE_IDX);
        int32_t  composer_idx = tcdb_le32s(rec + TCDB_REC_COMPOSER_IDX);
        uint64_t rec_art_off  = tcdb_le64 (rec + TCDB_REC_ART_OFF);
        uint64_t rec_art_len  = tcdb_le64 (rec + TCDB_REC_ART_LEN);

        staging->titles[i] = tcdb_strdup_at(strings, strings_len, title_off);
        staging->paths [i] = tcdb_strdup_at(strings, strings_len, path_off);
        if (!staging->titles[i] || !staging->paths[i]) return -1;

        /* artist/album/genre/composer: derive from uniq tables when
         * the per-song idx is non-negative; NULL when missing. We
         * dup the uniq entry rather than alias it so library_clear's
         * per-song free() doesn't double-free the uniq table. */
        staging->artists  [i] = (artist_idx   >= 0 && artist_idx   < (int)n_artists)
                                ? strdup(uniq_artists  [artist_idx])   : NULL;
        staging->albums   [i] = (album_idx    >= 0 && album_idx    < (int)n_albums)
                                ? strdup(uniq_albums   [album_idx])    : NULL;
        staging->genres   [i] = (genre_idx    >= 0 && genre_idx    < (int)n_genres)
                                ? strdup(uniq_genres   [genre_idx])    : NULL;
        staging->composers[i] = (composer_idx >= 0 && composer_idx < (int)n_composers)
                                ? strdup(uniq_composers[composer_idx]) : NULL;

        staging->song_artist_idx  [i] = (artist_idx   >= 0 && artist_idx   < (int)n_artists)   ? artist_idx   : -1;
        staging->song_album_idx   [i] = (album_idx    >= 0 && album_idx    < (int)n_albums)    ? album_idx    : -1;
        staging->song_genre_idx   [i] = (genre_idx    >= 0 && genre_idx    < (int)n_genres)    ? genre_idx    : -1;
        staging->song_composer_idx[i] = (composer_idx >= 0 && composer_idx < (int)n_composers) ? composer_idx : -1;

        /* Art: rec_art_off is relative to header.art_off. We bounds-
         * check against the art section's stated length (not the
         * whole file) so an adversarial header with rec_art_off
         * pointing outside the art region — but inside the file —
         * is rejected. The art section itself was already validated
         * to be in-file; verifying inside-art is sufficient. The
         * compound check uses subtraction (`rec_art_off > art_len -
         * rec_art_len`) instead of addition so we never compute a
         * sum that could wrap u64. */
        if (rec_art_len > 0) {
            if (rec_art_len > art_len) return -1;
            if (rec_art_off > art_len - rec_art_len) return -1;
            void *copy = malloc((size_t)rec_art_len);
            if (!copy) return -1;
            memcpy(copy, p + art_off + rec_art_off, (size_t)rec_art_len);
            staging->art_bytes[i] = copy;
            staging->art_lens [i] = (size_t)rec_art_len;
        }
        /* else: art_bytes/art_lens already zeroed via memset above. */
    }

    /* Per-group song lists. Each block starts with N u32 offsets
     * (relative to the block's start), then [u32 count, count u32
     * indices] tuples. Layout matches Go's tagcache.encodeGroups. */
    struct group_dim {
        uint64_t block_off;
        uint32_t n;
        int    ***out_lists;     /* &staging->artist_songs etc */
        int    **out_counts;     /* &staging->artist_song_counts */
    };
    struct group_dim dims[4] = {
        { artist_groups_off,   n_artists,   &staging->artist_songs,   &staging->artist_song_counts   },
        { album_groups_off,    n_albums,    &staging->album_songs,    &staging->album_song_counts    },
        { genre_groups_off,    n_genres,    &staging->genre_songs,    &staging->genre_song_counts    },
        { composer_groups_off, n_composers, &staging->composer_songs, &staging->composer_song_counts },
    };
    for (int d = 0; d < 4; d++) {
        if (dims[d].n == 0) continue;
        int **lists  = (int **)calloc(dims[d].n, sizeof(int *));
        int  *counts = (int  *)calloc(dims[d].n, sizeof(int));
        if (!lists || !counts) { free(lists); free(counts); return -1; }
        *dims[d].out_lists  = lists;
        *dims[d].out_counts = counts;

        for (uint32_t g = 0; g < dims[d].n; g++) {
            uint32_t group_off = tcdb_le32(p + dims[d].block_off + (uint64_t)g * 4);
            uint64_t body_off  = dims[d].block_off + group_off;
            /* Each group body is: u32 count + count*u32 indices. */
            if (!tcdb_section_in_bounds(body_off, 4, len)) return -1;
            uint32_t count = tcdb_le32(p + body_off);
            if (!tcdb_section_in_bounds(body_off + 4, (uint64_t)count * 4, len)) return -1;
            if (count == 0) continue;
            int *idx = (int *)malloc((size_t)count * sizeof(int));
            if (!idx) return -1;
            for (uint32_t k = 0; k < count; k++) {
                uint32_t s = tcdb_le32(p + body_off + 4 + (uint64_t)k * 4);
                /* Reject group song-indices that escape the song
                 * array. The drilldown accessors trust list[n] and
                 * dereference LIBRARY.titles[s] without re-bounding,
                 * so an invalid s here would OOB-read at draw time. */
                if (s >= song_count) { free(idx); return -1; }
                idx[k] = (int)s;
            }
            lists[g]  = idx;
            counts[g] = (int)count;
        }
    }

    return 0;
}

/*
 * Slurp `path` into a malloc'd buffer. Returns 0 on success and
 * writes *out_buf + *out_len; -1 on any error (open, seek, OOM,
 * short read). Caller frees *out_buf on success.
 */
static int tcdb_slurp(const char *path, void **out_buf, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return -1; }
    long n = ftell(fp);
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return -1; }
    if (n <= 0) { fclose(fp); return -1; }
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

int tagcache_library_load_tcdb(const char *path) {
    if (!path) return -1;

    void *bytes = NULL;
    size_t len = 0;
    if (tcdb_slurp(path, &bytes, &len) != 0) return -1;

    library_t staging = { 0 };
    int rc = tcdb_parse(bytes, len, &staging);
    free(bytes);   /* strings + art are heap-copied; safe to drop the buffer */

    if (rc != 0) {
        library_free_contents(&staging);
        return -1;
    }

    library_clear();
    LIBRARY = staging;
    LIBRARY.loaded = 1;
    return LIBRARY.count;
}

int tagcache_library_loaded(void) {
    return LIBRARY.loaded;
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

const void *tagcache_song_art_bytes(int idx, size_t *out_len) {
    if (out_len) *out_len = 0;
    if (!LIBRARY.loaded) return NULL;
    if (idx < 0 || idx >= LIBRARY.count) return NULL;
    if (!LIBRARY.art_bytes) return NULL;
    void *bytes = LIBRARY.art_bytes[idx];
    if (!bytes) return NULL;
    if (out_len) *out_len = LIBRARY.art_lens[idx];
    return bytes;
}

/* ---------- Filtered queries ---------------------------------------- */

/*
 * Resolve the (group_idx, n) pair into a global song index. Returns
 * -1 when no library is loaded, the group is out of range, the
 * filter table never built, or n is out of range. Centralizes the
 * bounds checks that all six accessors below would otherwise repeat.
 */
static int song_idx_in_artist(int artist_idx, int n) {
    if (!LIBRARY.loaded) return -1;
    if (artist_idx < 0 || artist_idx >= LIBRARY.uniq_artist_count) return -1;
    if (!LIBRARY.artist_songs) return -1;
    if (n < 0 || n >= LIBRARY.artist_song_counts[artist_idx]) return -1;
    int *list = LIBRARY.artist_songs[artist_idx];
    if (!list) return -1;
    return list[n];
}

static int song_idx_in_album(int album_idx, int n) {
    if (!LIBRARY.loaded) return -1;
    if (album_idx < 0 || album_idx >= LIBRARY.uniq_album_count) return -1;
    if (!LIBRARY.album_songs) return -1;
    if (n < 0 || n >= LIBRARY.album_song_counts[album_idx]) return -1;
    int *list = LIBRARY.album_songs[album_idx];
    if (!list) return -1;
    return list[n];
}

int tagcache_song_count_for_artist(int artist_idx) {
    if (!LIBRARY.loaded) return 0;
    if (artist_idx < 0 || artist_idx >= LIBRARY.uniq_artist_count) return 0;
    if (!LIBRARY.artist_song_counts) return 0;
    return LIBRARY.artist_song_counts[artist_idx];
}

const char *tagcache_song_title_for_artist(int artist_idx, int n) {
    int s = song_idx_in_artist(artist_idx, n);
    return (s < 0) ? "(?)" : LIBRARY.titles[s];
}

const char *tagcache_song_path_for_artist(int artist_idx, int n) {
    int s = song_idx_in_artist(artist_idx, n);
    return (s < 0) ? NULL : LIBRARY.paths[s];
}

int tagcache_song_count_for_album(int album_idx) {
    if (!LIBRARY.loaded) return 0;
    if (album_idx < 0 || album_idx >= LIBRARY.uniq_album_count) return 0;
    if (!LIBRARY.album_song_counts) return 0;
    return LIBRARY.album_song_counts[album_idx];
}

const char *tagcache_song_title_for_album(int album_idx, int n) {
    int s = song_idx_in_album(album_idx, n);
    return (s < 0) ? "(?)" : LIBRARY.titles[s];
}

const char *tagcache_song_path_for_album(int album_idx, int n) {
    int s = song_idx_in_album(album_idx, n);
    return (s < 0) ? NULL : LIBRARY.paths[s];
}

int tagcache_song_index_for_artist(int artist_idx, int n) {
    return song_idx_in_artist(artist_idx, n);
}
int tagcache_song_index_for_album(int album_idx, int n) {
    return song_idx_in_album(album_idx, n);
}

/* Genre / composer drilldown — same shape as the artist/album helpers. */
static int song_idx_in_genre(int genre_idx, int n) {
    if (!LIBRARY.loaded) return -1;
    if (genre_idx < 0 || genre_idx >= LIBRARY.uniq_genre_count) return -1;
    if (!LIBRARY.genre_songs) return -1;
    if (n < 0 || n >= LIBRARY.genre_song_counts[genre_idx]) return -1;
    int *list = LIBRARY.genre_songs[genre_idx];
    if (!list) return -1;
    return list[n];
}

static int song_idx_in_composer(int composer_idx, int n) {
    if (!LIBRARY.loaded) return -1;
    if (composer_idx < 0 || composer_idx >= LIBRARY.uniq_composer_count) return -1;
    if (!LIBRARY.composer_songs) return -1;
    if (n < 0 || n >= LIBRARY.composer_song_counts[composer_idx]) return -1;
    int *list = LIBRARY.composer_songs[composer_idx];
    if (!list) return -1;
    return list[n];
}

int tagcache_song_count_for_genre(int genre_idx) {
    if (!LIBRARY.loaded) return 0;
    if (genre_idx < 0 || genre_idx >= LIBRARY.uniq_genre_count) return 0;
    if (!LIBRARY.genre_song_counts) return 0;
    return LIBRARY.genre_song_counts[genre_idx];
}
const char *tagcache_song_title_for_genre(int genre_idx, int n) {
    int s = song_idx_in_genre(genre_idx, n);
    return (s < 0) ? "(?)" : LIBRARY.titles[s];
}
const char *tagcache_song_path_for_genre(int genre_idx, int n) {
    int s = song_idx_in_genre(genre_idx, n);
    return (s < 0) ? NULL : LIBRARY.paths[s];
}
int tagcache_song_index_for_genre(int genre_idx, int n) {
    return song_idx_in_genre(genre_idx, n);
}

int tagcache_song_count_for_composer(int composer_idx) {
    if (!LIBRARY.loaded) return 0;
    if (composer_idx < 0 || composer_idx >= LIBRARY.uniq_composer_count) return 0;
    if (!LIBRARY.composer_song_counts) return 0;
    return LIBRARY.composer_song_counts[composer_idx];
}
const char *tagcache_song_title_for_composer(int composer_idx, int n) {
    int s = song_idx_in_composer(composer_idx, n);
    return (s < 0) ? "(?)" : LIBRARY.titles[s];
}
const char *tagcache_song_path_for_composer(int composer_idx, int n) {
    int s = song_idx_in_composer(composer_idx, n);
    return (s < 0) ? NULL : LIBRARY.paths[s];
}
int tagcache_song_index_for_composer(int composer_idx, int n) {
    return song_idx_in_composer(composer_idx, n);
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
int tagcache_genre_count(void) {
    return LIBRARY.loaded ? LIBRARY.uniq_genre_count : ARRAY_LEN(GENRES);
}
int tagcache_composer_count(void) {
    return LIBRARY.loaded ? LIBRARY.uniq_composer_count : ARRAY_LEN(COMPOSERS);
}

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
    if (LIBRARY.loaded) {
        if (idx < 0 || idx >= LIBRARY.uniq_genre_count) return "(?)";
        return LIBRARY.uniq_genres[idx];
    }
    return bounded(GENRES, ARRAY_LEN(GENRES), idx);
}
const char *tagcache_composer_name(int idx) {
    if (LIBRARY.loaded) {
        if (idx < 0 || idx >= LIBRARY.uniq_composer_count) return "(?)";
        return LIBRARY.uniq_composers[idx];
    }
    return bounded(COMPOSERS, ARRAY_LEN(COMPOSERS), idx);
}
