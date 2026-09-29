#ifndef UART_PLATFORM_STDIO_H
#define UART_PLATFORM_STDIO_H

/* XC32/newlib stdio retarget. Application printf/putchar/fwrite calls reach
 * the PIC32CK console without importing board registers into application code. */
int write(int handle, void *buffer, unsigned int len)
    __attribute__((__section__(".libc.write")));

int read(int handle, void *buffer, unsigned int len)
    __attribute__((__section__(".libc.read")));

#endif /* UART_PLATFORM_STDIO_H */
