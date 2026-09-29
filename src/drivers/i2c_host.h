/*
 * i2c_host.h - SERCOM0 I2C host (bus transport only).
 *
 * PD06 = SERCOM0/PAD0 = SDA, PD07 = SERCOM0/PAD1 = SCL, 400 kHz.
 *
 * This layer knows nothing about the WM8904. Every call is bounded by a
 * timeout: a missing or wedged device returns an error, it never spins
 * forever. That is what lets the firmware boot with no codec attached.
 */
#ifndef I2C_HOST_H
#define I2C_HOST_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    I2C_OK = 0,
    I2C_ERR_TIMEOUT,        /* peripheral never reported completion       */
    I2C_ERR_NACK,           /* address or data byte was not acknowledged  */
    I2C_ERR_BUS,            /* BUSERR / ARBLOST / SCL low timeout         */
    I2C_ERR_BUS_STUCK,      /* bus never reached a usable state           */
} i2c_status_t;

void i2c_host_init(void);

/* Write `len` bytes then STOP. */
i2c_status_t i2c_host_write(uint8_t addr7, const uint8_t *data, uint32_t len);

/* Write `wlen` bytes, repeated START, read `rlen` bytes, then STOP.
 * `wlen` may be 0 for a plain read. */
i2c_status_t i2c_host_write_read(uint8_t addr7,
                                 const uint8_t *wdata, uint32_t wlen,
                                 uint8_t *rdata, uint32_t rlen);

const char *i2c_status_str(i2c_status_t s);

#endif /* I2C_HOST_H */
