/*
 * dsp_dwt_diag.h - DWT auxiliary counter diagnostic for the IIR benchmark.
 *
 * WHAT THIS IS FOR, AND WHAT IT IS NOT
 *
 * The official benchmark metric stays CYCCNT via dsp_cycles.c. CPICNT, LSUCNT,
 * FOLDCNT and EXCCNT are read in the SAME run and are reported TOGETHER: no
 * single auxiliary counter may be quoted as the reason for a timing. FOLDCNT is
 * the one that speaks to the issue floor, which is why it gets a dedicated probe
 * below, but it speaks to one term of a cycle account and not to the total - and
 * which counter an FP dependency stall lands in is itself unknown until measured
 * (the TRM defines CPICNT and LSUCNT in section 11.1, page 76, and says nothing
 * about FPU dependency). Deciding a cause from FOLDCNT alone would repeat what
 * prediction v1 did: take the quantity that was easy to obtain and treat it as
 * the whole answer.
 *
 * This module is diagnostic only: it reads the DWT's auxiliary counters around
 * a kernel run so the gap between the static instruction count and the
 * measured cycle count can be attributed instead of guessed at.
 * No number this module prints may be quoted as a benchmark result.
 *
 * WHY IT EXISTS AT ALL
 *
 * Prediction v2 rests on an unresolved question: whether the processor folds
 * any instruction pair in this hot loop. The Cortex-M33 TRM (Issue 08) says
 * only "Limited dual-issue of common 16-bit instruction pairs" (section 1.4.1,
 * page 13) and never enumerates the eligible pairs, so the issue floor cannot
 * be derived from the document. FOLDCNT answers it by measurement:
 *
 *   TRM section 11.1, page 76, "The DWT contains counters for:"
 *     - Cycles (CYCCNT).
 *     - Folded Instructions (FOLDCNT).
 *     - Additional cycles required to execute all load or store instructions
 *       (LSUCNT).
 *     - Processor sleep cycles (SLEEPCNT).
 *     - Additional cycles required to execute multi-cycle instructions and
 *       instruction fetch stalls (CPICNT)
 *     - Cycles spent in exception processing (EXCCNT).
 *
 * FOUR THINGS THIS MODULE REFUSES TO ASSUME
 *
 * 1. That the counters exist. TRM section 11.2, page 77: "Depending on the
 *    implementation of your processor, some of these registers might not be
 *    present. Any register that is configured as not present reads as zero."
 *    A missing counter therefore looks exactly like a counter reporting no
 *    stalls, which is the most dangerous failure mode available here. Every
 *    counter is proved to move before its value is believed.
 *
 * 2. That DWT_CTRL can be decoded. The TRM lists its possible reset values as
 *    0x2800000 / 0x2000000 (Reduced DWT) and 0x4800000 / 0x4000000 (Full DWT),
 *    which as printed put nothing in NUMCOMP[31:28] even though section 11.1
 *    says a Reduced DWT has two comparators and a Full DWT has four. The
 *    document is ambiguous, so this module prints the raw register and reports
 *    whether it matches, and never branches on a decode.
 *
 * 3. That the counters are 32 bits. The TRM defers the register descriptions
 *    to the Armv8-M Architecture Reference Manual (page 78), which this project
 *    does not hold, so the widths are unknown. The run is therefore chunked and
 *    the counters are drained between chunks, which is correct for any width,
 *    and the largest per-chunk value seen is reported so a narrow counter shows
 *    up as saturation rather than as a quiet undercount.
 *
 * 4. That reading the counters is free. The drain happens outside the timed
 *    region and its own cost is measured and reported separately.
 *
 * The DWT software lock at 0xFB0 is not touched, for the reason given in
 * dsp_cycles.h.
 */
#ifndef DSP_DWT_DIAG_H
#define DSP_DWT_DIAG_H

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"

/* What the hardware says about itself, before any workload runs. */
typedef struct
{
    uint32_t ctrl;              /* DWT->CTRL as read                        */
    uint32_t numcomp;           /* CTRL[31:28]                              */
    bool     nocyccnt;          /* CTRL[25], as named by CMSIS core_cm33.h  */
    bool     noprfcnt;          /* CTRL[24], likewise                       */
    bool     ctrl_matches_trm;  /* equal to one of the four listed values    */
    bool     trcena;            /* DEMCR[24] after this module sets it      */

    /* Proved by observation, not by decoding: each counter is zeroed, a small
     * known workload runs, and the counter is read back. */
    bool cyccnt_moves;
    bool cpicnt_moves;
    bool exccnt_moves;
    bool lsucnt_moves;
    bool foldcnt_moves;
    bool sleepcnt_moves;
} dsp_dwt_caps_t;

/* One drained sample of the auxiliary counters. */
typedef struct
{
    uint32_t cpi;
    uint32_t exc;
    uint32_t lsu;
    uint32_t fold;
    uint32_t sleep;

    /* Largest single-chunk value seen for each, so a counter that is narrower
     * than the chunk needs shows up as a value pinned near its ceiling. */
    uint32_t cpi_chunk_max;
    uint32_t exc_chunk_max;
    uint32_t lsu_chunk_max;
    uint32_t fold_chunk_max;

    uint32_t chunks;
} dsp_dwt_acc_t;

/* Sets DEMCR.TRCENA, probes what is present, and proves what moves. Safe to
 * call more than once. Returns false when CYCCNT itself does not advance, in
 * which case the caller must not report a cycle accounting. */
bool dsp_dwt_diag_probe(dsp_dwt_caps_t *caps);

void dsp_dwt_diag_print_caps(const dsp_dwt_caps_t *caps);

/* Zeroes the auxiliary counters. Call immediately before a chunk. */
void dsp_dwt_diag_zero(void);

/* Reads the auxiliary counters, adds them into `acc`, and zeroes them again.
 * Call immediately after a chunk, outside any timed region. */
void dsp_dwt_diag_drain(dsp_dwt_acc_t *acc);

/* Cost in cycles of one zero/drain pair, measured at probe time. Reported so
 * that it is visibly not folded into the kernel figure. */
uint32_t dsp_dwt_diag_overhead(void);

void dsp_dwt_diag_print_acc(const dsp_dwt_acc_t *acc,
                            const dsp_dwt_caps_t *caps,
                            uint32_t kernel_cycles,
                            uint32_t instructions_issued);

#endif /* DSP_DWT_DIAG_H */
