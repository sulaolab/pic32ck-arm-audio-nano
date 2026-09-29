#ifndef UART_PLATFORM_UART2_USB_SERIAL_DEVICE_H
#define UART_PLATFORM_UART2_USB_SERIAL_DEVICE_H

#include <stdbool.h>
#include <stdint.h>

/* PIC32CK Nano exposes one command UART. Keep Sonora's UART2-facing parser
 * API intact with a transport-only, permanently-empty compatibility port. */
bool    UART2_IsRxReady(void);
uint8_t UART2_Read(void);
void    UART2_RxFlush(void);

#endif /* UART_PLATFORM_UART2_USB_SERIAL_DEVICE_H */
