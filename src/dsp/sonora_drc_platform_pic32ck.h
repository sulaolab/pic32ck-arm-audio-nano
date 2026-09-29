/*
 * PIC32CK platform seam for imported Sonora Classic/DRC modules.
 *
 * Keep the legacy platform-facing names here so that the imported state
 * machines do not grow PIC32CK-specific branches. This header is deliberately local
 * to the DRC import: transport, console and timing ownership remain with the
 * existing PIC32CK platform layers.
 */
#ifndef SONORA_DRC_PLATFORM_PIC32CK_H
#define SONORA_DRC_PLATFORM_PIC32CK_H

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"

#if APP_ENABLE_AUDIO_DRC

#include "audio_transport.h"
#include "console.h"
#include "dsp_cycles.h"

/* Sonora's UART status vocabulary, projected onto the one PIC32CK console
 * endpoint.  SERCOM5 exposes an ISR ring and ring-overflow count; it does not
 * currently retain per-byte FIFO/framing/parity counters, so those fields are
 * reported as zero rather than invented. */
typedef enum
{
    NORA_UART_OK = 0,
    NORA_UART_ERR_UNSUPPORTED,
} nora_uart_status_t;

typedef uint8_t nora_uart_instance_t;

typedef enum
{
    NORA_UART_RX_MODE_POLLING = 0,
    NORA_UART_RX_MODE_ISR_RING,
} nora_uart_rx_mode_t;

typedef struct
{
    nora_uart_rx_mode_t rx_mode;
    uint32_t rx_isr_count;
    uint32_t rx_byte_count;
    uint32_t rx_fifo_overflow_count;
    uint32_t framing_error_count;
    uint32_t parity_error_count;
    uint32_t autobaud_overflow_count;
    uint32_t tx_collision_count;
    uint32_t rx_ring_overflow_count;
    uint32_t rx_max_drain_count;
    uint32_t rx_stall_recovery_count;
    uint32_t rx_ie_lost_count;
    uint32_t rx_overrun_recovered_count;
    uint32_t rx_integrity_fault_count;
} nora_uart_rx_status_t;

#define UART_PLATFORM_UART1_USB_SERIAL_PORT_INST ((nora_uart_instance_t)0u)

static inline nora_uart_status_t nora_uart_rx_status_get(
    nora_uart_instance_t inst, nora_uart_rx_status_t *out)
{
    if ((inst != UART_PLATFORM_UART1_USB_SERIAL_PORT_INST) || (out == NULL))
    {
        return NORA_UART_ERR_UNSUPPORTED;
    }

    console_rx_status_t status;
    console_rx_status_get(&status);

    out->rx_mode = NORA_UART_RX_MODE_ISR_RING;
    out->rx_isr_count = status.rx_isr_count;
    out->rx_byte_count = status.rx_byte_count;
    out->rx_fifo_overflow_count = 0u;
    out->framing_error_count = 0u;
    out->parity_error_count = 0u;
    out->autobaud_overflow_count = 0u;
    out->tx_collision_count = 0u;
    out->rx_ring_overflow_count = status.rx_ring_overflow_count;
    out->rx_max_drain_count = 0u;
    out->rx_stall_recovery_count = 0u;
    out->rx_ie_lost_count = 0u;
    out->rx_overrun_recovered_count = 0u;
    out->rx_integrity_fault_count = status.rx_ring_overflow_count;
    return NORA_UART_OK;
}

static inline uint32_t nora_uart_rx_integrity_fault_count(
    nora_uart_instance_t inst)
{
    nora_uart_rx_status_t status;
    return (nora_uart_rx_status_get(inst, &status) == NORA_UART_OK)
               ? status.rx_integrity_fault_count
               : 0u;
}

/* The imported CSV watchdog needs a monotonic millisecond clock.  The DRC
 * path already initializes dsp_cycles before the transport is exposed.  Keep
 * an extended millisecond accumulator here so a DWT wrap cannot manufacture a
 * false two-second inactivity timeout. */
static inline uint32_t sonora_drc_platform_tick_ms(void)
{
    static bool initialized;
    static uint32_t previous;
    static uint32_t remainder_cycles;
    static uint32_t elapsed_ms;
    const uint32_t now = dsp_cycles_now();
    const uint32_t cycles_per_ms = APP_CPU_CLOCK_HZ / 1000u;

    if (!initialized)
    {
        initialized = true;
        previous = now;
        return elapsed_ms;
    }

    if ((dsp_cycles_source() != DSP_CYCLES_SRC_NONE) &&
        (cycles_per_ms != 0u))
    {
        const uint32_t delta = dsp_cycles_delta(previous, now);
        const uint64_t total = (uint64_t)remainder_cycles + delta;

        elapsed_ms += (uint32_t)(total / cycles_per_ms);
        remainder_cycles = (uint32_t)(total % cycles_per_ms);
    }
    previous = now;
    return elapsed_ms;
}

static inline uint32_t sonora_drc_platform_high_res_count(void)
{
    return dsp_cycles_now();
}

static inline uint32_t sonora_drc_platform_elapsed_us(uint32_t start)
{
    const uint32_t cycles = dsp_cycles_delta(start, dsp_cycles_now());
    return (uint32_t)(((uint64_t)cycles * 1000000u) / APP_CPU_CLOCK_HZ);
}

static inline bool nora_spi_i2s_tdm_is_running(void)
{
    return audio_transport_is_running();
}

/* Sonora consumes any residual delimiter/queued payload after a completed CSV
 * END marker before handing normal command input back to the console.  The
 * PIC32CK console owns the only RX ring, so drain that same ownership point. */
static inline void sonora_drc_platform_print_uart_hw_diag(const char *tag)
{
#if defined(SONORA_DRC_HOST_TEST)
    (void)tag;
#else
    sercom_usart_int_registers_t *const u = &SERCOM5_REGS->USART_INT;
    const unsigned int rxie =
        ((u->SERCOM_INTENSET & SERCOM_USART_INT_INTENSET_RXC_Msk) != 0u) ? 1u : 0u;
    const unsigned int rxif =
        ((u->SERCOM_INTFLAG & SERCOM_USART_INT_INTFLAG_RXC_Msk) != 0u) ? 1u : 0u;

    console_printf("BIQUAD CSV UART HW %s: ie=%u if=%u u1stat=%08lX\n",
                   (tag != NULL) ? tag : "", rxie, rxif,
                   (unsigned long)u->SERCOM_STATUS);
#endif
}

/* Preserve Sonora timing spellings at the PIC32CK platform boundary. */
#define GetTicks()                       sonora_drc_platform_tick_ms()
#define nora_high_res_timer_get_count()  sonora_drc_platform_high_res_count()
#define nora_high_res_timer_elapsed_us(t) sonora_drc_platform_elapsed_us((t))

#endif /* APP_ENABLE_AUDIO_DRC */
#endif /* SONORA_DRC_PLATFORM_PIC32CK_H */
