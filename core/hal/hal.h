/*
 * core/hal/hal.h — Hardware Abstraction Layer contract.
 *
 * This is the only header the kernel + apps + codecs include to talk to
 * "the hardware." Two backends implement it:
 *
 *   hal/hw/   — touches real PP5022 registers; runs on the iPod
 *   hal/sim/  — backed by SDL2 + a file-backed disk image; runs on the host
 *
 * The contract is deliberately small. Everything above the HAL is
 * portable; everything below is target-specific.
 *
 * Convention: functions return 0 on success, negative on failure. Output
 * pointers are written only on success.
 */

#ifndef CORE_HAL_H
#define CORE_HAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------- Generic ------------------------------------------------- */

/*
 * hal_init initializes the entire HAL: LCD, button, clock, log.
 * Must be called once before any other hal_* function.
 * Returns 0 on success, negative on init failure.
 */
int hal_init(void);

/*
 * hal_shutdown tears down the HAL cleanly. Should be called before
 * exit so the sim window closes nicely. Has no effect on hw target.
 */
void hal_shutdown(void);

/* ---------- LCD ----------------------------------------------------- */

#define LCD_WIDTH   320
#define LCD_HEIGHT  240
#define LCD_BPP     16    /* RGB565 */

/* Pixel type. RGB565 little-endian both targets. */
typedef uint16_t lcd_pixel_t;

/*
 * lcd_framebuffer returns a pointer to the host-side back buffer.
 * Writes to this buffer don't appear on the LCD until lcd_present().
 * Buffer is LCD_WIDTH * LCD_HEIGHT * sizeof(lcd_pixel_t) = 153,600 bytes.
 */
lcd_pixel_t *lcd_framebuffer(void);

/*
 * lcd_present pushes the back buffer to the display. On hw this issues
 * BCMCMD_LCD_UPDATE; on sim it copies into the SDL texture and presents.
 */
void lcd_present(void);

/*
 * Convenience: pack a 24-bit (R8G8B8) color into RGB565.
 */
static inline lcd_pixel_t lcd_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return (lcd_pixel_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/*
 * Convenience: fill the entire framebuffer with one color. Doesn't
 * present; caller must lcd_present() to make it visible.
 */
void lcd_fill(lcd_pixel_t color);

/* ---------- Button -------------------------------------------------- */

/*
 * Button bitmap. Multiple bits can be set (e.g. SELECT held while
 * SCROLL_FWD).  The values match the iPod's wheel-packet button bits
 * (see core/docs/hw/03-clickwheel.md) so the hw backend can pass them
 * through unchanged.
 */
typedef enum {
    BUTTON_NONE        = 0,
    BUTTON_SELECT      = 1u << 0,
    BUTTON_RIGHT       = 1u << 1,
    BUTTON_LEFT        = 1u << 2,
    BUTTON_PLAY        = 1u << 3,
    BUTTON_MENU        = 1u << 4,
    BUTTON_SCROLL_FWD  = 1u << 5,
    BUTTON_SCROLL_BACK = 1u << 6,
    BUTTON_HOLD        = 1u << 7,
    BUTTON_QUIT        = 1u << 8,   /* sim only: window-close */
} button_t;

/*
 * button_get blocks for up to timeout_ms milliseconds and returns a
 * button bitmap (or BUTTON_NONE on timeout). timeout_ms == 0 means
 * "non-blocking poll"; timeout_ms == -1 means "block until an event".
 */
button_t button_get(int timeout_ms);

/* ---------- Clock --------------------------------------------------- */

/*
 * clock_ms returns monotonic milliseconds since hal_init().
 * Wraps after ~49.7 days (uint32_t).
 */
uint32_t clock_ms(void);

/*
 * clock_us returns monotonic microseconds since hal_init(). Used for
 * fine-grained timing; wraps after ~71 minutes (uint32_t). For longer
 * intervals use clock_ms.
 */
uint32_t clock_us(void);

/*
 * sleep_ms blocks the current thread for at least ms milliseconds.
 * Coarse — minimum granularity is the OS / kernel tick.
 */
void sleep_ms(uint32_t ms);

/* ---------- Log ----------------------------------------------------- */

/*
 * log_printf writes a printf-style formatted message to the debug log.
 * On hw, this goes to the dock-connector UART. On sim, to stdout.
 *
 * In a release build (-Drelease=true) this compiles out entirely.
 */
#ifdef CORE_RELEASE
#  define log_printf(...) ((void)0)
#else
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#endif

#endif /* CORE_HAL_H */
