#ifndef UART_PLATFORM_UART1_USB_SERIAL_PORT_H
#define UART_PLATFORM_UART1_USB_SERIAL_PORT_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Sonora UART1 compatibility surface.
 *
 * The application owns byte interpretation. This facade only reports RX
 * availability, returns one byte, or discards queued bytes. SERCOM5, its IRQ,
 * and the RX ring remain private to drivers/console.c.
 */
bool    UART1_IsRxReady(void);
uint8_t UART1_Read(void);
void    UART1_RxFlush(void);

#endif /* UART_PLATFORM_UART1_USB_SERIAL_PORT_H */
