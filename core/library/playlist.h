/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/library/playlist.h — Playlists, the READ path: find the .m3u8 files
 * on the volume and turn one of them into rows the player can be handed.
 *
 * WHERE PLAYLISTS LIVE. `<library root>/Playlists/NAME.m3u8` (`.m3u` accepted
 * too) — i.e. Music/Playlists/ on a device laid out the documented way, or
 * Playlists/ at the volume root on the back-compat layout with no Music/
 * folder. Under the library root, not the FAT root, for the same reason
 * the albums are: the root stays the boot ROM's and Apple's, and everything
 * the importer manages is under one folder. A file's own name, extension
 * trimmed, is the playlist's name on screen; nothing inside the file names
 * it. Entries may be volume-root-absolute ("/Music/Artist - Album/01.flac")
 * or relative to the playlist's own folder ("../Artist - Album/01.flac"):
 * fs/m3u.c canonicalises both to a root-relative path before this module
 * sees them.
 *
 * WHAT THIS MODULE IS. The glue between two things that already exist and
 * are already tested — the M3U8 parser (fs/m3u.c) and the FAT32 path walk
 * (fat32_resolve_path) — plus the one decision that is the library's:
 * which rows are PLAYABLE, named exactly as a browse row would name them,
 * and carrying the locator (file_hash) that binds a row to its CORELIB.IDX
 * record. It does NOT bind to the library itself: g_songs lives in
 * kernel/main.c, which the host cannot compile, so this module hands back
 * (dir_clus, file cluster, file_hash) and main.c does the lookup with the
 * same test resolve_art_cb applies. It does NOT touch the player.
 *
 * WHY A SEPARATE FILE. Same reason as idx.c and names.c: this is the code
 * that reads a file some other machine wrote and decides what the device
 * will do about it, and it must be testable against a synthetic volume on
 * the host (tests/library/playlist_test.c) rather than by flashing.
 *
 * BOUNDS. Every cap is a fixed number and every overflow is REPORTED:
 *   PLAYLIST_MAX          playlists listed (the first that many in directory
 *                         order; *truncated says there were more)
 *   PLAYLIST_TRACKS_MAX   rows per playlist — BROWSE_MAX, because the rows
 *                         are what a browse listing is and what
 *                         player_play_queue() takes. A longer file parses to
 *                         its first 128 resolvable-or-not entries and
 *                         stats.truncated says so.
 * Nothing recurses (m3u.c does not follow nested playlists; the path walk is
 * a loop), nothing allocates: the working memory is one caller-owned
 * playlist_scratch_t (~35 KB — the parser's scratch, the entry array, one
 * dirent), so its cost is visible at the one call site that holds it.
 */
#ifndef CORE_LIBRARY_PLAYLIST_H
#define CORE_LIBRARY_PLAYLIST_H

#include <stdint.h>

#include "../fs/fat32.h"
#include "../fs/m3u.h"
#include "../player/player.h"          /* NAME_MAX, BROWSE_MAX */

#define PLAYLIST_DIR        "Playlists"
#define PLAYLIST_MAX        64
#define PLAYLIST_TRACKS_MAX BROWSE_MAX

/* One playlist file, as the Playlists list shows it. */
typedef struct {
    char     name[NAME_MAX + 1];  /* filename, extension trimmed, UTF-8 —
                                   * copy_display_name(), like every row     */
    uint32_t clus, size;          /* the .m3u8 file itself                   */
    uint32_t hash;                /* name_hash(name): the resume context key */
} playlist_t;

/*
 * One resolved, playable row. `name` is copy_display_name(on-disk name, 1):
 * the same bytes a browse row / queue entry / lib_song_t.file holds for the
 * file, so it hashes to the same resume locator. `file_hash` is name_hash of
 * the FULL on-disk name (extension included) — the CORELIB.IDX record<->file
 * locator, 0 when the entry's long name was lossy (unmatchable, as
 * resolve_art_cb treats it). `dir_clus` is the folder the file was found in,
 * which is lib_song_t.dir_clus for a track the index knows.
 */
typedef struct {
    char     name[NAME_MAX + 1];
    uint32_t clus, size;
    uint32_t dir_clus;
    uint32_t file_hash;
    uint8_t  fmt;                 /* classify_ext(): 0 FLAC, 1 MP3            */
} playlist_track_t;

/* What became of the file's entries. Counts, so the UI can say "3 missing"
 * instead of showing a shorter list and letting the user wonder. */
typedef struct {
    uint32_t listed;      /* entries the parser produced                     */
    uint32_t missing;     /* paths that name nothing on the disk (ENOENT)    */
    uint32_t unplayable;  /* resolved to a folder, or a non-audio extension  */
    uint32_t io_err;      /* resolves that hit a disk/corruption error (once
                           * one read fails, the rest of the list is counted
                           * here unwalked: a failing disk is not paid per row) */
    uint32_t rejected;    /* lines the parser refused (too long, escaped the
                           * root, URL/control bytes) — listed but unusable    */
    uint8_t  truncated;   /* the file has more entries than the row cap      */
    m3u_result_t m3u;     /* the parser's own report, for the curious        */
} playlist_stats_t;

/* Caller-owned working memory for playlist_resolve; contents on entry are
 * irrelevant, nothing persists across calls. */
typedef struct {
    m3u_scratch_t  m3u;
    m3u_entry_t    ent[PLAYLIST_TRACKS_MAX];
    fat32_dirent_t de;
    char           dir[M3U_PATH_MAX + 1];  /* last entry's folder prefix ... */
    uint32_t       dir_clus;               /* ... and the cluster it walked to */
    int            dir_len;                /* prefix length, 0 = nothing cached */
    int            dir_failed;             /* the cached prefix would not read */
} playlist_scratch_t;

/*
 * List the playlists: the .m3u8 / .m3u FILES directly inside
 * <lib_root>/Playlists, sorted A-Z by name (title_cmp). Subfolders and other
 * files are ignored. Returns the count (0 when there is no Playlists folder,
 * which is not an error: *dir_clus is 0 then), or a negative FAT32_* code
 * when the library root or the folder could not be read — a listing that
 * cannot be trusted is not a listing (see fat32_readdir). On success
 * *dir_clus is the folder's cluster and *truncated is 1 when more than `max`
 * playlists were there (the first `max` in directory order were kept).
 */
int playlist_scan(fat32_t *fs, uint32_t lib_root_clus,
                  playlist_t *out, int max,
                  uint32_t *dir_clus, int *truncated);

/*
 * Parse `pl` and resolve every entry to a directory entry on the volume,
 * keeping the playable ones as rows in `out` (at most `max`, in file order).
 * `base_dir` is the playlist folder as a canonical root-relative path
 * ("Music/Playlists", or "Playlists" on the no-Music/ layout): what a
 * relative entry is relative to. Returns the row count, or a negative
 * M3U_* / FAT32_* code when the playlist FILE itself could not be read. A
 * missing or unplayable entry is skipped and counted in *st, never fatal —
 * a playlist that lost a track to a re-import still plays the rest. `st` is
 * filled on every return.
 */
int playlist_resolve(fat32_t *fs, const playlist_t *pl, const char *base_dir,
                     playlist_track_t *out, int max,
                     playlist_scratch_t *scr, playlist_stats_t *st);

#endif /* CORE_LIBRARY_PLAYLIST_H */
