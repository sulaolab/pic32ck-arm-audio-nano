/*
 * dsp_dwt_diag.c - see dsp_dwt_diag.h.
 */
#include "app_config.h"

#if APP_ENABLE_DSP_BENCH && APP_ENABLE_DWT_DIAG

#include <xc.h>

#include "drivers/console.h"

#include "dsp_cycles.h"
#include "dsp_dwt_diag.h"

/*
 * The four DWT_CTRL reset values the TRM lists, quoted exactly as printed in
 * Table 11-1 (Issue 08, page 77). They are used only to answer "does this part
 * report one of the documented values?" - never to decide what is implemented.
 * As printed they leave NUMCOMP[31:28] at zero, which cannot be reconciled with
 * section 11.1's "a Reduced DWT contains two comparators ... a Full DWT
 * contains four comparators", so a decode built on them would be a decode
 * built on a document ambiguity.
 */
static const uint32_t k_trm_ctrl_reset[] = {
    0x2800000u, /* Reduced DWT with no ITM trace */
    0x2000000u, /* Reduced DWT with ITM trace    */
    0x4800000u, /* Full DWT with no ITM trace    */
    0x4000000u, /* Full DWT with ITM trace       */
};

/* Keep a local fallback for packs that omit these CMSIS DWT field definitions,
 * without redefining the pack's API when they are supplied. */
#ifndef DWT_CTRL_NUMCOMP_Pos
#define DWT_CTRL_NUMCOMP_Pos    28u
#endif
#ifndef DWT_CTRL_NUMCOMP_Msk
#define DWT_CTRL_NUMCOMP_Msk    (0xFu << DWT_CTRL_NUMCOMP_Pos)
#endif

/*
 * Iterations of the folding probe, and the most disjoint adjacent 16-bit pairs
 * one iteration can offer. Verified in the disassembly of this build, not
 * assumed: the loop body assembles to six consecutive 16-bit encodings
 *
 *     2001 movs r0,#1 / 3001 adds r0,#1 / 2102 movs r1,#2
 *     3101 adds r1,#1 / 3b01 subs r3,#1 / d1f9 bne.n
 *
 * so there are five adjacent-pair positions and at most three disjoint folds.
 * See fold_probe().
 */
#define FOLD_PROBE_ITERATIONS   64u
#define FOLD_PROBE_PAIRS_EACH   3u

static uint32_t s_overhead;

/* ------------------------------------------------------------------ */
/* Workloads used only to prove a counter moves                        */
/* ------------------------------------------------------------------ */
static volatile uint32_t s_sink;
static volatile uint32_t s_touch[16];

/*
 * A few hundred cycles of loads, stores and taken branches. Enough for CYCCNT
 * and LSUCNT to be non-zero on any implementation that has them, and it says
 * nothing about folding.
 */
static void generic_probe(void)
{
    uint32_t acc = 0u;

    for (uint32_t i = 0u; i < 16u; i++)
    {
        s_touch[i] = i * 3u;
    }
    for (uint32_t i = 0u; i < 16u; i++)
    {
        acc += s_touch[i];
    }
    s_sink = acc;
}

/*
 * The probe that makes FOLDCNT interpretable.
 *
 * FOLDCNT reading zero around the IIR hot loop has two possible meanings: the
 * counter is not implemented (TRM page 77: a register that is not present
 * "reads as zero"), or it is implemented and nothing was folded. Those two
 * lead to opposite conclusions about the issue floor, so they have to be told
 * apart, and only a workload that the documented mechanism *should* fold can
 * do it.
 *
 * The TRM's only statement is "Limited dual-issue of common 16-bit instruction
 * pairs" (section 1.4.1, page 13) and it never enumerates the eligible pairs.
 * So this loop is written in assembly to guarantee adjacent 16-bit encodings -
 * movs/adds on low registers, which are as common a 16-bit pair as Thumb has.
 * If FOLDCNT still does not move here, the counter or the mechanism is not
 * available to us and prediction v2's issue floor stays unproven. That is a
 * reported outcome, not a failure.
 */
static void fold_probe(void)
{
    uint32_t n = FOLD_PROBE_ITERATIONS;

    __asm volatile(
        "1:                     \n"
        "   movs  r0, #1        \n"
        "   adds  r0, #1        \n"
        "   movs  r1, #2        \n"
        "   adds  r1, #1        \n"
        "   subs  %0, %0, #1    \n"
        "   bne   1b            \n"
        : "+r"(n)
        :
        : "r0", "r1", "cc");

    s_sink = n;
}

/* ------------------------------------------------------------------ */
/* Counter plumbing                                                    */
/* ------------------------------------------------------------------ */
void dsp_dwt_diag_zero(void)
{
    DWT->CPICNT   = 0u;
    DWT->EXCCNT   = 0u;
    DWT->LSUCNT   = 0u;
    DWT->FOLDCNT  = 0u;
    DWT->SLEEPCNT = 0u;
}

static void chunk_max(uint32_t *dst, uint32_t v)
{
    if (v > *dst)
    {
        *dst = v;
    }
}

void dsp_dwt_diag_drain(dsp_dwt_acc_t *acc)
{
    const uint32_t cpi   = DWT->CPICNT;
    const uint32_t exc   = DWT->EXCCNT;
    const uint32_t lsu   = DWT->LSUCNT;
    const uint32_t fold  = DWT->FOLDCNT;
    const uint32_t sleep = DWT->SLEEPCNT;

    dsp_dwt_diag_zero();

    acc->cpi   += cpi;
    acc->exc   += exc;
    acc->lsu   += lsu;
    acc->fold  += fold;
    acc->sleep += sleep;

    chunk_max(&acc->cpi_chunk_max, cpi);
    chunk_max(&acc->exc_chunk_max, exc);
    chunk_max(&acc->lsu_chunk_max, lsu);
    chunk_max(&acc->fold_chunk_max, fold);

    acc->chunks++;
}

uint32_t dsp_dwt_diag_overhead(void)
{
    return s_overhead;
}

/* ------------------------------------------------------------------ */
/* Probe                                                               */
/* ------------------------------------------------------------------ */
bool dsp_dwt_diag_probe(dsp_dwt_caps_t *caps)
{
    for (uint32_t i = 0u; i < sizeof(*caps); i++)
    {
        ((uint8_t *)caps)[i] = 0u;
    }

    DCB->DEMCR |= DCB_DEMCR_TRCENA_Msk;
    caps->trcena = (DCB->DEMCR & DCB_DEMCR_TRCENA_Msk) != 0u;

    caps->ctrl     = DWT->CTRL;
    caps->numcomp  = (caps->ctrl & DWT_CTRL_NUMCOMP_Msk) >> DWT_CTRL_NUMCOMP_Pos;
    caps->nocyccnt = (caps->ctrl & DWT_CTRL_NOCYCCNT_Msk) != 0u;
    caps->noprfcnt = (caps->ctrl & DWT_CTRL_NOPRFCNT_Msk) != 0u;

    /* Only the read-only capability bits are compared: CYCCNTENA and the event
     * enables are writable, and dsp_cycles_init() has already set one of them,
     * so comparing the whole register would never match. */
    const uint32_t ro = caps->ctrl & 0xFF000000u;
    for (uint32_t i = 0u; i < (sizeof(k_trm_ctrl_reset) / sizeof(k_trm_ctrl_reset[0])); i++)
    {
        if (ro == (k_trm_ctrl_reset[i] & 0xFF000000u))
        {
            caps->ctrl_matches_trm = true;
        }
    }

    DWT->CYCCNT = 0u;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    dsp_dwt_diag_zero();

    const uint32_t c0 = DWT->CYCCNT;
    generic_probe();
    const uint32_t c1 = DWT->CYCCNT;

    caps->cyccnt_moves   = (c1 != c0);
    caps->cpicnt_moves   = (DWT->CPICNT != 0u);
    caps->exccnt_moves   = (DWT->EXCCNT != 0u);
    caps->lsucnt_moves   = (DWT->LSUCNT != 0u);
    caps->sleepcnt_moves = (DWT->SLEEPCNT != 0u);

    /* Folding gets its own workload, for the reason in fold_probe(). */
    dsp_dwt_diag_zero();
    fold_probe();
    caps->foldcnt_moves = (DWT->FOLDCNT != 0u);

    /* Cost of one zero/drain pair, so it is visibly excluded from the kernel
     * figure rather than silently inside it. */
    dsp_dwt_acc_t scratch;
    for (uint32_t i = 0u; i < sizeof(scratch); i++)
    {
        ((uint8_t *)&scratch)[i] = 0u;
    }

    const uint32_t o0 = dsp_cycles_now();
    dsp_dwt_diag_zero();
    dsp_dwt_diag_drain(&scratch);
    const uint32_t o1 = dsp_cycles_now();

    s_overhead = dsp_cycles_delta(o0, o1);
    if (s_overhead > dsp_cycles_overhead())
    {
        s_overhead -= dsp_cycles_overhead();
    }

    dsp_dwt_diag_zero();

    return caps->cyccnt_moves;
}

/* ------------------------------------------------------------------ */
/* Reporting                                                           */
/* ------------------------------------------------------------------ */
static const char *yes_no(bool v)
{
    return v ? "yes" : "no";
}

void dsp_dwt_diag_print_caps(const dsp_dwt_caps_t *caps)
{
    console_writeln("DWT diagnostic capability probe (diagnostic only, never a"
                    " benchmark metric)");
    console_writeln("  CYCCNT is the official metric. The counters below are read"
                    " in the same run and");
    console_writeln("  reported together; none of them alone explains a timing.");
    console_printf("  DWT->CTRL         : 0x%08X\n", caps->ctrl);
    console_printf("  NUMCOMP[31:28]    : %u\n", caps->numcomp);
    console_printf("  NOCYCCNT / NOPRFCNT: %s / %s\n",
                   yes_no(caps->nocyccnt), yes_no(caps->noprfcnt));
    console_printf("  matches a TRM-listed reset value (RO bits): %s\n",
                   yes_no(caps->ctrl_matches_trm));
    console_printf("  DEMCR.TRCENA      : %s\n", yes_no(caps->trcena));
    console_writeln("  counters proved to move by a workload:");
    console_printf("    CYCCNT %s   CPICNT %s   EXCCNT %s\n",
                   yes_no(caps->cyccnt_moves), yes_no(caps->cpicnt_moves),
                   yes_no(caps->exccnt_moves));
    console_printf("    LSUCNT %s   SLEEPCNT %s\n",
                   yes_no(caps->lsucnt_moves), yes_no(caps->sleepcnt_moves));
    console_printf("    FOLDCNT %s  (dedicated %u x %u adjacent 16-bit pairs)\n",
                   yes_no(caps->foldcnt_moves),
                   (uint32_t)FOLD_PROBE_ITERATIONS,
                   (uint32_t)FOLD_PROBE_PAIRS_EACH);

    if (!caps->foldcnt_moves)
    {
        console_writeln("  *** FOLDCNT did not move even for a workload built"
                        " out of adjacent 16-bit");
        console_writeln("  *** pairs. Not implemented and 'nothing folded' are"
                        " then indistinguishable,");
        console_writeln("  *** so prediction v2's issue floor stays UNPROVEN."
                        " Do not upgrade it.");
    }
    console_printf("  zero+drain overhead: %u cycles (excluded from kernel"
                   " figures)\n", s_overhead);
}

void dsp_dwt_diag_print_acc(const dsp_dwt_acc_t *acc,
                            const dsp_dwt_caps_t *caps,
                            uint32_t kernel_cycles,
                            uint32_t instructions_issued)
{
    console_printf("  chunks            : %u\n", acc->chunks);
    console_printf("  kernel cycles     : %u (CYCCNT, the official metric)\n",
                   kernel_cycles);
    console_printf("  instructions issued (static, from the disassembly): %u\n",
                   instructions_issued);
    console_printf("  FOLDCNT %u (max/chunk %u)   CPICNT %u (max/chunk %u)\n",
                   acc->fold, acc->fold_chunk_max, acc->cpi, acc->cpi_chunk_max);
    console_printf("  LSUCNT  %u (max/chunk %u)   EXCCNT %u (max/chunk %u)\n",
                   acc->lsu, acc->lsu_chunk_max, acc->exc, acc->exc_chunk_max);
    console_printf("  SLEEPCNT %u\n", acc->sleep);

    /*
     * A cycle accounting, presented as a model and not as a TRM statement.
     * The TRM defines each counter's meaning (section 11.1, page 76) but never
     * writes an identity relating them to the cycle count, so the residual
     * below is exactly the part this project cannot yet explain - which is the
     * useful number, not an error.
     */
    const uint32_t explained = instructions_issued
                             + acc->cpi + acc->lsu + acc->exc
                             - ((acc->fold <= instructions_issued) ? acc->fold : 0u);

    console_writeln("  model: cycles ~= issued - FOLDCNT + CPICNT + LSUCNT +"
                    " EXCCNT  (not a TRM identity)");
    if (kernel_cycles >= explained)
    {
        console_printf("    accounted %u, unexplained +%u cycles\n",
                       explained, kernel_cycles - explained);
        console_writeln("    unexplained cycles are where FPU dependency"
                        " latency would land.");
    }
    else
    {
        console_printf("    accounted %u, which EXCEEDS the measured %u by %u\n",
                       explained, kernel_cycles, explained - kernel_cycles);
        console_writeln("    *** The model over-counts. Either a counter is"
                        " narrower than a chunk");
        console_writeln("    *** (check max/chunk above) or the accounting is"
                        " wrong. Do not report.");
    }

    if (acc->fold == 0u && caps->foldcnt_moves)
    {
        console_writeln("  FOLDCNT = 0 over the whole run, and FOLDCNT is known"
                        " to work on this part:");
        console_writeln("  no instruction pair in this kernel was folded. That is"
                        " NECESSARY but NOT");
        console_writeln("  SUFFICIENT to call the issue floor measured - it only"
                        " removes folding as an");
        console_writeln("  explanation. Read it with the account above, never on"
                        " its own.");
    }
}

#endif /* APP_ENABLE_DSP_BENCH && APP_ENABLE_DWT_DIAG */
