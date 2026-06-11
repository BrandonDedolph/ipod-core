/*
 * core/hal/hw/uart.h — SER0 debug UART (hw target only).
 *
 * Polled-TX serial output on the dock-connector UART (115200 8N1).
 * This is the panic/debug channel: it exists before the LCD driver,
 * before printf, before interrupts. Keep it dependency-free.
 *
 * Not part of the portable HAL contract (hal.h) — sim has no UART.
 * The portable log_printf() will sit on top of this in a later PR.
 */

#ifndef CORE_HAL_HW_UART_H
#define CORE_HAL_HW_UART_H

#include <stdint.h>

/* Configure SER0 for 115200 8N1 and route it to the dock pins.
 * Call once, early in kernel_main, before any other uart_* call. */
void uart_init(void);

/* Transmit one character (polled). '\n' is expanded to "\r\n". */
void uart_putc(char c);

/* Transmit a NUL-terminated string via uart_putc. */
void uart_puts(const char *s);

/* Transmit `v` as exactly 8 uppercase hex digits, no prefix —
 * composable register dumps before printf exists. */
void uart_put_hex32(uint32_t v);

#endif /* CORE_HAL_HW_UART_H */
