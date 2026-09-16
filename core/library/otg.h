/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/library/otg.h — the On-The-Go playlist's RAM model: the LIVE list the
 * user builds by holding Select on a song or an album.
 *
 * WHAT AN ENTRY IS. A pair of folded name hashes — (folder_hash, file_hash) —
 * and nothing else. That pair is exactly the locator CORELIB.IDX already uses
 * to bind a record to the file on the disk (the index record carries
 * folder_hash at offset 248 and file_hash at 252, and kernel/main.c's
 * resolve_art_cb binds by file_hash within the folder's cluster). Using the
 * same locator means an entry survives an index rebuild and a re-sync that
 * keeps names, and it costs EIGHT BYTES — the whole 512-entry list is one
 * 5 KiB disk slot. A stored path would cost 192 bytes an entry (200 KiB of
 * pre-allocated file for a list that is session state) and the device would
 * still have to walk directories to resolve it at every boot. The
 * host-portable path form is produced once, at Save time, by
 * library/otg_slot.c — which is where it belongs.
 *
 * WHAT THIS MODULE IS NOT. It does not bind entries to songs: g_songs and the
 * folder map live in kernel/main.c, which the host cannot compile, so the
 * binding stays there (as library/playlist.c leaves its row binding there).
 * It does not sort, de-duplicate or touch the disk. It is an ordered array
 * with a mutation counter, which is all a list the user builds by hand needs,
 * and it is pure so tests/library/otg_test.c can drive every edge of it.
 *
 * DUPLICATES ARE ALLOWED. The original iPod allows the same track twice in
 * On-The-Go, a playlist file may list one twice, and playlist_resolve()
 * already copes. Refusing them would mean the count on screen disagreed with
 * the number of times the user pressed the button, which is worse.
 *
 * ORDER IS INSERTION ORDER. Nothing re-sorts: "the order I added them" is the
 * only order a hand-built list has.
 */
#ifndef CORE_LIBRARY_OTG_H
#define CORE_LIBRARY_OTG_H

#include <stdint.h>

/*
 * Entries in the live list. 512 is the number the whole design is sized
 * around: 512 * 8 B = 4096 B of entries, which is what makes one on-disk slot
 * 5120 B (kernel/otg_store.h) and what sizes a saved slot
 * playlist's pre-allocated file (library/otg_slot.h). It is also
 * PLAYLIST_TRACKS_MAX, so a saved On-The-Go list opens WHOLE — a list you
 * cannot re-open in full is not a saved list.
 */
#define OTG_MAX   512u

/* Saved-playlist slot files: Music/Playlists/On-The-Go 1..5.m3u8. Here
 * because both halves of the feature need the number and neither owns it. */
#define OTG_SLOTS 5u

/* One entry: the folded hashes of the FULL on-disk names of the album folder
 * and of the file inside it (extension included, library/names.c name_hash).
 * A (0, 0) pair is the "no entry" value and is never stored. */
typedef struct {
    uint32_t folder_hash;
    uint32_t file_hash;
} otg_entry_t;

/*
 * The list. 4 + 4096 = 4100 bytes of .bss at the one instance kernel/main.c
 * holds.
 *
 * `gen` counts MUTATIONS, not entries. It is written into the disk slot's
 * header and into both the header and the trailer of a saved slot playlist,
 * where a header gen that disagrees with the trailer gen is precisely how a
 * torn save is detected (library/otg_slot.h). It wraps at 16 bits, which is
 * fine: nothing compares two gens for order, only for equality WITHIN one
 * file written in one pass.
 */
typedef struct {
    uint16_t    n;
    uint16_t    gen;
    otg_entry_t e[OTG_MAX];
} otg_list_t;

/* Empty the list and reset the mutation counter. The state a device with no
 * COREOTG.DAT (or an unreadable one) runs the session in. */
void otg_init(otg_list_t *l);

/*
 * Append one entry. Returns 1 when it was added (and `gen` moved), 0 when it
 * was refused — the list is full (n == OTG_MAX) or the pair is (0, 0), the
 * null locator a hand-edited slot file could otherwise inject. A refused add
 * does NOT move `gen`: nothing changed, so nothing on disk is stale.
 */
int otg_add(otg_list_t *l, uint32_t folder_hash, uint32_t file_hash);

/*
 * Append up to `n` entries in order — one album's tracks, in disc/track
 * order, is the caller. Returns how many were actually added, which is fewer
 * than `n` when the list fills up (the caller says "Added 7 of 12") and 0
 * when it was already full. Null pairs are skipped as otg_add skips them, and
 * `gen` moves once for the whole call, not once per entry.
 */
int otg_add_many(otg_list_t *l, const otg_entry_t *e, int n);

/*
 * Remove the entry at `idx`, closing the gap so the order of the rest is
 * kept. Returns 1 when something was removed, 0 for an index outside
 * [0, n).
 */
int otg_remove(otg_list_t *l, int idx);

/* Empty the list. `gen` moves only when there was something to empty. */
void otg_clear(otg_list_t *l);

/*
 * zlib's CRC-32 (reflected, polynomial 0xEDB88320, init/final 0xFFFFFFFF) —
 * the same one kernel/config.c, kernel/evlog.c and the host tools compute,
 * which is what lets tools/make_otg.py and `core sync` write bytes this
 * firmware accepts.
 *
 * It lives HERE, in the feature's shared base, because BOTH of On-The-Go's
 * on-disk formats need it and neither may include the other:
 * kernel/otg_store.c CRCs a COREOTG.DAT slot, library/otg_slot.c CRCs the
 * entry lines of a saved slot playlist, and library/ must not pull in
 * kernel/'s HAL-dependent headers. A second copy of a checksum is a second
 * chance to get a checksum subtly wrong.
 *
 * Bitwise, no 1 KB lookup table: it runs twice at boot and once per save,
 * over a few kilobytes. The table would cost more RAM than the record.
 *
 * otg_crc32_update() is the same computation left RUNNING, for a caller that
 * checksums bytes it is formatting one line at a time and never holds whole:
 * seed it with OTG_CRC32_INIT, feed it each piece, and invert the result
 * (~crc) to get what otg_crc32() would have returned over the concatenation.
 */
#define OTG_CRC32_INIT 0xFFFFFFFFu
uint32_t otg_crc32_update(uint32_t crc, const uint8_t *p, uint32_t n);
uint32_t otg_crc32(const uint8_t *p, uint32_t n);

/*
 * The two hardware hooks BOTH On-The-Go writers take, injected by the caller
 * so neither file has a HAL dependency and the host tests can drive a RAM
 * disk and record every write. kernel/main.c passes ata_write_sectors and
 * ata_wakeup; kernel/evlog.c's pair is the precedent.
 *
 * They are declared here, in the shared base, so kernel/otg_store.h and
 * library/otg_slot.h name ONE type rather than two structurally identical
 * ones — library/ must not include kernel/'s HAL-dependent headers, and a
 * second typedef is a second thing to keep in step.
 *
 * `write` writes `count` 512-byte sectors at absolute 512-byte LBA `lba`;
 * both must be multiples of the drive's logical-per-physical ratio and the
 * buffer must be 16-bit aligned (hal/hw/ata.h). 0 on success, negative
 * otherwise. `wake` spins a parked drive up on the read path; 0 on success.
 */
typedef int (*otg_write_fn)(uint32_t lba, uint32_t count, const void *buf);
typedef int (*otg_wake_fn)(void);

#endif /* CORE_LIBRARY_OTG_H */
