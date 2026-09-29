/*
 * dsp_cycles.h - a CPU cycle counter for the isolated benchmark.
 *
 * The module reports an unavailable or non-running counter explicitly rather
 * than returning a plausible-looking zero.
 *
 * Two sources, in order of preference:
 *
 *   DWT CYCCNT   32-bit, free-running, 1 count per CPU cycle. The right
 *                instrument. It lives in the debug block, so it needs
 *                DCB->DEMCR.TRCENA set, and on some parts it does not run
 *                at all unless a debugger has enabled trace. DWT->CTRL
 *                .NOCYCCNT tells us whether the counter is even
 *                implemented; whether it *runs* is then checked by reading
 *                it twice and requiring it to have moved.
 *
 *   SysTick      24-bit, counts DOWN, reloads. Always present, needs no
 *                debug enablement. 24 bits is 16.78 Mcycle = 139.8 ms at
 *                120 MHz, which is four orders of magnitude more than one
 *                measured block, so the narrower counter costs nothing
 *                here. board_delay_ms()/us() also use SysTick, but only
 *                inside the call and they leave CTRL = 0 on exit, so the
 *                two uses do not overlap as long as the bench does not
 *                call a delay inside a measured region. It does not.
 *
 * The DWT software lock (LAR/LSR at 0xFB0) is deliberately NOT touched.
 * It is not in the CMSIS DWT_Type for Cortex-M33, writing an unlock key to
 * a register that may not be implemented is exactly the kind of write that
 * hard-faults, and the SysTick fallback removes any need to gamble on it.
 */
#ifndef DSP_CYCLES_H
#define DSP_CYCLES_H

#include <stdbool.h>
#include <stdint.h>

#include <xc.h>

typedef enum
{
    DSP_CYCLES_SRC_NONE = 0,
    DSP_CYCLES_SRC_DWT,
    DSP_CYCLES_SRC_SYSTICK,
} dsp_cycles_src_t;

/* Selects and starts a source, then proves it advances. Returns false and
 * leaves the source at DSP_CYCLES_SRC_NONE when neither counter moves, in
 * which case the bench must refuse to report timings. */
bool dsp_cycles_init(void);

dsp_cycles_src_t dsp_cycles_source(void);
const char      *dsp_cycles_source_name(void);

/* Largest interval this source can measure without ambiguity. */
uint32_t dsp_cycles_span(void);

/* Cost of one dsp_cycles_now() ... dsp_cycles_now() pair, measured at
 * init. The bench subtracts it so that a short measurement is not mostly
 * its own instrumentation. */
uint32_t dsp_cycles_overhead(void);

/* Raw counter read. Cheap, but not free: the source is a runtime choice,
 * so this is a load plus a branch. See dsp_cycles_overhead(). */
uint32_t dsp_cycles_now(void);

/* Cycles from `start` to `end`, handling the direction and the width of
 * whichever source is active. Wrap is indistinguishable from a short
 * interval on a counter this narrow, so the caller must keep measured
 * regions well inside dsp_cycles_span(); the bench checks that. */
uint32_t dsp_cycles_delta(uint32_t start, uint32_t end);

#endif /* DSP_CYCLES_H */
