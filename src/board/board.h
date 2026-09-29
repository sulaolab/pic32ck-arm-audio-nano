/*
 * board.h - board / CPU level services: clocks, cache, pin muxing, LED,
 *           coarse delays.
 *
 * This layer owns everything that is "the chip and the PCB". It knows
 * nothing about the console, the codec or the audio transport.
 */
#ifndef BOARD_H
#define BOARD_H

#include <stdbool.h>
#include <stdint.h>

/* Device header first: board_pins.h aliases the DFP instance macros
 * (SERCOM4_DMAC_ID_RX, PORT_MCLK_ID_APB, ...) that <xc.h> brings in. */
#include <xc.h>

#include "app_config.h"
#include "board_pins.h"

/* Board startup: clocks -> cache policy -> PORT clock -> LED pin.
 * Must be the first thing main() calls. */
void board_init(void);

/* Actual CPU clock in Hz as configured by board_init(). Compile-time
 * constant APP_CPU_CLOCK_HZ, returned as a function so callers do not
 * bake the profile into their own code. */
uint32_t board_cpu_clock_hz(void);

/* Frequency of the generic clock feeding the SERCOM core inputs. */
uint32_t board_sercom_core_clock_hz(void);

/* Human-readable name of the active clock profile, e.g. "PLL0 120 MHz". */
const char *board_clock_profile_name(void);

/* Enable one peripheral clock. `mclk_id` is an MCLK_ID_AHB / MCLK_ID_APB
 * index from board_pins.h. */
void board_periph_clock_enable(uint32_t mclk_id);

/* Route one peripheral generic-clock channel to a GCLK generator and
 * enable it. Blocks until the channel reports enabled. */
void board_gclk_channel_enable(uint32_t gclk_id, uint32_t generator);

/* Put `pin` of PORT `group` on peripheral function `mux`.
 * `input_enable` also sets PINCFG.INEN, needed for any pad the
 * peripheral reads (UART RX, I2C SDA/SCL, SPI DI/SCK/SS). */
void board_pin_mux(uint32_t group, uint32_t pin, uint32_t mux, bool input_enable);

/* Read a pin's synchronized input level. This remains available while PMUX is
 * enabled when board_pin_mux(..., true) selected PINCFG.INEN. */
bool board_pin_read(uint32_t group, uint32_t pin);

/* Configure `pin` of PORT `group` as a plain push-pull GPIO output. */
void board_pin_output(uint32_t group, uint32_t pin, bool initial_level);

/* User LED (PD05). */
void board_led_set(bool on);
void board_led_toggle(void);

/* Coarse blocking delays built on the polled SysTick counter. Both are
 * "at least this long": the per-tick loop overhead makes them slightly
 * generous, which is the right side to err on for settling delays. */
void board_delay_ms(uint32_t ms);
void board_delay_us(uint32_t us);

#endif /* BOARD_H */
