/*
 * PIC32CK platform binding for the Sonora WM8904 common driver.
 *
 * No codec register names, values or sequencing belong here.  This file
 * describes only the fixed transport geometry and the six platform services
 * used by the common driver.
 */
#ifndef WM8904_PORT_PIC32CK_H
#define WM8904_PORT_PIC32CK_H

#include <stdbool.h>
#include <stdint.h>

#include "console.h"

#define RESOLVED_TRANSPORT_SLOTS_PER_FRAME       8u
/* DSP mode A aligns the WM8904's one-bit data delay with this framed-SPI
 * configuration. The codec and transport settings must be changed together. */
#define RESOLVED_TRANSPORT_DATA_DELAY_BITS       1u
#define RESOLVED_BOARD_CODEC_INPUT_IS_RED_JACK   0u /* AC164162 test circuit uses IN2. */
#define RESOLVED_BOARD_CODEC_MIC_BIAS_ENABLED    0u

#define COMPILEASSERT(condition) _Static_assert((condition), #condition)
#define wm8904_port_log          console_printf

bool wm8904_port_i2c_write(uint8_t inst, uint8_t addr7,
                           const uint8_t *data, uint32_t len);
bool wm8904_port_i2c_write_read(uint8_t inst, uint8_t addr7,
                                const uint8_t *wdata, uint32_t wlen,
                                uint8_t *rdata, uint32_t rlen);
void wm8904_port_delay_us(uint32_t us);
void wm8904_port_delay_ms(uint32_t ms);
uint32_t wm8904_port_tick_ms(void);

/* Keep the imported register sequence readable with its original delay/tick
 * spellings; the implementation still resolves only through this port. */
#define delay_us(us) wm8904_port_delay_us((uint32_t)(us))
#define delay_ms(ms) wm8904_port_delay_ms((uint32_t)(ms))
#define GetTicks()   wm8904_port_tick_ms()
#define printf       wm8904_port_log

#endif /* WM8904_PORT_PIC32CK_H */
