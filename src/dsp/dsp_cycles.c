/*
 * dsp_cycles.c - see dsp_cycles.h.
 */
#include "app_config.h"

/*
 * This is measurement-only code, so the whole translation unit is behind
 * APP_ENABLE_DSP_BENCH. It compiles to nothing in the live audio
 * configurations.
 */

#if APP_ENABLE_DSP_BENCH || APP_ENABLE_AUDIO_DRC

#include "dsp_cycles.h"

#define SYSTICK_MASK    0x00FFFFFFu

static dsp_cycles_src_t s_src      = DSP_CYCLES_SRC_NONE;
static uint32_t         s_overhead = 0u;

uint32_t dsp_cycles_now(void)
{
    if (s_src == DSP_CYCLES_SRC_DWT)
    {
        return DWT->CYCCNT;
    }
    if (s_src == DSP_CYCLES_SRC_SYSTICK)
    {
        return SysTick->VAL & SYSTICK_MASK;
    }
    return 0u;
}

uint32_t dsp_cycles_delta(uint32_t start, uint32_t end)
{
    if (s_src == DSP_CYCLES_SRC_SYSTICK)
    {
        /* Counts down and reloads, so elapsed = start - end, modulo 24 bits. */
        return (start - end) & SYSTICK_MASK;
    }

    /* DWT counts up; unsigned subtraction is correct across the 32-bit wrap. */
    return end - start;
}

/* Does the active source actually move? A counter that is present but held
 * static reads as a constant, which would show up as a suspiciously round
 * "0 cycles" rather than as a failure - so require movement explicitly. */
static bool source_advances(void)
{
    const uint32_t a = dsp_cycles_now();

    for (uint32_t i = 0u; i < 64u; i++)
    {
        __asm__ volatile ("nop");
    }

    return dsp_cycles_delta(a, dsp_cycles_now()) != 0u;
}

static bool try_dwt(void)
{
    DCB->DEMCR |= DCB_DEMCR_TRCENA_Msk;
    __DSB();
    __ISB();

    /* NOCYCCNT set means the cycle counter is not implemented at all. */
    if ((DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk) != 0u)
    {
        return false;
    }

    DWT->CYCCNT = 0u;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    __DSB();

    s_src = DSP_CYCLES_SRC_DWT;
    if (source_advances())
    {
        return true;
    }

    /* Leave no half-enabled state behind for the fallback to trip over. */
    DWT->CTRL &= ~DWT_CTRL_CYCCNTENA_Msk;
    s_src = DSP_CYCLES_SRC_NONE;
    return false;
}

static bool try_systick(void)
{
    /*
     * Free-running over the full 24-bit range, CPU clock (CLKSOURCE = 1),
     * no interrupt. board_delay_*() will overwrite LOAD and then clear
     * CTRL, so anything measured after a delay call must re-init - which is
     * why the bench takes its timestamps with no delay in between.
     */
    SysTick->CTRL = 0u;
    SysTick->LOAD = SYSTICK_MASK;
    SysTick->VAL  = 0u;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_ENABLE_Msk;
    __DSB();

    s_src = DSP_CYCLES_SRC_SYSTICK;
    if (source_advances())
    {
        return true;
    }

    SysTick->CTRL = 0u;
    s_src = DSP_CYCLES_SRC_NONE;
    return false;
}

/* Cost of the timestamp pair itself, taken as the minimum of several
 * attempts: the minimum is the uninterrupted case, and an interrupted
 * sample would inflate the figure that later gets subtracted. */
static uint32_t measure_overhead(void)
{
    uint32_t best = 0xFFFFFFFFu;

    for (uint32_t i = 0u; i < 32u; i++)
    {
        const uint32_t a = dsp_cycles_now();
        const uint32_t b = dsp_cycles_now();
        const uint32_t d = dsp_cycles_delta(a, b);

        if (d < best)
        {
            best = d;
        }
    }

    return (best == 0xFFFFFFFFu) ? 0u : best;
}

bool dsp_cycles_init(void)
{
    s_src      = DSP_CYCLES_SRC_NONE;
    s_overhead = 0u;

    if (!try_dwt())
    {
        if (!try_systick())
        {
            return false;
        }
    }

    s_overhead = measure_overhead();
    return true;
}

dsp_cycles_src_t dsp_cycles_source(void)
{
    return s_src;
}

const char *dsp_cycles_source_name(void)
{
    switch (s_src)
    {
    case DSP_CYCLES_SRC_DWT:     return "DWT CYCCNT (32 bit, up)";
    case DSP_CYCLES_SRC_SYSTICK: return "SysTick VAL (24 bit, down)";
    default:                     return "none - no usable cycle counter";
    }
}

uint32_t dsp_cycles_span(void)
{
    switch (s_src)
    {
    case DSP_CYCLES_SRC_DWT:     return 0xFFFFFFFFu;
    case DSP_CYCLES_SRC_SYSTICK: return SYSTICK_MASK;
    default:                     return 0u;
    }
}

uint32_t dsp_cycles_overhead(void)
{
    return s_overhead;
}

#endif /* APP_ENABLE_DSP_BENCH || APP_ENABLE_AUDIO_DRC */
