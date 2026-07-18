/*
 * core/kernel/console.h — minimal on-screen text/hex console.
 *
 * Portable, freestanding logic only: renders an 8x8 bitmap font into an
 * RGB565 back buffer. No hardware access — hand the framebuffer to
 * lcd_present() (a separate module) to actually light the panel.
 *
 * The panel is LCD_WIDTH x LCD_HEIGHT RGB565 (see hal/hal.h). With an
 * 8x8-pixel font that is a 40x30 character grid.
 */

#ifndef CORE_KERNEL_CONSOLE_H
#define CORE_KERNEL_CONSOLE_H

#include <stdint.h>

/* RGB565 color helpers. */
#define CON_BLACK  0x0000u
#define CON_WHITE  0xFFFFu
#define CON_RED    0xF800u
#define CON_GREEN  0x07E0u
#define CON_BLUE   0x001Fu
#define CON_YELLOW 0xFFE0u
#define CON_CYAN   0x07FFu

/* The back buffer (LCD_WIDTH*LCD_HEIGHT RGB565), for handing to lcd_present(). */
const uint16_t *console_framebuffer(void);

/* Fill the whole framebuffer with one color. */
void console_clear(uint16_t rgb565);

/* Draw one character at character-cell (col,row) [col 0..39, row 0..29]
 * with fg/bg colors. Supported glyphs: '0'-'9', 'A'-'F' (hex), space,
 * and the uppercase letters used for labels: at minimum
 * F R E Q P L S T C U N G I O W M = - (any unsupported char draws as a
 * blank/space cell). Out-of-range
 * col/row is a no-op (bounds-checked). */
void console_char(int col, int row, char ch, uint16_t fg, uint16_t bg);

/* Draw a NUL-terminated string starting at (col,row), advancing right;
 * does not wrap (stops at col 40). */
void console_str(int col, int row, const char *s, uint16_t fg, uint16_t bg);

/* Draw `value` as 8 uppercase hex digits at (col,row). */
void console_hex32(int col, int row, uint32_t value, uint16_t fg, uint16_t bg);

#endif /* CORE_KERNEL_CONSOLE_H */
