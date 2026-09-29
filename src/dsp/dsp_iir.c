/*
 * dsp_iir.c - see dsp_iir.h.
 *
 * Host-portable: no device header, no register, no board dependency.
 * The project host-parity test builds this exact file with a host compiler.
 */
#include "app_config.h"

#if APP_ENABLE_DSP_BENCH

#include <string.h>

#include "dsp_iir.h"

/* Size of the perturbation DSP_IIR_FAULT_COEFF applies. Large enough that
 * no plausible tolerance absorbs it, small enough that the filter stays
 * stable and the run does not simply produce infinities - a fault that
 * blows the filter up would be caught by any test at all, and would prove
 * nothing about the tolerance. */
#define DSP_IIR_FAULT_COEFF_DELTA   1.0e-3f

/* Which coefficient to perturb: index 3 of stage 0 is -a1, a feedback
 * term, so the error propagates through the whole cascade instead of
 * scaling one tap. */
#define DSP_IIR_FAULT_COEFF_INDEX   3u

bool dsp_iir_configure(dsp_iir_t *f,
                       uint32_t channels,
                       uint32_t stages,
                       const float32_t *coeffs,
                       uint32_t coeff_stages,
                       dsp_iir_fault_t fault)
{
    if ((f == NULL) || (coeffs == NULL))
    {
        return false;
    }
    if ((channels == 0u) || (channels > DSP_BENCH_MAX_CHANNELS))
    {
        return false;
    }
    if ((stages == 0u) || (stages > DSP_BENCH_MAX_STAGES))
    {
        return false;
    }
    if (stages > coeff_stages)
    {
        /* The bank is shorter than the cascade asked for. Refused, because
         * the alternative is a read past the end of a const array that
         * produces plausible-looking numbers. */
        return false;
    }
    if ((fault == DSP_IIR_FAULT_STAGE_COUNT) && (stages < 2u))
    {
        /* Nothing to take away. Refused rather than silently ignored: a
         * fault injection that quietly does nothing turns the negative
         * test into a test that always passes. */
        return false;
    }

    f->channels = channels;
    f->stages   = stages;
    f->fault    = fault;

    memcpy(f->coeffs, coeffs,
           (size_t)stages * DSP_IIR_COEFFS_PER_STAGE * sizeof(float32_t));

    if (fault == DSP_IIR_FAULT_COEFF)
    {
        f->coeffs[DSP_IIR_FAULT_COEFF_INDEX] += DSP_IIR_FAULT_COEFF_DELTA;
    }

    const uint8_t nstages =
        (uint8_t)((fault == DSP_IIR_FAULT_STAGE_COUNT) ? (stages - 1u) : stages);

    for (uint32_t c = 0u; c < channels; c++)
    {
        float32_t *const st = (fault == DSP_IIR_FAULT_SHARED_STATE)
                                  ? &f->state[0][0]
                                  : &f->state[c][0];

        /* arm_biquad_cascade_df2T_init_f32() also zeroes the state it is
         * given, which is why dsp_iir_reset() and this agree on the
         * start-of-run condition. */
        arm_biquad_cascade_df2T_init_f32(&f->inst[c], nstages, f->coeffs, st);
    }

    return true;
}

void dsp_iir_reset(dsp_iir_t *f)
{
    if (f == NULL)
    {
        return;
    }

    /* Every channel's slot, not just the configured ones: leftover state
     * from a wider geometry must not be visible to a later narrower run. */
    memset(f->state, 0, sizeof(f->state));
}

void dsp_iir_process(dsp_iir_t *f,
                     const float32_t *in,
                     float32_t *out,
                     uint32_t frames)
{
    for (uint32_t c = 0u; c < f->channels; c++)
    {
        const uint32_t off = c * DSP_IIR_FRAME_STRIDE;

        /*
         * The cast drops const because the CMSIS prototype takes a
         * non-const source pointer. The kernel does not write through it:
         * it reads pSrc for stage 0 and then works in pDst for every
         * later stage.
         */
        arm_biquad_cascade_df2T_f32(&f->inst[c],
                                    (float32_t *)(uintptr_t)(in + off),
                                    out + off,
                                    (uint32_t)frames);
    }
}

const char *dsp_iir_fault_name(dsp_iir_fault_t fault)
{
    switch (fault)
    {
    case DSP_IIR_FAULT_NONE:         return "none";
    case DSP_IIR_FAULT_COEFF:        return "coefficient perturbed";
    case DSP_IIR_FAULT_SHARED_STATE: return "state shared across channels";
    case DSP_IIR_FAULT_STAGE_COUNT:  return "one stage short";
    default:                         return "?";
    }
}

#endif /* APP_ENABLE_DSP_BENCH */
