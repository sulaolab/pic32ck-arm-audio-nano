/* PIC32CK platform services for the Sonora WM8904 common driver. */
#include "wm8904_port_pic32ck.h"

#include "../board/board.h"
#include "i2c_host.h"

enum { WM8904_PIC32CK_I2C_INSTANCE = 1u };
static bool s_i2c_error_reported;

static bool wm8904_port_i2c_result(const char *operation, i2c_status_t status)
{
    if ((status != I2C_OK) && !s_i2c_error_reported)
    {
        console_printf("wm8904 port: I2C %s failed: %s\n",
                       operation, i2c_status_str(status));
        s_i2c_error_reported = true;
    }
    return status == I2C_OK;
}

bool wm8904_port_i2c_write(uint8_t inst, uint8_t addr7,
                           const uint8_t *data, uint32_t len)
{
    if (inst != WM8904_PIC32CK_I2C_INSTANCE)
    {
        return false;
    }

    i2c_status_t status = i2c_host_write(addr7, data, len);
    if (status != I2C_OK)
    {
        i2c_host_init();
        status = i2c_host_write(addr7, data, len);
    }
    return wm8904_port_i2c_result("write", status);
}

bool wm8904_port_i2c_write_read(uint8_t inst, uint8_t addr7,
                                const uint8_t *wdata, uint32_t wlen,
                                uint8_t *rdata, uint32_t rlen)
{
    if (inst != WM8904_PIC32CK_I2C_INSTANCE)
    {
        return false;
    }

    i2c_status_t status = i2c_host_write_read(addr7, wdata, wlen, rdata, rlen);
    if (status != I2C_OK)
    {
        i2c_host_init();
        status = i2c_host_write_read(addr7, wdata, wlen, rdata, rlen);
    }
    return wm8904_port_i2c_result("write-read", status);
}

void wm8904_port_delay_us(uint32_t us)
{
    board_delay_us(us);
}

void wm8904_port_delay_ms(uint32_t ms)
{
    board_delay_ms(ms);
}

uint32_t wm8904_port_tick_ms(void)
{
    /* PIC32CK has no application millisecond counter yet.  The common driver
     * uses this value for trace timestamps only; all waits use bounded delay
     * loops, so zero is an intentional and safe port value. */
    return 0u;
}
