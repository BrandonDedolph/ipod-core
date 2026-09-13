/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/library/playlist.c — Playlists, the read path. See playlist.h.
 *
 * Freestanding: no libc, no statics, no recursion. The two loops that
 * matter are bounded by the caller's array sizes (the directory walk stops
 * once `max` playlists are collected; the resolve loop runs over at most
 * PLAYLIST_TRACKS_MAX parsed entries), and every step below them is a call
 * into fat32.c or m3u.c, which carry their own bounds.
 */

#include "playlist.h"
#include "names.h"

#include "../lib/mem.h"

/* ---- helpers ------------------------------------------------------------ */

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static uint32_t slen_n(const char *s, uint32_t cap)
{
    uint32_t i = 0;
    while (i < cap && s[i] != '\0') {
        i++;
    }
    return i;
}

/* Does `name` end with `ext` (ASCII case-insensitive), with something before
 * it? ".m3u8" alone is not a playlist called "". */
static int ends_with_ci(const char *name, const char *ext)
{
    uint32_t nl = slen_n(name, FAT32_NAME_BYTES);
    uint32_t el = slen_n(ext, 16);
    if (nl <= el) {
        return 0;
    }
    for (uint32_t i = 0; i < el; i++) {
        if (lower(name[nl - el + i]) != lower(ext[i])) {
            return 0;
        }
    }
    return 1;
}

static int is_playlist_name(const char *name)
{
    return ends_with_ci(name, ".m3u8") || ends_with_ci(name, ".m3u");
}

/* ---- scan --------------------------------------------------------------- */

typedef struct {
    const char *want;
    uint32_t    clus;
} find_dir_t;

static int find_dir_cb(void *ud, const fat32_dirent_t *e)
{
    find_dir_t *f = (find_dir_t *)ud;
    if (e->is_dir && name_eq_ci(e->name, f->want)) {
        f->clus = e->first_clus;
        return 1;
    }
    return 0;
}

typedef struct {
    playlist_t *out;
    int         max;
    int         n;
    int         truncated;
} collect_t;

static int collect_cb(void *ud, const fat32_dirent_t *e)
{
    collect_t *c = (collect_t *)ud;
    if (e->is_dir || !is_playlist_name(e->name)) {
        return 0;
    }
    if (c->n >= c->max) {
        c->truncated = 1;             /* one more than fits: that is all we
                                       * need to know, stop reading */
        return 1;
    }
    playlist_t *p = &c->out[c->n++];
    copy_display_name(p->name, e->name, 1);
    p->clus = e->first_clus;
    p->size = e->size;
    p->hash = name_hash(p->name);
    return 0;
}

int playlist_scan(fat32_t *fs, uint32_t lib_root_clus,
                  playlist_t *out, int max,
                  uint32_t *dir_clus, int *truncated)
{
    if (!fs || !dir_clus || !truncated || (max > 0 && !out)) {
        return FAT32_EINVAL;
    }
    *dir_clus  = 0;
    *truncated = 0;

    find_dir_t f;
    f.want = PLAYLIST_DIR;
    f.clus = 0;
    int rc = fat32_readdir(fs, lib_root_clus, find_dir_cb, &f);
    if (rc != 0) {
        return rc;
    }
    if (f.clus == 0) {
        return 0;                     /* no Playlists folder: no playlists */
    }
    *dir_clus = f.clus;

    collect_t c;
    c.out       = out;
    c.max       = max;
    c.n         = 0;
    c.truncated = 0;
    rc = fat32_readdir(fs, f.clus, collect_cb, &c);
    if (rc != 0) {
        return rc;
    }
    *truncated = c.truncated;

    /* A-Z, case-insensitive, the library's own order. Insertion sort: n is
     * at most PLAYLIST_MAX and the whole struct moves so name, cluster and
     * hash travel together (as build_artists does it). */
    for (int i = 1; i < c.n; i++) {
        playlist_t v;
        memcpy(&v, &out[i], sizeof v);
        int j = i - 1;
        while (j >= 0 && title_cmp(out[j].name, v.name) > 0) {
            memcpy(&out[j + 1], &out[j], sizeof v);
            j--;
        }
        memcpy(&out[j + 1], &v, sizeof v);
    }
    return c.n;
}

/* ---- resolve ------------------------------------------------------------ */

int playlist_resolve(fat32_t *fs, const playlist_t *pl, const char *base_dir,
                     playlist_track_t *out, int max,
                     playlist_scratch_t *scr, playlist_stats_t *st)
{
    if (!st) {
        return M3U_EINVAL;
    }
    memset(st, 0, sizeof *st);
    if (!fs || !pl || !scr || (max > 0 && !out)) {
        return M3U_EINVAL;
    }

    /* The parse. Bounded by the entry array: a file with more tracks than
     * PLAYLIST_TRACKS_MAX yields the first that many and says so. */
    int rc = m3u_parse_file(fs, pl->clus, pl->size, base_dir,
                            scr->ent, PLAYLIST_TRACKS_MAX,
                            &scr->m3u, &st->m3u);
    if (rc != M3U_OK) {
        return rc;                    /* the playlist file itself failed */
    }
    st->listed    = st->m3u.count;
    st->rejected  = st->m3u.skipped_long + st->m3u.skipped_escape +
                    st->m3u.skipped_bad;
    st->truncated = st->m3u.truncated;

    /* The walk, once per entry. Every outcome but "playable file" is a
     * counter, not a return: the rows that do resolve are the playlist.
     *
     * Directory sectors are not cached below us, so a walk from the root is
     * a handful of uncached reads per entry — and playlists are
     * overwhelmingly album-grouped. Remember the last entry's folder prefix
     * and the cluster it walked to; an entry with the same prefix walks only
     * its leaf inside that folder. Same answer either way (the leaf lookup
     * IS the last step of the full walk), ~1 full walk per album instead of
     * per track. */
    scr->dir_len    = 0;
    scr->dir_failed = 0;
    int n = 0;
    for (uint32_t i = 0; i < st->m3u.count; i++) {
        if (n >= max) {
            st->truncated = 1;
            break;
        }
        const char *path = scr->ent[i].path;
        int plen = 0, cut = -1;
        for (; path[plen] != '\0' && plen <= (int)M3U_PATH_MAX; plen++) {
            if (path[plen] == '/' || path[plen] == '\\') {
                cut = plen;
            }
        }
        int same_dir = cut > 0 && scr->dir_len == cut &&
                       memcmp(scr->dir, path, (size_t)cut) == 0;
        if (same_dir && scr->dir_failed) {
            /* This folder already failed to read. Walking it again costs
             * the same ATA timeout for the same answer (up to 128 x 10 s
             * with the UI frozen — at boot, inside the resume). */
            st->io_err++;
            continue;
        }
        uint32_t parent = 0;
        if (same_dir) {
            rc = fat32_resolve_path(fs, scr->dir_clus, path + cut + 1,
                                    &scr->de, &parent);
        } else {
            rc = fat32_resolve_path(fs, fs->root_clus, path, &scr->de, &parent);
            if (cut > 0 && cut <= (int)M3U_PATH_MAX &&
                (rc == 0 || (rc != FAT32_ENOENT && rc != FAT32_EINVAL))) {
                memcpy(scr->dir, path, (size_t)cut);
                scr->dir[cut]   = '\0';
                scr->dir_len    = cut;
                scr->dir_clus   = parent;
                scr->dir_failed = (rc != 0);
            }
        }
        if (rc == FAT32_ENOENT || rc == FAT32_EINVAL) {
            st->missing++;            /* not on the disk (or not a path) */
            continue;
        }
        if (rc != 0) {
            /* A read failed: unknown, not gone. One folder that will not
             * read is remembered above and skipped cheaply; a SECOND
             * distinct failure means the disk itself is failing, and a
             * listing that cannot be trusted is not a listing: count the
             * rest unwalked and stop. */
            if (st->io_err++ > 0 && !same_dir) {
                st->io_err += st->m3u.count - i - 1;
                break;
            }
            continue;
        }
        const fat32_dirent_t *de = &scr->de;
        int fmt = de->is_dir ? -1 : classify_ext(de->name);
        if (fmt < 0) {
            st->unplayable++;
            continue;
        }
        playlist_track_t *t = &out[n++];
        copy_display_name(t->name, de->name, 1);   /* the browse row's name */
        t->clus      = de->first_clus;
        t->size      = de->size;
        t->dir_clus  = parent;
        t->file_hash = de->name_lossy ? 0 : name_hash(de->name);
        t->fmt       = (uint8_t)fmt;
    }
    return n;
}
