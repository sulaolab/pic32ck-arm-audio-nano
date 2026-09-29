/*
 * console.c - SERCOM5 USART, internal clock, 230400 8N1, polled.
 */
#include "console.h"

#include <stdarg.h>
#include <stddef.h>

#include "board.h"

#define CONSOLE_SERCOM_REGS     SERCOM5_REGS
#define CONSOLE_RX_RING_SIZE    4096u

/* RX drains into a small SPSC ring so complete command lines and coefficient
 * transfers do not overrun the USART FIFO. TX remains deliberately polled. */
static volatile uint8_t  console_rx_ring[CONSOLE_RX_RING_SIZE];
static volatile uint32_t console_rx_head;
static volatile uint32_t console_rx_tail;
static volatile uint32_t console_rx_isr_count;
static volatile uint32_t console_rx_byte_count;
static volatile uint32_t console_rx_ring_overflow_count;

/*
 * Asynchronous arithmetic baud generator, 16x oversampling:
 *
 *      BAUD = 65536 * (1 - 16 * fBAUD / fREF)
 *
 * With fREF = GCLK_SERCOM5_CORE = 48 MHz and fBAUD = 230400:
 *      BAUD = 65536 * (1 - 3686400/48000000) = 60503
 *      actual = 48e6 * (65536-60503) / 65536 / 16 = 230392 baud (-0.003 %)
 *
 * Computed here rather than written as a literal, so changing
 * APP_GCLK_SERCOM_CORE_HZ or APP_CONSOLE_BAUD stays correct. The 64-bit
 * intermediate keeps 65536 * fREF from overflowing.
 */
#define CONSOLE_BAUD_REG                                                      \
    ((uint16_t)(65536ull -                                                    \
                ((65536ull * 16ull * (uint64_t)APP_CONSOLE_BAUD) /            \
                 (uint64_t)APP_GCLK_SERCOM_CORE_HZ)))

void console_init(void)
{
    sercom_usart_int_registers_t *const u = &CONSOLE_SERCOM_REGS->USART_INT;

    board_periph_clock_enable(BOARD_MCLK_ID_SERCOM5_APB);
    board_gclk_channel_enable(BOARD_GCLK_ID_SERCOM5_CORE, APP_GCLK_GEN_SERCOM_CORE);

    /* PA11 -> TX (output, but PMUXEN is what matters), PA12 -> RX (needs
     * INEN so the pad drives the peripheral input). */
    board_pin_mux(BOARD_UART_TX_PORT, BOARD_UART_TX_PIN, BOARD_UART_TX_MUX, false);
    board_pin_mux(BOARD_UART_RX_PORT, BOARD_UART_RX_PIN, BOARD_UART_RX_MUX, true);

    u->SERCOM_CTRLA = SERCOM_USART_INT_CTRLA_SWRST_Msk;
    while ((u->SERCOM_SYNCBUSY & SERCOM_USART_INT_SYNCBUSY_SWRST_Msk) != 0u)
    {
        /* wait for software reset */
    }

    u->SERCOM_CTRLA =
        SERCOM_USART_INT_CTRLA_MODE_USART_INT_CLK |
        SERCOM_USART_INT_CTRLA_SAMPR_ARITHM16X |
        SERCOM_USART_INT_CTRLA_TXPO(BOARD_UART_TXPO) |
        SERCOM_USART_INT_CTRLA_RXPO(BOARD_UART_RXPO) |
        SERCOM_USART_INT_CTRLA_DORD_LSB;              /* LSB first, standard UART */

    u->SERCOM_CTRLB =
        SERCOM_USART_INT_CTRLB_CHSIZE_8_BIT |         /* 8 data bits          */
        SERCOM_USART_INT_CTRLB_TXEN_Msk |
        SERCOM_USART_INT_CTRLB_RXEN_Msk;
    /* SBMODE = 0 -> 1 stop bit, PMODE/parity disabled via CTRLA.FORM = 0.
     * Together: 8N1. */
    while ((u->SERCOM_SYNCBUSY & SERCOM_USART_INT_SYNCBUSY_CTRLB_Msk) != 0u)
    {
        /* wait for CTRLB */
    }

    u->SERCOM_BAUD = CONSOLE_BAUD_REG;

    u->SERCOM_CTRLA |= SERCOM_USART_INT_CTRLA_ENABLE_Msk;
    while ((u->SERCOM_SYNCBUSY & SERCOM_USART_INT_SYNCBUSY_ENABLE_Msk) != 0u)
    {
        /* wait for enable */
    }

    u->SERCOM_INTENSET = SERCOM_USART_INT_INTENSET_RXC_Msk;
    NVIC_ClearPendingIRQ(SERCOM5_2_IRQn);    /* SERCOM INT2 = USART RXC */
    /* 230400 baud delivers one byte every 43.4 us, while the 4-channel DRC
     * callback takes about 145 us.  RX must therefore pre-empt the audio DMA
     * callback or SERCOM's hardware holding register overflows before this
     * handler can drain it.  The handler only copies bytes into the ring. */
    NVIC_SetPriority(SERCOM5_2_IRQn, 0u);
    NVIC_EnableIRQ(SERCOM5_2_IRQn);
}

void SERCOM5_2_Handler(void)
{
    sercom_usart_int_registers_t *const u = &CONSOLE_SERCOM_REGS->USART_INT;

    console_rx_isr_count++;

    while ((u->SERCOM_INTFLAG & SERCOM_USART_INT_INTFLAG_RXC_Msk) != 0u)
    {
        const uint8_t data = (uint8_t)u->SERCOM_DATA;
        const uint32_t head = console_rx_head;
        const uint32_t next = (head + 1u) & (CONSOLE_RX_RING_SIZE - 1u);

        console_rx_byte_count++;

        if (next != console_rx_tail)
        {
            console_rx_ring[head] = data;
            console_rx_head = next;
        }
        else
        {
            console_rx_ring_overflow_count++;
        }
        /* A full software ring drops the newest byte.  The 4 KiB ring holds a
         * complete coefficient file sent at once. */
    }
}

void console_putc(char c)
{
    sercom_usart_int_registers_t *const u = &CONSOLE_SERCOM_REGS->USART_INT;

    while ((u->SERCOM_INTFLAG & SERCOM_USART_INT_INTFLAG_DRE_Msk) == 0u)
    {
        /* wait for the data register to be empty */
    }
    u->SERCOM_DATA = (uint32_t)(uint8_t)c;
}

size_t console_write_raw(const uint8_t *data, size_t length)
{
    size_t written = 0u;

    if (data == NULL)
    {
        return 0u;
    }

    while (written < length)
    {
        console_putc((char)data[written]);
        written++;
    }

    return written;
}

void console_write(const char *s)
{
    while (*s != '\0')
    {
        if (*s == '\n')
        {
            console_putc('\r');
        }
        console_putc(*s++);
    }
}

void console_writeln(const char *s)
{
    console_write(s);
    console_write("\n");
}

bool console_getc(char *out)
{
    const uint32_t tail = console_rx_tail;
    if (tail == console_rx_head)
    {
        return false;
    }
    *out = (char)console_rx_ring[tail];
    console_rx_tail = (tail + 1u) & (CONSOLE_RX_RING_SIZE - 1u);
    return true;
}

bool console_rx_ready(void)
{
    return console_rx_tail != console_rx_head;
}

void console_rx_flush(void)
{
    console_rx_tail = console_rx_head;
}

void console_rx_status_get(console_rx_status_t *out)
{
    if (out != NULL)
    {
        out->rx_isr_count = console_rx_isr_count;
        out->rx_byte_count = console_rx_byte_count;
        out->rx_ring_overflow_count = console_rx_ring_overflow_count;
    }
}

void console_rx_status_clear(void)
{
    console_rx_isr_count = 0u;
    console_rx_byte_count = 0u;
    console_rx_ring_overflow_count = 0u;
}

_Static_assert((CONSOLE_RX_RING_SIZE & (CONSOLE_RX_RING_SIZE - 1u)) == 0u,
               "console RX ring size must be a power of two");

/* ------------------------------------------------------------------ */
/* Minimal formatter                                                  */
/* ------------------------------------------------------------------ */

static void emit_unsigned(uint32_t value, uint32_t base, bool upper,
                          uint32_t width, char pad)
{
    char buf[11];
    uint32_t n = 0u;

    do
    {
        const uint32_t digit = value % base;
        buf[n++] = (char)((digit < 10u)
                              ? ('0' + digit)
                              : ((upper ? 'A' : 'a') + (digit - 10u)));
        value /= base;
    } while ((value != 0u) && (n < sizeof buf));

    while (n < width)
    {
        if (n >= sizeof buf)
        {
            break;
        }
        buf[n++] = pad;
    }

    while (n != 0u)
    {
        console_putc(buf[--n]);
    }
}

void console_printf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);

    while (*fmt != '\0')
    {
        if (*fmt != '%')
        {
            if (*fmt == '\n')
            {
                console_putc('\r');
            }
            console_putc(*fmt++);
            continue;
        }

        fmt++;      /* skip '%' */

        char     pad   = ' ';
        uint32_t width = 0u;
        uint32_t precision = 0u;
        bool     has_precision = false;
        bool     long_modifier = false;

        if (*fmt == '0')
        {
            pad = '0';
            fmt++;
        }
        while ((*fmt >= '0') && (*fmt <= '9'))
        {
            width = (width * 10u) + (uint32_t)(*fmt++ - '0');
        }

        if (*fmt == '.')
        {
            has_precision = true;
            fmt++;
            while ((*fmt >= '0') && (*fmt <= '9'))
            {
                precision = (precision * 10u) + (uint32_t)(*fmt++ - '0');
            }
        }

        if (*fmt == 'l')
        {
            long_modifier = true;
            fmt++;
        }

        switch (*fmt)
        {
        case 'c':
            console_putc((char)va_arg(ap, int));
            break;

        case 's':
        {
            const char *s = va_arg(ap, const char *);
            s = (s != NULL) ? s : "(null)";
            if (has_precision)
            {
                while ((*s != '\0') && (precision-- != 0u))
                {
                    console_putc(*s++);
                }
            }
            else
            {
                console_write(s);
            }
            break;
        }

        case 'u':
            emit_unsigned(long_modifier ? (uint32_t)va_arg(ap, unsigned long)
                                        : (uint32_t)va_arg(ap, unsigned int),
                          10u, false, width, pad);
            break;

        case 'd':
        {
            const int32_t v = long_modifier ? (int32_t)va_arg(ap, long)
                                             : (int32_t)va_arg(ap, int);
            if (v < 0)
            {
                console_putc('-');
                emit_unsigned((uint32_t)(-(v + 1)) + 1u, 10u, false, width, pad);
            }
            else
            {
                emit_unsigned((uint32_t)v, 10u, false, width, pad);
            }
            break;
        }

        case 'x':
            emit_unsigned(long_modifier ? (uint32_t)va_arg(ap, unsigned long)
                                        : (uint32_t)va_arg(ap, unsigned int),
                          16u, false, width, pad);
            break;

        case 'X':
            emit_unsigned(long_modifier ? (uint32_t)va_arg(ap, unsigned long)
                                        : (uint32_t)va_arg(ap, unsigned int),
                          16u, true, width, pad);
            break;

        case '%':
            console_putc('%');
            break;

        case '\0':
            va_end(ap);
            return;

        default:
            console_putc('%');
            console_putc(*fmt);
            break;
        }

        fmt++;
    }

    va_end(ap);
}
