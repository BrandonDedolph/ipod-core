/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/kernel/evlog.h — the on-disk EVENT LOG: what the UART said, kept.
 *
 * The firmware narrates itself over the dock UART (`core: ...` lines — boot
 * phases, the 5 s battery sample, suspend/standby steps, disk park and
 * wake, player opens and errors). On the bench that is the whole debug
 * story; in a pocket nobody is listening, and "it was dead in the morning"
 * arrives with no evidence. This module keeps the last 8 KiB of that
 * narration in a RAM ring — every byte uart_putc()/uart_put_hex32() sends
 * is also handed to evlog_capture() — and writes it, one 2048-byte block at
 * a time, into a ring inside CORELOG.BIN, a file the host pre-allocates with
 * tools/make_log.py. The bytes on the wire are unchanged; this only listens.
 *
 * *** THE SECOND CALLER OF THE DISK WRITE PATH. ***
 *
 * kernel/config.c was the only thing that wrote to the user's disk, and its
 * banner says what any second caller owes: the same qualification (resolve
 * the LBA on the host and on the device before the first write, write, read
 * back raw, compare, `chkdsk`/fsck read-only afterwards) and the same
 * refusals. This module keeps every one of config.c's rules and adds none
 * of its own risks:
 *
 *   - NEVER creates, extends, truncates, moves or deletes the file. The host
 *     tool makes it; the device only overwrites whole blocks inside it. No
 *     FAT entry, no directory entry, no FSInfo is ever written.
 *   - Every block is a whole number of PHYSICAL sectors (2048 B = 4 LBAs =
 *     2 physical sectors on the stock drive, which IDNFs anything smaller),
 *     at an aligned LBA. Misaligned is refused, not bounced.
 *   - The LBA of every block is RE-RESOLVED before every write through the
 *     file's own cluster chain (fat32_file_lba_at: bounded walk, every hop
 *     validated, an offset past the chain refused) and re-checked here —
 *     never LBA 0, never at or below the partition start, always inside the
 *     cluster it resolved to. Nothing is cached across calls.
 *   - Block 0 is the header and is NEVER written by the device. It is read
 *     and validated at mount (magic, version, block size, block count
 *     against the directory entry's size, CRC); if anything is off the
 *     log is silently OFF for the session and About says so.
 *   - The write goes through THE SAME GATE as the settings save
 *     (cfg_commit_gate): never below the battery's disk-safe line except
 *     for the one CFG_COMMIT_LAST write at the DISKSAFE edge; an idle flush
 *     is debounced and NEVER wakes a parked drive (stricter than the
 *     settings gate: a log is not worth a spin-up); a forced flush wakes
 *     the drive first through ata_wakeup() so the spin-up is paid on the
 *     read path, as the settings save learned to.
 *   - At most ONE block per call, so the main loop never stalls on more
 *     than one 2 KB write + FLUSH.
 *   - Three consecutive failed writes turn the log off for the session.
 *
 * BLOCK FORMAT (tools/make_log.py is the host half; byte for byte):
 *
 *   header block 0:  magic "CLOG", u16 version 1, u16 block size 2048,
 *                    u32 block count (incl. this block), u32 file id,
 *                    u32 CRC-32 over the first 16 bytes
 *   ring block:      magic "CLOB", u32 seq, u16 boot id, u16 len (bits
 *                    0..14 = text bytes, bit 15 = FINAL: a forced flush),
 *                    u32 CRC-32 over the block minus this field, then
 *                    2032 bytes of text
 *   placement:       block 1 + (seq % (block_count - 1)); seq is monotonic
 *                    across boots and starts at 0 in a fresh file
 *
 * FINDING THE WRITE CURSOR AT BOOT. The header cannot hold it (block 0 is
 * never rewritten) and scanning every block is 4 MB of reads. The ring is
 * log-structured: with s0 = seq of ring slot 0, the predicate "slot r is
 * valid and its seq >= s0" is true for slots 0..k (this lap) and false
 * after (last lap's older seqs, or never written), so a binary search over
 * the slot index finds k — the newest block — in ceil(log2(slots)) reads:
 * eleven for the 2047-slot default. Unwritten slots are zero (no magic);
 * a torn write sits exactly where the search expects the first false and
 * is simply written again (a torn slot 0 after a wrap is handled by
 * anchoring on slot 1, where the previous lap is intact). A slot the
 * drive will not READ during the search is not a false — it is unknown —
 * and the log stays off for the session rather than guess a cursor and
 * overwrite the newest blocks.
 *
 * WHAT THE DUMP GIVES YOU. tools/make_log.py --dump orders the blocks by
 * seq and prints the text with boot boundaries; the boot id is the
 * previous newest block's + 1, and whether that block was FINAL says
 * whether the previous session ended through a graceful path (suspend,
 * standby, the low-battery last write) or just stopped.
 */
#ifndef CORE_KERNEL_EVLOG_H
#define CORE_KERNEL_EVLOG_H

#include <stdint.h>

#include "hw/ata.h"          /* ATA_PHYS_LOG / ATA_SECTOR_SZ: the write quantum */
#include "../fs/fat32.h"
#include "cfg_commit.h"      /* CFG_COMMIT_* modes and cfg_commit_env_t */

#define EVLOG_FILE_NAME     "CORELOG.BIN"

#define EVLOG_BLOCK_BYTES   2048u
#define EVLOG_BLOCK_SECTORS (EVLOG_BLOCK_BYTES / ATA_SECTOR_SZ)      /* 4      */
#define EVLOG_HDR_BYTES     16u
#define EVLOG_TEXT_BYTES    (EVLOG_BLOCK_BYTES - EVLOG_HDR_BYTES)    /* 2032   */
#define EVLOG_RING_BYTES    16384u  /* RAM capture ring; a power of two. 8 KiB
                                     * dropped bytes on the device during a
                                     * paused/parked stretch (no flush) */
#define EVLOG_MIN_BLOCKS    2u      /* header + one ring slot                 */
/* A FORCED or LAST flush drains the ring: as many blocks as it holds, in one
 * call, while the drive is up for the settings write anyway. Bounded by
 * what the ring can hold plus the drop marker's spill. DEVICE 2026-09-24:
 * the final block used to carry the OLDEST 2 KiB of a 16 KiB ring, so the
 * `suspend: entering` / `standby: entering` lines — the newest text, the
 * one thing a session's tail needed to say — were lost on every power-off
 * whose ring was full; the log could not tell a sleep from a crash. */
#define EVLOG_FORCE_MAX_BLOCKS ((EVLOG_RING_BYTES / EVLOG_TEXT_BYTES) + 2u)  /* 10 */
#define EVLOG_MAX_BLOCKS    65536u  /* ceiling on a header's count (128 MiB) */
#define EVLOG_VERSION       1u
#define EVLOG_LEN_MASK      0x7FFFu
#define EVLOG_LEN_FINAL     0x8000u

/* Boot reads (root walk, header, the scan) ride the same settle-and-retry
 * as config.c's: a read error at boot is usually the drive still spinning
 * up, not a bad block. */
#define EVLOG_READ_RETRIES  2u
#define EVLOG_READ_RETRY_MS 250u

/* Consecutive failed block writes before the log turns itself off. */
#define EVLOG_MAX_FAILURES  3u

/* The two hardware hooks, injected at mount so this file has no HAL
 * dependency and the host test can watch every write land in a RAM disk.
 * main.c passes ata_write_sectors and ata_wakeup. */
typedef int (*evlog_write_fn)(uint32_t lba, uint32_t count, const void *buf);
typedef int (*evlog_wake_fn)(void);

/*
 * THE TAP. Called by hal/hw/uart.c for every byte it is asked to send (the
 * character, before the '\n' -> "\r\n" expansion; each hex digit of
 * uart_put_hex32). Appends to the RAM ring, dropping the OLDEST byte when
 * full and counting the drop; the next block written carries a marker line
 * saying how many were lost. Never blocks, never touches hardware, safe
 * before evlog_mount() (the ring simply fills). uart.c carries a weak no-op
 * definition so the driver links without this module.
 */
void evlog_capture(uint8_t b);

/*
 * Bind to a mounted volume: locate EVLOG_FILE_NAME in the root, validate
 * the header block against the directory entry, and find the write cursor
 * (see the header comment). Returns 1 if the log is ON — every later
 * evlog_flush() may write — and 0 if it is OFF for the session (file absent,
 * header invalid, size mismatch, a block unreadable, an unresolvable or
 * misaligned address). Safe to call with null arguments (returns 0, OFF).
 */
int evlog_mount(fat32_t *fs, evlog_write_fn write, evlog_wake_fn wake);

/* 1 while the log is ON. */
int evlog_enabled(void);

/* The sequence number the NEXT block will carry: the count of blocks ever
 * written to this file (as far as the ring still shows). 0 in a fresh file. */
uint32_t evlog_seq(void);

/* This boot's id (previous newest block's boot + 1; 1 in a fresh file). */
uint16_t evlog_boot_id(void);

/* What the newest block at mount said about the previous session:
 * 1 = it was FINAL (a graceful end), 0 = it was not (the session just
 * stopped: crash, battery pull, reset), -1 = there was no block. */
int evlog_prev_final(void);

/* Bytes captured and not yet on disk; bytes dropped from the ring since
 * the last block was built (reported in that block's marker line). */
uint32_t evlog_pending(void);
uint32_t evlog_dropped(void);

/* Result of the last write attempt (0 = ok, negative = the write callback's
 * or the resolver's code), and how many writes have failed in a row. */
int      evlog_last_rc(void);
uint32_t evlog_failures(void);

/* evlog_flush() results. */
enum {
    EVLOG_FLUSH_NONE = 0,     /* nothing to do, not yet, or the log is OFF   */
    EVLOG_FLUSH_WROTE,        /* one block is on the platter                 */
    EVLOG_FLUSH_DEFERRED,     /* refused by the battery gate (stays pending):
                               * the FIRST refusal of this episode — worth a
                               * UART line                                   */
    EVLOG_FLUSH_DEFERRED_QUIET, /* refused again, already reported           */
    EVLOG_FLUSH_FAILED,       /* the resolve or the write failed; see
                               * evlog_last_rc(); the bytes stay pending     */
};

/*
 * Write at most ONE block. `mode` is CFG_COMMIT_IDLE (the main loop: only a
 * FULL block, only after the debounce, only if the drive is already up and
 * the battery allows), CFG_COMMIT_FORCE (suspend entry, standby: whatever
 * is pending, partial block included, waking the drive first if it is
 * parked, still refused below the disk-safe line) or CFG_COMMIT_LAST (the
 * DISKSAFE edge's last write: exempt from the battery gate). `env` is what
 * the caller gathered for its settings commit — now_us, parked,
 * player_active, battery_ok; `writable` is ignored (this module knows
 * whether IT is writable). A forced or last flush marks its blocks FINAL
 * and DRAINS the ring — up to EVLOG_FORCE_MAX_BLOCKS in one call, so the
 * newest lines (the ones that say why the device is going down) land; an
 * idle flush writes at most one block. Returns an EVLOG_FLUSH_* code for
 * the call: WROTE if at least one block landed, FAILED if a write failed
 * (what landed before it stays landed).
 */
int evlog_flush(int mode, const cfg_commit_env_t *env);

/*
 * DIAGNOSTIC: the absolute LBA the block for sequence `seq` resolves to
 * right now (fresh, exactly as a write would), without writing. 0 and
 * *lba set on success, negative with *lba = 0 otherwise. Printed at boot
 * for block 0 and for the next block, so the first on-device bring-up can
 * check them against tools/make_log.py --verify before any flush.
 */
int evlog_probe_lba(uint32_t seq, uint32_t *lba);

/* The same for block 0, the header — the address tools/make_log.py --verify
 * prints first. Read-only; the device never writes it. */
int evlog_probe_header_lba(uint32_t *lba);

/* ---- format codec, exposed for the host tests --------------------------- */

/* zlib's CRC-32, bitwise (the same one config.c and make_log.py use). */
uint32_t evlog_crc32(const uint8_t *p, uint32_t n);

/* Ring slot of `seq` as a BLOCK INDEX (1 .. count-1). `count` >= 2. */
uint32_t evlog_block_index(uint32_t seq, uint32_t count);

/* Header block 0: encode (test convenience — the device never writes one)
 * and decode. Decode returns 1 and fills *count / *file_id when magic,
 * version, block size, count range and CRC all check out; 0 otherwise. */
void evlog_header_encode(uint8_t *blk, uint32_t count, uint32_t file_id);
int  evlog_header_decode(const uint8_t *blk, uint32_t *count, uint32_t *file_id);

/* Ring block: `blk` is EVLOG_BLOCK_BYTES, 16-bit aligned. Encode copies
 * `len` (<= EVLOG_TEXT_BYTES) text bytes and zero-pads. Decode returns 1
 * and fills the out-params (text is read in place at blk + EVLOG_HDR_BYTES)
 * when magic, length and CRC check out; 0 otherwise. */
void evlog_block_encode(uint8_t *blk, uint32_t seq, uint16_t boot,
                        const uint8_t *text, uint32_t len, int final);
int  evlog_block_decode(const uint8_t *blk, uint32_t *seq, uint16_t *boot,
                        uint32_t *len, int *final);

#endif /* CORE_KERNEL_EVLOG_H */
