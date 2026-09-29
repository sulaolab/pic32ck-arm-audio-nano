#include <stddef.h>
#include <stdint.h>

#include "drivers/console.h"
#include "uart_platform_stdio.h"
#include "uart_platform_uart1_usb_serial_port.h"

int write(int handle, void *buffer, unsigned int len)
{
    (void)handle;

    if ((buffer == NULL) || (len == 0u))
    {
        return 0;
    }

    return (int)console_write_raw((const uint8_t *)buffer, (size_t)len);
}

int read(int handle, void *buffer, unsigned int len)
{
    uint8_t *dst = (uint8_t *)buffer;
    unsigned int count = 0u;

    (void)handle;

    if ((buffer == NULL) || (len == 0u))
    {
        return 0;
    }

    while ((count < len) && UART1_IsRxReady())
    {
        uint8_t data = UART1_Read();
        dst[count++] = (data == '\r') ? (uint8_t)'\n' : data;
    }

    return (int)count;
}
