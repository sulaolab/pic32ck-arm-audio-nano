/*
 * dsp_bench.h - isolated benchmark and correctness harness for
 *               the CMSIS-DSP float32 DF2T biquad cascade on Cortex-M33.
 *
 * Purpose: measure the Cortex-M33 IIR kernel with no audio hardware in the
 * loop.
 *
 * WHAT IS MEASURED, EXACTLY
 * -------------------------
 *   synthetic channel-major float32 block
 *          -> N second-order sections, per channel
 *          -> arm_biquad_cascade_df2T_f32()
 *          -> output block
 *
 * and nothing else. No TDM slot gather, no int32-to-float conversion, no
 * scatter into a transmit buffer, no ring, no observation code inside the
 * timed region. That is the point - and it is also the limit:
 *
 *   ***  The number this harness produces is a FLOOR for the cost of an   ***
 *   ***  IIR stage in a real signal chain, not a budget for one.          ***
 *
 * The integrated audio cost also includes gather, scatter and transport
 * instrumentation. Evaluate that cost with two live images, one with the
 * processing stage and one without it.
 *
 * Reporting therefore always states min AND mean. Min is the
 * uninterrupted cost of the kernel; mean includes whatever else the core
 * did. Adding a mean to a load figure that already accounts for interrupts
 * double-counts them, and quoting a min as a budget under-counts. Both are
 * printed so neither substitution is convenient.
 *
 * The known-answer test validates arithmetic before a timing result is shown.
 * The timing banner identifies the selected compiler and kernel settings.
 */
#ifndef DSP_BENCH_H
#define DSP_BENCH_H

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "dsp_iir.h"

/*
 * Which coefficient bank a measurement ran on.
 *
 * BW12_LP is the 6-section Butterworth: the default operating point and the
 * arithmetic check. AP84 is the 84-section allpass bank, the only one long
 * enough to exercise the long-cascade geometry - see dsp_coeffs.h for
 * why a low-pass cannot be used there.
 *
 * The bank is part of every reported result, because a cycles-per-section
 * figure from one bank is not interchangeable with one from the other.
 */
typedef enum
{
    DSP_BENCH_BANK_BW12_LP = 0,
    DSP_BENCH_BANK_AP84,
} dsp_bench_bank_t;

typedef struct
{
    bool     valid;         /* false when no usable cycle counter, or when
                             * the measured interval did not fit the
                             * counter - never a zero passed off as a
                             * measurement */
    uint32_t channels;
    uint32_t frames;
    uint32_t stages;
    uint32_t iterations;
    const char *bank;       /* which coefficient bank produced this        */

    uint32_t cycles_min;    /* per block, instrumentation subtracted */
    uint32_t cycles_mean;
    uint32_t cycles_max;
    uint32_t overhead;      /* what was subtracted, so it can be judged */
} dsp_bench_result_t;

/* Cycle counter selection plus the coefficient bit-pattern check. Prints
 * what it found. Call once, after console_init(). */
bool dsp_bench_init(void);

/* Build facts that change the numbers: optimisation level, whether
 * ARM_MATH_LOOPUNROLL was defined, FPU/DSP-extension presence, cycle
 * source. Printed rather than assumed, because "which build was that?" is
 * the first thing a surprising measurement raises. */
void dsp_bench_print_build_info(void);

/*
 * Known-answer test plus the fault injections that prove it can fail.
 *
 * Runs the 8 x 32 x 6 vectors from dsp_vectors.c and requires the residual
 * against the double-precision reference to be within DSP_VECTORS_TOLERANCE.
 * Then re-runs it once per entry in dsp_iir_fault_t and requires every one
 * of those to exceed the tolerance. A tolerance that cannot fail is not a
 * test, so the negative half is not optional and its result is part of the
 * verdict this returns.
 */
bool dsp_bench_self_test(void);

/* One measurement at the given geometry. Returns false only for a
 * geometry that does not fit or a counter that could not be used; a
 * completed run with an unusable timing still reports valid = false. */
bool dsp_bench_run(uint32_t channels,
                   uint32_t frames,
                   uint32_t stages,
                   uint32_t iterations,
                   dsp_bench_result_t *out);

/* As dsp_bench_run(), naming the coefficient bank. dsp_bench_run() is this
 * with DSP_BENCH_BANK_BW12_LP, which is what every pre-existing call site
 * wants and why its signature did not change. */
bool dsp_bench_run_bank(uint32_t channels,
                        uint32_t frames,
                        uint32_t stages,
                        uint32_t iterations,
                        dsp_bench_bank_t bank,
                        dsp_bench_result_t *out);

/* Human-readable single measurement, including cycles per sample per
 * stage, cycles per MAC, microseconds and the share of one 32-frame block
 * period at 48 kHz. */
void dsp_bench_print_result(const dsp_bench_result_t *r);

/* Sweeps. Stages 1..DSP_BENCH_MAX_STAGES at the default channel count, and
 * channels 1..DSP_BENCH_MAX_CHANNELS at the default stage count. A sweep is
 * what makes the fixed per-call overhead visible: a single operating point
 * cannot separate it from the per-sample cost. */
void dsp_bench_sweep_stages(void);
void dsp_bench_sweep_channels(void);

/* Cortex-M33 FPv5-SP-D16 issue/latency probe.  This is deliberately separate
 * from the DF2T result: it runs fixed hand-written instruction streams to
 * distinguish dependent latency from independent throughput. */
void dsp_bench_run_fpu_probe(void);

/*
 * The challenge point: DSP_TIER2_DSP_CHANNELS x 32 frames x
 * DSP_BENCH_CHALLENGE_STAGES, the geometry of the verified Sonora
 * Classic/DRC record (84 sections x 4 channels = 336 biquads). Prints N_max
 * and the feasibility threshold this build's clock implies.
 */
void dsp_bench_run_challenge(void);

/* ------------------------------------------------------------------ */
/* Tier 2 - the DRC-equivalent DSP block                               */
/* ------------------------------------------------------------------ */
/*
 * Known-answer test for the whole Tier 2 chain against the generated
 * int32-in/int32-out vectors, plus the fault injections that prove it can
 * fail: the two Tier-2-specific defects and the kernel ones underneath.
 */
bool dsp_bench_self_test_tier2(void);

/*
 * Times the whole Tier 2 chain at the given cascade length, and prints it
 * beside a Tier 1 run of the same cascade so the difference - the conversion
 * and expansion cost - is visible rather than inferred.
 */
void dsp_bench_run_tier2(uint32_t stages);

#if APP_ENABLE_DWT_DIAG
/*
 * DWT auxiliary counter diagnostic. Prints the capability probe, then a cycle
 * accounting (FOLDCNT / CPICNT / LSUCNT / EXCCNT) for Tier 1 at 1, 6 and 84
 * sections. Diagnostic only: nothing it prints is a benchmark result, and it
 * does not touch the timed region dsp_bench_run() uses. Its reason for
 * existing is FOLDCNT, which is the only way to find out whether this kernel
 * benefits from the limited dual issue the Cortex-M33 TRM mentions but never
 * specifies.
 */
void dsp_bench_run_dwt_diag(void);
#endif

#endif /* DSP_BENCH_H */
