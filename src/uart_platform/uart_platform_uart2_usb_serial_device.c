#include "uart_platform_uart2_usb_serial_device.h"

bool UART2_IsRxReady(void)
{
    return false;
}

uint8_t UART2_Read(void)
{
    return 0u;
}

void UART2_RxFlush(void)
{
}
