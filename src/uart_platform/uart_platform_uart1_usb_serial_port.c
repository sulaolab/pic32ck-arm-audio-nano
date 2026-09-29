#include "drivers/console.h"

#include "uart_platform_uart1_usb_serial_port.h"

bool UART1_IsRxReady(void)
{
    return console_rx_ready();
}

uint8_t UART1_Read(void)
{
    char data = 0;

    if (!console_getc(&data))
    {
        return 0u;
    }

    return (uint8_t)data;
}

void UART1_RxFlush(void)
{
    console_rx_flush();
}
