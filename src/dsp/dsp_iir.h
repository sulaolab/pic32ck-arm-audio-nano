/*
 * dsp_iir.h - the multichannel IIR kernel under test.
 *
 * One choice, made once and named here: the kernel is CMSIS-DSP
 * arm_biquad_cascade_df2T_f32(), transposed direct form II, float32.
 * Everything in this file is a thin, honest wrapper around it - no
 * reimplementation, no "optimised variant", nothing that would make the
 * measured number belong to this project rather than to CMSIS.
 *
 * Why DF2T and not DF1: for float32, transposed direct form II is the form
 * CMSIS itself recommends, because it needs 2 state words per stage
 * instead of 4 and its accumulator ordering suits a fused multiply-add.
 * DF1 is the right choice for the fixed-point (q31/q15) variants, where
 * DF2T is numerically worse. This is the float32 comparison, so DF2T
 * it is; a q31 DF1 arm is a separate exercise and is deliberately out of
 * scope for this float32 DF2T benchmark.
 *
 * Buffer layout is CHANNEL-MAJOR with a fixed stride:
 *
 *     sample n of channel c  ==  buf[(c * DSP_IIR_FRAME_STRIDE) + n]
 *
 * The stride is a compile-time constant rather than the runtime frame
 * count, so sweeping the frame count cannot silently change the memory
 * access pattern underneath a timing comparison. One
 * arm_biquad_cascade_df2T_f32() call per channel walks that channel
 * contiguously, which is what the kernel wants.
 *
 * State is PER CHANNEL. Coefficients are SHARED. This split is the whole
 * correctness question in a multichannel wrapper, so it has its own
 * negative test: DSP_IIR_FAULT_SHARED_STATE aliases every channel onto
 * channel 0 and the known-answer test must then fail.
 *
 * This translation unit is host-portable on purpose - it includes no
 * device header and touches no register - so a host-side correctness test
 * builds this exact source with a host compiler. The comparison uses firmware
 * source verbatim rather than a rewritten model.
 */
#ifndef DSP_IIR_H
#define DSP_IIR_H

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "dsp/filtering_functions.h"

#define DSP_IIR_COEFFS_PER_STAGE    5u      /* {b0, b1, b2, -a1, -a2}   */
#define DSP_IIR_STATE_PER_STAGE     2u      /* d1, d2                   */
#define DSP_IIR_MACS_PER_SAMPLE_STAGE 5u    /* DF2T: 5 mul, 4 add       */

#define DSP_IIR_FRAME_STRIDE        DSP_BENCH_MAX_FRAMES

/*
 * Deliberate defects, for proving the known-answer test can fail.
 *
 * A guard is not demonstrated by having been written. Each of these is
 * injected in turn by dsp_bench_self_test(), which requires every one of
 * them to be caught; a tolerance loose enough to pass them all would
 * otherwise look exactly like a tolerance that works.
 */
typedef enum
{
    DSP_IIR_FAULT_NONE = 0,
    DSP_IIR_FAULT_COEFF,            /* perturb one feedback coefficient  */
    DSP_IIR_FAULT_SHARED_STATE,     /* alias all channels onto channel 0 */
    DSP_IIR_FAULT_STAGE_COUNT,      /* run one stage short               */
    DSP_IIR_FAULT_COUNT,
} dsp_iir_fault_t;

typedef struct
{
    uint32_t channels;
    uint32_t stages;
    dsp_iir_fault_t fault;

    arm_biquad_cascade_df2T_instance_f32 inst[DSP_BENCH_MAX_CHANNELS];

    /* Per channel. Sized for the maximum so that changing the runtime
     * stage count never reallocates or re-aliases anything. */
    float32_t state[DSP_BENCH_MAX_CHANNELS]
                   [DSP_IIR_STATE_PER_STAGE * DSP_BENCH_MAX_STAGES];

    /* Shared by every channel. A private copy rather than a pointer to
     * the const table, because fault injection has to be able to perturb
     * it without corrupting the table the test compares against. */
    float32_t coeffs[DSP_IIR_COEFFS_PER_STAGE * DSP_BENCH_MAX_STAGES];
} dsp_iir_t;

/*
 * Copies `stages * 5` coefficients in, zeroes state, and binds one CMSIS
 * instance per channel. Returns false on a geometry the fixed buffers cannot
 * hold, or on a fault that the geometry cannot express (running one stage
 * short needs at least two stages).
 *
 * `coeff_stages` is how many sections the bank at `coeffs` actually holds,
 * and `stages > coeff_stages` is refused. The caller owns the bank and must
 * provide its length so configuration never reads past the coefficient array.
 */
bool dsp_iir_configure(dsp_iir_t *f,
                       uint32_t channels,
                       uint32_t stages,
                       const float32_t *coeffs,
                       uint32_t coeff_stages,
                       dsp_iir_fault_t fault);

/* Zero every channel's state. Must be called before a known-answer run:
 * DF2T carries state across blocks, so a second run over the same input
 * legitimately produces different output. */
void dsp_iir_reset(dsp_iir_t *f);

/* One block. `in` and `out` are channel-major with DSP_IIR_FRAME_STRIDE,
 * and must not overlap. This is the function the benchmark times, and it
 * contains nothing but the per-channel CMSIS calls. */
void dsp_iir_process(dsp_iir_t *f,
                     const float32_t *in,
                     float32_t *out,
                     uint32_t frames);

const char *dsp_iir_fault_name(dsp_iir_fault_t fault);

#endif /* DSP_IIR_H */
