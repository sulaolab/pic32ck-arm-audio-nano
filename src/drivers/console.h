/*
 * console.h - SERCOM5 USART console, 230400 8N1, blocking.
 *
 * SERCOM5/PAD0 = PA11 (target TX), SERCOM5/PAD1 = PA12 (target RX).
 * These are the pins the on-board debugger exposes as a USB CDC port.
 *
 * Deliberately blocking and interrupt-free: the whole point of this layer
 * during initial hardware checks is that a banner reaching the terminal proves the CPU and
 * clock tree came up. No printf() from libc, no heap, no DMA.
 */
#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void console_init(void);

void console_putc(char c);
size_t console_write_raw(const uint8_t *data, size_t length);
void console_write(const char *s);
void console_writeln(const char *s);

/* Minimal formatter. Supported conversions:
 *   %c %s %u %d %x %X %% and a zero-padded width for the integers
 *   (e.g. %04x). No floats, no long modifiers, no dynamic memory. */
void console_printf(const char *fmt, ...);

/* Non-blocking read of one character. Returns false if nothing arrived. */
bool console_getc(char *out);
bool console_rx_ready(void);
void console_rx_flush(void);

typedef struct
{
    uint32_t rx_isr_count;
    uint32_t rx_byte_count;
    uint32_t rx_ring_overflow_count;
} console_rx_status_t;

/* CSV-transfer diagnostics. Clearing resets counters only; queued RX bytes
 * remain untouched. */
void console_rx_status_get(console_rx_status_t *out);
void console_rx_status_clear(void);

#endif /* CONSOLE_H */
