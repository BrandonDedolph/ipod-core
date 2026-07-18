/*
 * core/tests/hw_console/console_test.c — host logic test for the on-screen
 * console. Pure portable logic; no MMIO / hardware. Build with plain cc.
 *
 *   cc -std=gnu11 -Wall -Wextra -Ikernel -Ihal \
 *      kernel/console.c tests/hw_console/console_test.c -o /tmp/cot && /tmp/cot
 *
 * main() returns nonzero if any case fails; prints PASS/FAIL per case.
 */

#include "console.h"
#include "hal.h"        /* LCD_WIDTH, LCD_HEIGHT */

#include <stdint.h>
#include <stdio.h>

#define GLYPH_W 8
#define GLYPH_H 8

/*
 * Expected glyph bitmaps, duplicated here independently of console.c so the
 * test actually pins the rendered output. Bit 7 = leftmost pixel; a set bit
 * must render as fg, a clear bit as bg. These must match console.c's font.
 */
typedef struct { char ch; uint8_t rows[GLYPH_H]; } exp_glyph_t;

static const exp_glyph_t k_expected[] = {
    { '0', { 0x70, 0x88, 0x98, 0xA8, 0xC8, 0x88, 0x70, 0x00 } },
    { '1', { 0x20, 0x60, 0x20, 0x20, 0x20, 0x20, 0x70, 0x00 } },
    { '2', { 0x70, 0x88, 0x10, 0x20, 0x40, 0x80, 0xF8, 0x00 } },
    { '3', { 0x70, 0x88, 0x08, 0x30, 0x08, 0x88, 0x70, 0x00 } },
    { '4', { 0x10, 0x30, 0x50, 0x90, 0xF8, 0x10, 0x10, 0x00 } },
    { 'A', { 0x70, 0x88, 0x88, 0xF8, 0x88, 0x88, 0x88, 0x00 } },
    { 'B', { 0xF0, 0x88, 0x88, 0xF0, 0x88, 0x88, 0xF0, 0x00 } },
    { 'C', { 0x70, 0x88, 0x80, 0x80, 0x80, 0x88, 0x70, 0x00 } },
    { 'D', { 0xF0, 0x88, 0x88, 0x88, 0x88, 0x88, 0xF0, 0x00 } },
    /* Label glyphs added for the audio diagnostic screen. */
    { 'G', { 0x70, 0x88, 0x80, 0xB8, 0x88, 0x88, 0x70, 0x00 } },
    { 'I', { 0x70, 0x20, 0x20, 0x20, 0x20, 0x20, 0x70, 0x00 } },
    { 'M', { 0x88, 0xD8, 0xA8, 0xA8, 0x88, 0x88, 0x88, 0x00 } },
    { 'O', { 0x70, 0x88, 0x88, 0x88, 0x88, 0x88, 0x70, 0x00 } },
    { 'W', { 0x88, 0x88, 0x88, 0xA8, 0xA8, 0xA8, 0x50, 0x00 } },
};

static const uint8_t *expected_rows(char ch)
{
    for (size_t i = 0; i < sizeof k_expected / sizeof k_expected[0]; i++) {
        if (k_expected[i].ch == ch) {
            return k_expected[i].rows;
        }
    }
    return NULL;
}

/*
 * Verify that the 8x8 cell (col,row) exactly renders `ch` with fg/bg.
 * Returns 1 on match, 0 on any mismatched pixel.
 */
static int cell_matches(const uint16_t *fb, int col, int row, char ch,
                        uint16_t fg, uint16_t bg)
{
    const uint8_t *rows = expected_rows(ch);
    int base_x = col * GLYPH_W;
    int base_y = row * GLYPH_H;

    if (rows == NULL) {
        printf("  (no expected glyph for '%c')\n", ch);
        return 0;
    }
    for (int ry = 0; ry < GLYPH_H; ry++) {
        for (int cx = 0; cx < GLYPH_W; cx++) {
            int set = (rows[ry] >> (7 - cx)) & 1;
            uint16_t want = set ? fg : bg;
            uint16_t got = fb[(base_y + ry) * LCD_WIDTH + (base_x + cx)];
            if (got != want) {
                printf("  cell(%d,%d) '%c' pixel(%d,%d): got %04X want %04X\n",
                       col, row, ch, cx, ry, got, want);
                return 0;
            }
        }
    }
    return 1;
}

static int case1_clear(void)
{
    console_clear(CON_BLUE);
    const uint16_t *fb = console_framebuffer();
    for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++) {
        if (fb[i] != CON_BLUE) {
            printf("FAIL case1: pixel %d = %04X, expected %04X\n",
                   i, fb[i], CON_BLUE);
            return 0;
        }
    }
    printf("PASS case1: console_clear fills entire framebuffer\n");
    return 1;
}

static int case2_char(void)
{
    console_clear(CON_BLACK);
    console_char(0, 0, '0', CON_WHITE, CON_BLACK);
    const uint16_t *fb = console_framebuffer();

    /* At least one WHITE pixel somewhere in cell (0,0). */
    int found_white = 0;
    for (int ry = 0; ry < GLYPH_H && !found_white; ry++) {
        for (int cx = 0; cx < GLYPH_W; cx++) {
            if (fb[ry * LCD_WIDTH + cx] == CON_WHITE) { found_white = 1; break; }
        }
    }
    if (!found_white) {
        printf("FAIL case2: no WHITE pixel in cell (0,0)\n");
        return 0;
    }

    /* Everything outside cell (0,0) stays BLACK. */
    for (int y = 0; y < LCD_HEIGHT; y++) {
        for (int x = 0; x < LCD_WIDTH; x++) {
            if (x < GLYPH_W && y < GLYPH_H) { continue; }
            if (fb[y * LCD_WIDTH + x] != CON_BLACK) {
                printf("FAIL case2: pixel(%d,%d) outside cell is %04X\n",
                       x, y, fb[y * LCD_WIDTH + x]);
                return 0;
            }
        }
    }

    /* Rendered pattern matches the '0' glyph exactly. */
    if (!cell_matches(fb, 0, 0, '0', CON_WHITE, CON_BLACK)) {
        printf("FAIL case2: cell (0,0) does not match '0' glyph\n");
        return 0;
    }
    printf("PASS case2: console_char renders '0' into correct cell\n");
    return 1;
}

static int case3_hex32(void)
{
    console_clear(CON_BLACK);
    console_hex32(0, 0, 0x1234ABCDu, CON_WHITE, CON_BLACK);
    const uint16_t *fb = console_framebuffer();

    const char *expect = "1234ABCD";
    for (int i = 0; i < 8; i++) {
        if (!cell_matches(fb, i, 0, expect[i], CON_WHITE, CON_BLACK)) {
            printf("FAIL case3: cell (%d,0) != '%c'\n", i, expect[i]);
            return 0;
        }
    }
    printf("PASS case3: console_hex32 renders 1234ABCD\n");
    return 1;
}

static int case4_bounds(void)
{
    const uint16_t SENTINEL = 0x1234u;
    console_clear(SENTINEL);
    const uint16_t *fb = console_framebuffer();

    /* Each of these is out of range and must be a no-op. */
    console_char(-1, 0, '0', CON_WHITE, CON_BLACK);
    console_char(40, 0, '0', CON_WHITE, CON_BLACK);
    console_char(0, 30, '0', CON_WHITE, CON_BLACK);
    console_char(0, -1, '0', CON_WHITE, CON_BLACK);

    for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++) {
        if (fb[i] != SENTINEL) {
            printf("FAIL case4: OOB draw modified pixel %d (=%04X)\n",
                   i, fb[i]);
            return 0;
        }
    }
    printf("PASS case4: out-of-range console_char calls are no-ops\n");
    return 1;
}

/* The label glyphs added for the audio diagnostic screen (W/O/G/I) — the
 * ones that used to render blank and garbled WROT/FIFO/CFG into R T/F F/CF. */
static int case5_label_glyphs(void)
{
    console_clear(CON_BLACK);
    console_str(0, 0, "WOGIM", CON_WHITE, CON_BLACK);
    const uint16_t *fb = console_framebuffer();

    const char *expect = "WOGIM";
    for (int i = 0; i < 5; i++) {
        if (!cell_matches(fb, i, 0, expect[i], CON_WHITE, CON_BLACK)) {
            printf("FAIL case5: cell (%d,0) != '%c'\n", i, expect[i]);
            return 0;
        }
    }
    printf("PASS case5: W/O/G/I label glyphs render\n");
    return 1;
}

int main(void)
{
    int ok = 1;
    ok &= case1_clear();
    ok &= case2_char();
    ok &= case3_hex32();
    ok &= case4_bounds();
    ok &= case5_label_glyphs();

    if (ok) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("SOME FAILED\n");
    return 1;
}
