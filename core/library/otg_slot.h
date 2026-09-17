/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/library/otg_slot.h — the five SAVED On-The-Go playlists:
 * Music/Playlists/On-The-Go 1.m3u8 .. On-The-Go 5.m3u8.
 *
 * *** THE FOURTH PLACE IN THE FIRMWARE THAT WRITES TO THE USER'S DISK ***
 * (kernel/config.c, kernel/evlog.c, kernel/otg_store.c are the others), and
 * the only one whose file is READ BACK through fs/fat32.c's cached paths —
 * see fat32_cache_drop(), which this module calls after its last write.
 *
 * WHY M3U8 AND NOT A BINARY CONTAINER. A saved playlist is USER data, not
 * derived index (fs/m3u.h says so, and the project's playlist memo says so).
 * Making the saved lists ordinary extended M3U8 files means the Playlists
 * screen lists them, library/playlist.c opens them, `core sync --prune` keeps
 * them, a desktop player opens them, and the user can copy one off the device
 * into their own collection — all of it for free, none of it code. The price
 * is two comment lines the rest of the world ignores.
 *
 * THE FORMAT (one fixed-width header line, the entries, one fixed-width
 * trailer, newline padding to the last byte of the file):
 *
 *     #EXTM3U\n                                                      8 B
 *     #CORE-OTG v1 count=00012 crc=1A2B3C4D gen=00042\n             48 B
 *     /Music/Artist - Album/03. Title.flac\n                  <- count lines
 *     ...
 *     #CORE-OTG-END gen=00042\n                                     24 B
 *     \n\n\n...                                    <- to the end of the file
 *
 *   count  entry lines actually written, zero-padded to five decimals.
 *   crc    zlib CRC-32 of the entry lines' bytes — from the byte after the
 *          header line's \n to the \n that ends the last entry — as eight
 *          upper-case hex digits. The HOST checks it (tools/make_otg.py
 *          --dump, `core doctor`); the device does not need to.
 *   gen    the live list's mutation counter at save time, zero-padded to
 *          five decimals, written TWICE: once in the header, once in the
 *          trailer. That repetition is the whole tear-detection scheme.
 *
 * WHY THE PADDING IS NEWLINES AND NOT NUL. fs/m3u.c rejects any line holding
 * a byte below 0x20 (it counts as skipped_bad), and a NUL run would therefore
 * be reported as thousands of bad lines. A blank line is ignored, silently
 * and by specification.
 *
 * WHY THE FILE IS ALWAYS WRITTEN WHOLE. The device cannot truncate a file, so
 * saving a shorter list over a longer one must overwrite the old tail — or
 * the entries past the new trailer would still be there, and m3u.c (which
 * ignores '#' lines but not paths) would happily play them.
 *
 * TEAR DETECTION, AND THE WRITE ORDER THAT MAKES IT WORK. The file is
 * streamed through a 4096-byte staging buffer in ONE pass that formats the
 * lines and accumulates the CRC as it goes. Stage 0 — the header lines and
 * the first few entries — is HELD IN RAM; stages 1..K are written as they
 * fill, then the padding, and STAGE 0 IS WRITTEN LAST, once the final count
 * and CRC are known. So:
 *
 *   - a cut before that last write leaves the OLD stage 0 (old header gen,
 *     old first lines) on top of the NEW tail: the header gen no longer
 *     matches the trailer gen, and the file reads as DAMAGED;
 *   - a cut mid-tail leaves the old header over a mix: the trailer is
 *     missing or carries the old gen, and the parser's line count does not
 *     equal the header's count. DAMAGED again;
 *   - a completed last write is durable, because the injected write issues
 *     FLUSH CACHE and stage 0 is the final call.
 *
 * A damaged slot is never believed: it lists as "On-The-Go N" and opens to
 * "Playlist damaged — save again" with Delete Playlist under it. Checking it
 * costs no CRC pass — two small reads and the count the parser already
 * produced.
 *
 * IT IS NOT FREE FOR THE NEXT SAVE. Only an EMPTY slot (count = 0, i.e. a
 * file that is all padding after its two directive lines) is, and that is
 * load-bearing rather than a simplification: writing a new list over an old
 * one is the single case the gen/count test cannot see, because a new line
 * that happens to be exactly as long as the old one it lands on leaves a file
 * whose count still matches. Save only ever writes over padding, so that case
 * cannot arise. Delete is what recovers a damaged slot, and it rewrites the
 * file WHOLE.
 *
 * A FOREIGN FILE AT A SLOT NAME. A user may have their own
 * "On-The-Go 2.m3u8". It has no directive line, so it is not a slot: it is
 * listed and played as the ordinary playlist it is, and Save skips it. The
 * host half refuses to plan one too (`core sync` warns and renames nothing).
 */
#ifndef CORE_LIBRARY_OTG_SLOT_H
#define CORE_LIBRARY_OTG_SLOT_H

#include <stdint.h>

#include "otg.h"                  /* OTG_SLOTS, otg_write_fn, otg_crc32 */
#include "../fs/fat32.h"
#include "../fs/m3u.h"            /* M3U_PATH_MAX: what a line may cost  */
/* ATA_PHYS_LOG / ATA_SECTOR_SZ: the write quantum, as kernel/config.h takes
 * them. This is the one file under library/ that writes to the disk, so it is
 * the one that needs them; the path is relative because library/ is built
 * with no hal include directory and should not gain one for a constants
 * header with no MMIO in it. */
#include "../hal/hw/ata.h"

/*
 * The size the host creates a slot file at: 128 KiB = four stock clusters.
 *
 * The number is "the largest possible live list always fits, whole". A line
 * is '/' + the canonical path + '\n', the canonical path is capped at
 * M3U_PATH_MAX (191), so the worst case is 512 * 193 + 8 + 48 + 24 = 98896
 * bytes. Four clusters is the next whole-cluster size above that with room
 * to spare; three (96 KiB) is 592 bytes short of it, which would have meant
 * a 512-track save silently losing its last few rows to `truncated` on a
 * library with very long names.
 */
#define OTG_SLOT_FILE_BYTES 131072u

/*
 * The smallest slot file this module will write into, and the rule that goes
 * with it: a slot file's size must be at least this AND a multiple of 1024,
 * so every staged write is a whole number of physical sectors on the stock
 * drive. Anything else is "unusable": never written to, and if it holds
 * entries it is simply listed as the playlist it is.
 */
#define OTG_SLOT_FILE_MIN   4096u
#define OTG_SLOT_SIZE_GRAIN 1024u

/* The staging buffer, and therefore the size of the region held back and
 * written last. A multiple of OTG_SLOT_SIZE_GRAIN. */
#define OTG_SAVE_STAGE      4096u

/* "On-The-Go N" plus its terminator — the ext-trimmed playlist NAME, which
 * is what the Playlists list shows and what name_hash() folds for the resume
 * context. The file on disk is that plus ".m3u8". */
#define OTG_SLOT_NAME_BYTES 12u

/* The two fixed-width directive lines, including their newline. */
#define OTG_HDR_LINE_BYTES  48u
#define OTG_END_LINE_BYTES  24u

/* otg_slot_save / otg_slot_erase: the target is not a slot file — it carries
 * no `#CORE-OTG` directive, so it is somebody's own playlist (or unreadable).
 * Nothing was written. */
#define OTG_SLOT_EFOREIGN   (-5)

/*
 * Which slot `name` is: 1..OTG_SLOTS for exactly "On-The-Go N" (ASCII
 * case-insensitive, one digit, nothing before or after), 0 for anything else.
 *
 * `name` is the EXT-TRIMMED playlist name, i.e. playlist_t.name — so
 * "On-The-Go 1.m3u8" is 0, because that is not a name this list ever holds.
 *
 * A NAME IS NOT A SLOT. This answers "could this file be slot N", and a user
 * is perfectly entitled to keep their own "On-The-Go 2.m3u8". Anything that
 * is going to WRITE must ask otg_slot_of() instead, which also looks inside.
 */
int otg_slot_index(const char *name);

/* Write "On-The-Go N" into `dst` (at least OTG_SLOT_NAME_BYTES). An `n`
 * outside 1..OTG_SLOTS writes an empty string. */
void otg_slot_name(char *dst, int n);

/* What a slot file's own header (and, after otg_slot_verify, its trailer)
 * says about it. */
typedef struct {
    uint16_t count;    /* entry lines the header claims                     */
    uint16_t gen;      /* the header's gen                                  */
    uint32_t crc;      /* the header's CRC of the entry bytes (host-checked) */
    uint8_t  present;  /* 1 = a #CORE-OTG directive was found: this IS a slot
                        * file. 0 = a foreign .m3u8 sitting at a slot name   */
    uint8_t  damaged;  /* otg_slot_verify only: the trailer is missing, its
                        * gen disagrees with the header's, or the parser
                        * counted a different number of entries             */
} otg_slot_info_t;

/*
 * Read a slot file's header. Reads the FIRST 512 BYTES and nothing else, so
 * the Playlists list can classify all five slots for the price of five
 * sector reads.
 *
 * Returns 0 and fills *out on success (including "not a slot file at all",
 * which is present = 0), negative on a disk read error. `size` is the
 * directory entry's size; a file too small to hold the header is not a slot.
 */
int otg_slot_probe(fat32_t *fs, uint32_t clus, uint32_t size,
                   otg_slot_info_t *out);

/*
 * otg_slot_probe plus the trailer: the full "is this file intact" test, run
 * when a slot is OPENED. `listed` is what the parser counted
 * (playlist_stats_t.listed) — pass the number of entry lines the file
 * actually produced.
 *
 * Returns 0 and fills *out (with out->damaged set or clear), negative on a
 * disk read error. Scans forward for the trailer line, so it costs one pass
 * over the file's CONTENT (not its padding) — the same order of work as the
 * parse that just produced `listed`, and no CRC pass.
 */
int otg_slot_verify(fat32_t *fs, uint32_t clus, uint32_t size,
                    uint32_t listed, otg_slot_info_t *out);

/*
 * Which slot the FILE is — the question every writer has to ask, and the one
 * otg_slot_index() cannot answer.
 *
 * Returns 1..OTG_SLOTS only when `name` is a slot name AND the file at
 * (`clus`, `size`) carries the `#CORE-OTG` directive. 0 for everything else:
 * a name that is not a slot name, a FOREIGN playlist the user keeps at one
 * (the case this exists for), a file too small or unreadable. `out` is
 * otg_slot_verify()'s report, so a caller that wants `damaged` gets it from
 * the same two reads; `listed` is what the parser counted, as for verify.
 *
 * A 0 here means "do not write to this file". The device offering to DELETE
 * somebody's own playlist because of its name would be exactly the data loss
 * this whole feature is built to avoid.
 */
int otg_slot_of(fat32_t *fs, const char *name, uint32_t clus, uint32_t size,
                uint32_t listed, otg_slot_info_t *out);

/*
 * One row to save: where the file is. kernel/main.c fills these from g_songs
 * (dir_clus / file_clus), because the NAMES it holds are not usable here —
 * lib_song_t.file has lost its extension and the folder map's name is capped
 * at 63 characters, and a path that is not byte-exact names nothing. The
 * exact on-disk names are read back out of the directory entries instead.
 */
typedef struct {
    uint32_t dir_clus;
    uint32_t file_clus;
} otg_save_row_t;

/* What became of the rows. `written` + `unsaveable` is what was offered
 * (minus anything past `truncated`). */
typedef struct {
    uint32_t written;     /* entry lines on the disk                        */
    uint32_t unsaveable;  /* a row whose folder or file could not be named:
                           * not in the library root, a lossy long name, a
                           * path over M3U_PATH_MAX, or a name holding a byte
                           * the parser would reject                        */
    uint32_t io_err;      /* rows skipped because a directory would not read */
    uint8_t  truncated;   /* the rows outgrew the file; the rest were dropped */
    uint32_t crc;         /* the CRC written into the header                */
} otg_save_stats_t;

/*
 * Caller-owned working memory for otg_slot_save. ~9.7 KB, one instance in
 * kernel/main.c — held here rather than on the stack so its cost is visible
 * at the one place that pays it, exactly as playlist_scratch_t is.
 */
typedef struct {
    uint8_t  stage0[OTG_SAVE_STAGE];   /* the held-back first stage         */
    uint8_t  stage[OTG_SAVE_STAGE];    /* every other stage, in turn        */
    char     folder[FAT32_NAME_BYTES]; /* the cached folder's exact name    */
    char     file[FAT32_NAME_BYTES];   /* the row's exact file name         */
    uint32_t folder_clus;              /* which folder that name is for     */
    int      folder_state;             /* 0 none yet, 1 usable, 2 unusable,
                                        * 3 its directory would not read    */
    char     path[M3U_PATH_MAX + 2];   /* the line, without its newline     */
} otg_save_scratch_t;

/*
 * Overwrite slot file (`clus`, `size`) with `rows` as an extended M3U8.
 *
 * `lib_root_clus` is the folder the album folders live in (Music/, or the
 * volume root on the layout with no Music/), which is what a row's dir_clus
 * is looked up in; `root_prefix` is the matching absolute prefix, "/Music/"
 * or "/", and must end in '/'. `gen` is the live list's mutation counter.
 * `write` is the injected sector writer. `st` is filled on every return.
 *
 * REFUSES A FILE THAT IS NOT A SLOT FILE. The target must already carry a
 * `#CORE-OTG` directive, which is what the host's empty form has and what a
 * playlist of the user's own at a slot name does not. That check is here, at
 * the writer, and not only at the callers: "never write to a file the user
 * made" is the promise this module exists to keep, and a promise kept in one
 * place cannot be forgotten at a second call site.
 *
 * Returns 0 when the whole file is on the platter, OTG_SLOT_EFOREIGN when the
 * target is not a slot file, and other negatives when nothing was written (a
 * size the format cannot use, an unresolvable address, a read that failed) or
 * when a write failed part-way — in which case `st` says how far it got.
 *
 * WHAT A PART-WAY FAILURE LEAVES, AND HOW IT COMES BACK. A failure BEFORE the
 * last write leaves a file the next reader calls damaged (old header, new
 * tail); Delete Playlist rewrites it and the slot is usable again. A failure
 * inside the LAST write, the one that puts stage 0 down, can instead leave a
 * first sector with no header at all — and that file reads as FOREIGN, so
 * every writer here refuses it for ever. So does an unreadable first sector
 * (the probe fails and this returns -1). That is the correct failure
 * direction and it is deliberate, but it is also a dead end on the device:
 * the slot is listed, opens as whatever the bytes parse to, and Save skips it.
 *
 * THE RECOVERY IS ON THE HOST, and it is a human one: look at
 * `Music/Playlists/On-The-Go N.m3u8`, and if it is not something you put
 * there, DELETE IT and run `core sync` (or `tools/make_otg.py --create`),
 * which recreates an absent slot empty. `core doctor` prints exactly that on
 * the slot's line, because it is the only thing that will ever show a user
 * this state.
 *
 * It is a human one on purpose. The obvious alternative — a host command that
 * rewrites a slot file "when it is not a valid playlist" — needs a test that
 * tells a torn slot from a playlist somebody made, and no such test exists:
 * a torn slot's surviving bytes are stale ENTRY LINES that parse perfectly,
 * while a real user's playlist is perfectly entitled to be empty, or to name
 * files that are all missing. A heuristic that is wrong in both directions is
 * not something to put in front of "never overwrite a file the user made";
 * the person looking at the file is the only reliable discriminator, and the
 * host already hands them the file.
 *
 * Calls fat32_cache_drop() before returning, so the parse that follows reads
 * the bytes that were just written rather than the ones that were there
 * before.
 */
int otg_slot_save(fat32_t *fs, uint32_t clus, uint32_t size,
                  uint32_t lib_root_clus, const char *root_prefix,
                  const otg_save_row_t *rows, int n, uint16_t gen,
                  otg_write_fn write, otg_save_scratch_t *scr,
                  otg_save_stats_t *st);

/*
 * Write the EMPTY form (count = 0, crc = 0, gen = `gen`) over a slot file —
 * what "Delete Playlist" does, and the ONLY way a slot that holds something
 * (a saved list, or a torn save) becomes free again. Same writer, same
 * stats, same tear rules, and the same refusal to touch a foreign file.
 */
int otg_slot_erase(fat32_t *fs, uint32_t clus, uint32_t size, uint16_t gen,
                   otg_write_fn write, otg_save_scratch_t *scr,
                   otg_save_stats_t *st);

#endif /* CORE_LIBRARY_OTG_SLOT_H */
