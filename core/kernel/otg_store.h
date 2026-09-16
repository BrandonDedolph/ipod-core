/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/kernel/otg_store.h — the On-The-Go LIVE list on disk: COREOTG.DAT,
 * a pre-allocated file the firmware overwrites in place.
 *
 * *** THE THIRD PLACE IN THE FIRMWARE THAT WRITES TO THE USER'S DISK ***
 * (the others: kernel/config.c, the settings record; kernel/evlog.c, the
 * event log; and library/otg_slot.c, the fourth, which overwrites a saved
 * On-The-Go slot playlist.)
 *
 * Every rule in kernel/config.h's banner is copied here, not reinterpreted:
 *
 *   - The host tool creates COREOTG.DAT in the VOLUME ROOT once
 *     (tools/make_otg.py --create, or `core sync`). The device never
 *     creates, grows, shrinks, moves or deletes it — it only overwrites the
 *     bytes of slots INSIDE it. No FAT entry, no directory entry, no
 *     free-cluster search, no FSInfo is ever touched.
 *   - Two slots, alternated by a wrapping sequence number, each CRC-32
 *     protected over its whole body, so a torn or failed write can only
 *     damage the slot that is NOT the newest good one.
 *   - Every LBA is RE-RESOLVED and RE-VALIDATED before every read and every
 *     write. Nothing is cached across calls, no literal LBA appears, and the
 *     module disables itself for the session at the first thing it cannot
 *     prove. REFUSING TO SAVE IS ALWAYS THE CORRECT ANSWER WHEN IN DOUBT.
 *   - Reads go through the volume's RAW block callback, never the cached
 *     file path, so what is decoded is the platter's truth.
 *
 * WHAT IS DIFFERENT FROM config.c, and why.
 *
 *   - A SLOT IS 5120 BYTES, not one physical sector: 512 entries of 8 bytes
 *     plus a header, a reserved word and the CRC. That is bigger than a
 *     cluster on a small-cluster volume, so a slot may STRADDLE a cluster
 *     boundary — and config.c deliberately never follows a chain. This
 *     module resolves per CLUSTER RUN with fat32_file_lba_at() (evlog.c's
 *     proven primitive) and reads/writes a slot as one or more runs, each
 *     inside the cluster it resolved to, each physical-sector aligned.
 *   - THE WRITE AND THE WAKE ARE INJECTED at mount, as evlog.c injects
 *     them, so this file has no HAL dependency and the host test drives a
 *     RAM disk and records every write. kernel/main.c passes
 *     ata_write_sectors and ata_wakeup.
 *   - THE COMMIT GATE LIVES HERE, not in main.c. The live list is touched
 *     from the UI (a hold-Select add, a remove, a Clear) and committed from
 *     the same six places the settings record is: the idle pass, the
 *     Settings exit, disk mode, suspend, standby and the DISKSAFE last
 *     write. Keeping its cfg_commit_t inside the module — evlog.c's shape —
 *     is what lets tests/kernel/otg_store_test.c drive the debounce, the
 *     parked-drive refusal, the battery gate and the retry policy on the
 *     host, and leaves main.c with one line per site.
 *
 * WHAT IS *NOT* PERSISTED HERE. The five SAVED On-The-Go playlists are
 * ordinary .m3u8 files under Music/Playlists (library/otg_slot.c). This file
 * is only the live list — session state, like the resume record.
 */
#ifndef CORE_KERNEL_OTG_STORE_H
#define CORE_KERNEL_OTG_STORE_H

#include <stdint.h>

#include "hw/ata.h"              /* ATA_PHYS_LOG / ATA_SECTOR_SZ            */
#include "../fs/fat32.h"
#include "../library/otg.h"
#include "cfg_commit.h"

/* The file the host tool pre-creates in the volume ROOT (8.3 name, so it
 * matches with or without an LFN). */
#define OTG_FILE_NAME       "COREOTG.DAT"

/*
 * On-disk geometry.
 *
 * OTG_SLOT_BYTES is 5120 = 5 * 1024: a whole number of PHYSICAL sectors on
 * the stock drive (which IDNFs any sub-physical-sector access), and the
 * smallest such size that holds 512 * 8 bytes of entries plus a header and
 * the CRC. Two slots; the host creates the file at one stock cluster
 * (32 KiB) so there is room for the format to grow without a new file.
 */
#define OTG_SLOT_BYTES      5120u
#define OTG_SLOT_SECTORS    (OTG_SLOT_BYTES / ATA_SECTOR_SZ)      /* 10     */
#define OTG_SLOTS_ON_DISK   2u
#define OTG_STORE_MIN_BYTES (OTG_SLOT_BYTES * OTG_SLOTS_ON_DISK)  /* 10240  */

#define OTG_MAGIC           0x47544F43u    /* 'C''O''T''G' little-endian    */
#define OTG_VERSION         1u

/* Settle-and-retry for the boot reads, config.c's numbers and rules: a read
 * error at boot is usually the drive still spinning up, not a bad slot. */
#define OTG_READ_RETRIES    2u
#define OTG_READ_RETRY_MS   250u

/*
 * Bind to a mounted volume and load the newest valid slot into *out.
 *
 * Enumerates the volume root for OTG_FILE_NAME, resolves and validates the
 * address of slot 0 (which proves the file is usable), reads both slots
 * through `fs`'s own block callback with the settle-and-retry, validates
 * magic/version/count/CRC on each and takes the valid one with the newer
 * sequence number.
 *
 * Returns 1 when a valid slot was found and *out holds it; 0 otherwise — and
 * in that case *out is otg_init()'d, i.e. an empty list, because "no saved
 * list" and "an empty saved list" are the same thing to the user.
 *
 * A 0 return does not necessarily disable saving: a file that exists and
 * resolves but holds no valid slot (a fresh one, or both slots damaged) is
 * still writable, and writing slot 0 is how it recovers. A slot the drive
 * would not READ is different: if the other slot holds nothing valid either,
 * the module refuses to write for the session, because the unread slot may
 * hold the newest record and a save from seq 0 would lose to it at the next
 * boot. otg_store_writable() says which.
 *
 * Safe to call with null arguments (returns 0, module disabled).
 */
int otg_store_mount(fat32_t *fs, otg_write_fn write, otg_wake_fn wake,
                    otg_list_t *out);

/* 1 when otg_store_mount() found a COREOTG.DAT this module may write. */
int otg_store_writable(void);

/* The sequence number of the slot currently believed newest (0 if none). */
uint32_t otg_store_seq(void);

/*
 * Persist `l` into the slot that was NOT most recently read, with a bumped
 * sequence, then FLUSH CACHE (the injected write does that). Re-resolves and
 * re-validates every run's address on every call. Returns 0 on success,
 * negative on refusal or on the write callback's error. Bookkeeping moves
 * only on success, so a failed write leaves the previous good slot as the
 * newest and the next attempt targets the same slot again.
 *
 * Call it through otg_store_commit(); this is exposed for the host test and
 * for a caller that has already decided.
 */
int otg_store_save(const otg_list_t *l);

/*
 * DIAGNOSTIC: the absolute 512-byte LBA of slot `slot`'s FIRST run, resolved
 * fresh exactly as a write would. 0 and *lba set on success, negative with
 * *lba = 0 otherwise.
 *
 * This exists for the same one reason config_probe_lba() does: the first
 * on-device bring-up must compare the address the firmware would write with
 * the address `tools/make_otg.py --verify` computed, BEFORE any write
 * happens. A slot that straddles a cluster boundary has more runs than this
 * one address; the tool prints the whole chain, and the first run is what the
 * boot line quotes.
 */
int otg_store_probe_lba(uint32_t slot, uint32_t *lba);

/* ---- the commit gate ---------------------------------------------------- */

/* otg_store_commit() results, evlog_flush()'s shape. */
enum {
    OTG_COMMIT_NONE = 0,       /* nothing pending, not yet, or not writable  */
    OTG_COMMIT_WROTE,          /* the list is on the platter                 */
    OTG_COMMIT_DEFERRED,       /* refused by the battery gate, FIRST refusal
                                * of this episode — worth a UART line        */
    OTG_COMMIT_DEFERRED_QUIET, /* refused again, already reported            */
    OTG_COMMIT_FAILED          /* the resolve or the write failed            */
};

/*
 * The list changed at `now_us` — call this from every mutation site (add,
 * remove, clear, and after a Save empties it).
 */
void otg_store_touch(uint32_t now_us);

/* Nothing is pending. Called at boot beside cfg_commit_clear(&g_cfg_commit):
 * loading the saved list is not a change. */
void otg_store_commit_clear(void);

/* 1 while a change is waiting for a write. For the UART narration and the
 * About page. */
int otg_store_pending(void);

/*
 * Write the pending change, if the gate allows. `mode` is CFG_COMMIT_IDLE
 * (the main loop: debounced, never wakes a parked drive), CFG_COMMIT_SOFT
 * (leaving Settings: no debounce, still never wakes a parked drive),
 * CFG_COMMIT_FORCE (suspend, power-off, disk mode: pays the spin-up) or
 * CFG_COMMIT_LAST (the DISKSAFE edge's one write, exempt from the battery
 * gate). `env` is what the caller already gathered for its settings commit;
 * `env->writable` is IGNORED — this module knows whether IT is writable.
 *
 * Returns an OTG_COMMIT_* code. A failed write keeps the change pending for
 * another attempt a debounce later, and CFG_SAVE_MAX_FAILURES consecutive
 * failures drop it (cfg_commit_result's rule): the previous slot is intact,
 * and that beats hammering a drive that has refused three times.
 */
int otg_store_commit(int mode, const cfg_commit_env_t *env, const otg_list_t *l);

/* The result of the last write attempt (0 = ok, negative = the resolver's or
 * the write callback's code). For the UART line and About. */
int otg_store_last_rc(void);

/* ---- slot codec, exposed for the host tests ----------------------------- */

/*
 * Encode `l` into an OTG_SLOT_BYTES slot with sequence `seq`. `slot` must be
 * OTG_SLOT_BYTES and 16-bit aligned (the write path's data-port requirement).
 * Always succeeds; every byte not covered by an entry is zero, and the CRC
 * covers the whole slot, so the padding is deterministic.
 */
void otg_slot_encode(uint8_t *slot, const otg_list_t *l, uint32_t seq);

/*
 * Validate and decode a slot. Returns 1 and fills *l and *seq when magic,
 * version, count and CRC-32 all check out; 0 otherwise, leaving *l untouched.
 *
 * A (0, 0) pair INSIDE the declared count is dropped rather than stored — it
 * is what the padding is made of, so a hand-edited file cannot inject an
 * entry that no add could have made. The decoded count is therefore <= the
 * declared one.
 */
int otg_slot_decode(const uint8_t *slot, otg_list_t *l, uint32_t *seq);

#endif /* CORE_KERNEL_OTG_STORE_H */
