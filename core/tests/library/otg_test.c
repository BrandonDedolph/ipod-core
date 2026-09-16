/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/library/otg_test.c — the On-The-Go live list (library/otg.c).
 *
 * Pure: no disk, no fixture, no hardware. What is asserted is the contract
 * the UI leans on — the count on screen is the number of times the user
 * pressed the button, the order is the order they added things, a full list
 * refuses rather than wraps or drops, and `gen` moves exactly once per
 * ACCEPTED change (it is what tells two versions of the list apart on disk,
 * so a gen that moved on a refused add would make an unchanged file read as
 * torn, and one that did not move on a real change would make a torn file
 * read as intact).
 */

#include <stdio.h>
#include <string.h>

#include "../../library/otg.h"

static int g_fails;

static void check(const char *label, int cond)
{
    printf("[%s] %s\n", label, cond ? "PASS" : "FAIL");
    if (!cond) {
        g_fails++;
    }
}

static otg_list_t g_l;

static int entry_is(const otg_list_t *l, int i, uint32_t fh, uint32_t xh)
{
    return i < (int)l->n && l->e[i].folder_hash == fh && l->e[i].file_hash == xh;
}

/* ---- 1. add ------------------------------------------------------------- */

static void test_add(void)
{
    otg_init(&g_l);
    check("a fresh list is empty at gen 0", g_l.n == 0 && g_l.gen == 0);

    check("add returns 1", otg_add(&g_l, 1, 2) == 1);
    check("...and appends", g_l.n == 1 && entry_is(&g_l, 0, 1, 2));
    check("...and moves gen", g_l.gen == 1);

    (void)otg_add(&g_l, 3, 4);
    (void)otg_add(&g_l, 5, 6);
    check("order is insertion order",
          g_l.n == 3 && entry_is(&g_l, 0, 1, 2) &&
          entry_is(&g_l, 1, 3, 4) && entry_is(&g_l, 2, 5, 6));
    check("gen moved once per add", g_l.gen == 3);

    /* The original iPod allows the same track twice, a playlist file may list
     * one twice, and playlist_resolve already copes. */
    check("a duplicate is a second entry", otg_add(&g_l, 1, 2) == 1);
    check("...really appended", g_l.n == 4 && entry_is(&g_l, 3, 1, 2));

    /* (0,0) is what the on-disk padding is made of; storing one would make a
     * saved slot decode to a different list than the one that was saved. */
    uint16_t gen = g_l.gen;
    check("the null pair is refused", otg_add(&g_l, 0, 0) == 0);
    check("...and a refused add does not move gen", g_l.gen == gen && g_l.n == 4);
    check("a pair with one zero half is fine",
          otg_add(&g_l, 0, 9) == 1 && otg_add(&g_l, 9, 0) == 1);

    check("a null list is refused", otg_add(0, 1, 2) == 0);
}

/* ---- 2. the ceiling ----------------------------------------------------- */

static void test_full(void)
{
    otg_init(&g_l);
    for (uint32_t i = 0; i < OTG_MAX; i++) {
        if (otg_add(&g_l, i + 1u, i + 2u) != 1) {
            check("the list takes OTG_MAX entries", 0);
            return;
        }
    }
    check("the list takes OTG_MAX entries", g_l.n == OTG_MAX);
    uint16_t gen = g_l.gen;
    check("one more is refused", otg_add(&g_l, 7, 7) == 0);
    check("...the list is untouched", g_l.n == OTG_MAX && g_l.gen == gen);
    check("...and the last entry is still the 512th",
          entry_is(&g_l, (int)OTG_MAX - 1, OTG_MAX, OTG_MAX + 1u));
}

/* ---- 3. add_many -------------------------------------------------------- */

static void test_add_many(void)
{
    otg_entry_t album[12];
    for (int i = 0; i < 12; i++) {
        album[i].folder_hash = 0xA0000000u;
        album[i].file_hash   = (uint32_t)(i + 1);
    }

    otg_init(&g_l);
    check("a whole album goes in at once", otg_add_many(&g_l, album, 12) == 12);
    check("...in order",
          g_l.n == 12 && entry_is(&g_l, 0, 0xA0000000u, 1) &&
          entry_is(&g_l, 11, 0xA0000000u, 12));
    check("...for ONE gen bump (one gesture, one version)", g_l.gen == 1);

    /* The partial fit: the caller says "Added 7 of 12". */
    otg_init(&g_l);
    for (uint32_t i = 0; i < OTG_MAX - 7u; i++) {
        (void)otg_add(&g_l, i + 1u, i + 1u);
    }
    check("add_many returns how many fit", otg_add_many(&g_l, album, 12) == 7);
    check("...and stops at the ceiling", g_l.n == OTG_MAX);

    uint16_t gen = g_l.gen;
    check("add_many into a full list adds nothing",
          otg_add_many(&g_l, album, 12) == 0);
    check("...and does not move gen", g_l.gen == gen);

    otg_init(&g_l);
    otg_entry_t mixed[3] = { { 0, 0 }, { 1, 1 }, { 0, 0 } };
    check("null pairs inside a batch are skipped",
          otg_add_many(&g_l, mixed, 3) == 1 && g_l.n == 1 &&
          entry_is(&g_l, 0, 1, 1));
    check("a batch of nothing but null pairs adds nothing and keeps gen",
          otg_add_many(&g_l, mixed, 1) == 0 && g_l.gen == 1);

    check("null/empty arguments are refused",
          otg_add_many(0, album, 1) == 0 && otg_add_many(&g_l, 0, 1) == 0 &&
          otg_add_many(&g_l, album, 0) == 0 && otg_add_many(&g_l, album, -3) == 0);
}

/* ---- 4. remove and clear ------------------------------------------------ */

static void test_remove_clear(void)
{
    otg_init(&g_l);
    for (uint32_t i = 0; i < 5; i++) {
        (void)otg_add(&g_l, 100u + i, 200u + i);
    }
    uint16_t gen = g_l.gen;

    check("remove from the middle closes the gap", otg_remove(&g_l, 2) == 1);
    check("...keeping the order of the rest",
          g_l.n == 4 && entry_is(&g_l, 0, 100, 200) && entry_is(&g_l, 1, 101, 201) &&
          entry_is(&g_l, 2, 103, 203) && entry_is(&g_l, 3, 104, 204));
    check("...and moves gen", g_l.gen == (uint16_t)(gen + 1));

    check("remove the last row", otg_remove(&g_l, 3) == 1 && g_l.n == 3);
    check("remove the first row",
          otg_remove(&g_l, 0) == 1 && g_l.n == 2 && entry_is(&g_l, 0, 101, 201));

    gen = g_l.gen;
    check("an index past the end is refused", otg_remove(&g_l, 2) == 0);
    check("a negative index is refused", otg_remove(&g_l, -1) == 0);
    check("...neither moves gen", g_l.gen == gen && g_l.n == 2);

    check("clear empties it and moves gen",
          (otg_clear(&g_l), g_l.n == 0 && g_l.gen == (uint16_t)(gen + 1)));
    gen = g_l.gen;
    check("clearing an empty list changes nothing",
          (otg_clear(&g_l), g_l.n == 0 && g_l.gen == gen));

    check("remove from an empty list is refused", otg_remove(&g_l, 0) == 0);

    /* A cleared list must be re-usable, and the entries it held must not
     * survive into the slot a later save encodes. */
    (void)otg_add(&g_l, 7, 8);
    check("the list works again after a clear",
          g_l.n == 1 && entry_is(&g_l, 0, 7, 8));
    check("...over a zeroed tail",
          g_l.e[1].folder_hash == 0 && g_l.e[1].file_hash == 0);
}

/* ---- 5. gen wraps ------------------------------------------------------- */

static void test_gen_wrap(void)
{
    otg_init(&g_l);
    g_l.gen = 0xFFFFu;
    (void)otg_add(&g_l, 1, 1);
    /* Nothing ever compares two gens for ORDER — only for equality inside one
     * file written in one pass — so the wrap is a non-event, and saying so
     * here is what stops someone "fixing" it with a saturating counter. */
    check("gen wraps at 16 bits", g_l.gen == 0);
}

/* ---- 6. the CRC --------------------------------------------------------- */

static void test_crc(void)
{
    /* The standard vector. Both on-disk formats and all three host
     * implementations depend on this being plain zlib CRC-32. */
    check("CRC-32(\"123456789\") == 0xCBF43926",
          otg_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u);
    check("CRC-32 of nothing is 0", otg_crc32((const uint8_t *)"", 0) == 0);

    /* The running form has to equal the one-shot form over the pieces —
     * otg_slot.c checksums entry lines as it formats them and never holds
     * the concatenation. */
    uint32_t run = OTG_CRC32_INIT;
    run = otg_crc32_update(run, (const uint8_t *)"1234", 4);
    run = otg_crc32_update(run, (const uint8_t *)"56789", 5);
    check("the running CRC equals the one-shot CRC",
          ~run == otg_crc32((const uint8_t *)"123456789", 9));
}

int main(void)
{
    /* The one instance lives in kernel/main.c's .bss; keep its size honest. */
    printf("sizeof(otg_list_t) = %u\n", (unsigned)sizeof(otg_list_t));
    check("otg_list_t is 4 + 8 * OTG_MAX bytes",
          sizeof(otg_list_t) == 4u + 8u * OTG_MAX);
    check("otg_entry_t is exactly the on-disk pair", sizeof(otg_entry_t) == 8u);

    test_add();
    test_full();
    test_add_many();
    test_remove_clear();
    test_gen_wrap();
    test_crc();

    printf("otg_test: %s (%d failure%s)\n",
           g_fails == 0 ? "PASS" : "FAIL", g_fails, g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
