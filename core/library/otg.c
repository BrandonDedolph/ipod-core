/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/library/otg.c — the On-The-Go live list. See otg.h.
 *
 * Freestanding: no libc, no statics, no allocation, no recursion. Every loop
 * is bounded by the caller's count or by OTG_MAX.
 */

#include "otg.h"

/* One mutation happened. Separate from the mutations themselves so that
 * "gen moves exactly once per accepted change" is one line and not a rule
 * three call sites have to remember. Wraps; see otg.h. */
static void bump(otg_list_t *l)
{
    l->gen = (uint16_t)(l->gen + 1u);
}

void otg_init(otg_list_t *l)
{
    if (l == 0) {
        return;
    }
    l->n   = 0;
    l->gen = 0;
    /* The entries past n are never read, but zeroing them keeps a slot
     * encoded from this list byte-deterministic even if a future encoder
     * covers the whole array. */
    for (uint32_t i = 0; i < OTG_MAX; i++) {
        l->e[i].folder_hash = 0;
        l->e[i].file_hash   = 0;
    }
}

int otg_add(otg_list_t *l, uint32_t folder_hash, uint32_t file_hash)
{
    if (l == 0 || l->n >= OTG_MAX) {
        return 0;
    }
    /* (0, 0) is the "no entry" value the on-disk padding is made of. Storing
     * one would make a saved record decode to a different list than the one
     * that was saved. */
    if (folder_hash == 0 && file_hash == 0) {
        return 0;
    }
    l->e[l->n].folder_hash = folder_hash;
    l->e[l->n].file_hash   = file_hash;
    l->n = (uint16_t)(l->n + 1u);
    bump(l);
    return 1;
}

int otg_add_many(otg_list_t *l, const otg_entry_t *e, int n)
{
    if (l == 0 || e == 0 || n <= 0) {
        return 0;
    }
    int added = 0;
    for (int i = 0; i < n && l->n < OTG_MAX; i++) {
        if (e[i].folder_hash == 0 && e[i].file_hash == 0) {
            continue;
        }
        l->e[l->n].folder_hash = e[i].folder_hash;
        l->e[l->n].file_hash   = e[i].file_hash;
        l->n = (uint16_t)(l->n + 1u);
        added++;
    }
    if (added > 0) {
        /* One bump for the whole album, not one per track: gen exists to tell
         * two versions of the list apart on disk, and an album added in one
         * gesture is one version. */
        bump(l);
    }
    return added;
}

int otg_remove(otg_list_t *l, int idx)
{
    if (l == 0 || idx < 0 || idx >= (int)l->n) {
        return 0;
    }
    for (int i = idx; i + 1 < (int)l->n; i++) {
        l->e[i] = l->e[i + 1];
    }
    l->n = (uint16_t)(l->n - 1u);
    l->e[l->n].folder_hash = 0;
    l->e[l->n].file_hash   = 0;
    bump(l);
    return 1;
}

void otg_clear(otg_list_t *l)
{
    if (l == 0 || l->n == 0) {
        return;                 /* clearing an empty list changes nothing */
    }
    for (uint32_t i = 0; i < l->n; i++) {
        l->e[i].folder_hash = 0;
        l->e[i].file_hash   = 0;
    }
    l->n = 0;
    bump(l);
}

uint32_t otg_crc32_update(uint32_t crc, const uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = (uint32_t)0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc;
}

uint32_t otg_crc32(const uint8_t *p, uint32_t n)
{
    return ~otg_crc32_update(OTG_CRC32_INIT, p, n);
}
