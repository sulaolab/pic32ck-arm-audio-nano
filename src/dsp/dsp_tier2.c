/*
 * dsp_tier2.c - see dsp_tier2.h.
 *
 * Host-portable: no device header, no register, no board dependency.
 * The project host-parity test builds this exact file with a host compiler.
 */
#include "app_config.h"

#if APP_ENABLE_DSP_BENCH

#include <string.h>

#include "dsp_tier2.h"

/*
 * The 24-bit mask.
 *
 * The WM8904 carries 24 bits in a 32-bit slot, so the bottom 8 bits of a
 * received sample are not signal and the bottom 8 bits of a transmitted one
 * are ignored. Masking both directions is what Sonora does, and it is real
 * per-sample work that a comparison should include.
 *
 * Written on the unsigned value and cast back, because a bitwise AND on a
 * negative signed integer is implementation-defined in the letter of the
 * standard even where every real compiler does the obvious thing.
 */
static inline int32_t mask24(int32_t v)
{
    return (int32_t)((uint32_t)v & (uint32_t)DSP_TIER2_SLOT_MASK);
}

static inline int32_t clip_q31(float32_t scaled)
{
    /* The comparison is on the float, before the cast: casting a float
     * outside int32 range is undefined behaviour, so the clip has to happen
     * first rather than being applied to the damage afterwards. */
    if (scaled >= 2147483647.0f)
    {
        return (int32_t)2147483647;
    }
    if (scaled <= -2147483648.0f)
    {
        return (int32_t)(-2147483647 - 1);
    }
    return (int32_t)scaled;      /* truncation toward zero */
}

bool dsp_tier2_configure(dsp_tier2_t *t,
                         uint32_t stages,
                         const float32_t *coeffs,
                         uint32_t coeff_stages,
                         dsp_iir_fault_t iir_fault,
                         dsp_tier2_fault_t fault)
{
    if ((t == NULL) || (fault >= DSP_TIER2_FAULT_COUNT))
    {
        return false;
    }

    if (!dsp_iir_configure(&t->iir, DSP_TIER2_DSP_CHANNELS, stages,
                           coeffs, coeff_stages, iir_fault))
    {
        return false;
    }

    t->pre_gain  = DSP_TIER2_PRE_GAIN;
    t->post_gain = DSP_TIER2_POST_GAIN;

    t->expand_gain[0] = DSP_TIER2_EXPAND_GAIN_L1;
    t->expand_gain[1] = DSP_TIER2_EXPAND_GAIN_R1;
    t->expand_gain[2] = DSP_TIER2_EXPAND_GAIN_L2;
    t->expand_gain[3] = DSP_TIER2_EXPAND_GAIN_R2;

    t->fault = fault;

    memset(t->stage_in, 0, sizeof(t->stage_in));
    memset(t->stage_out, 0, sizeof(t->stage_out));

    return true;
}

void dsp_tier2_reset(dsp_tier2_t *t)
{
    if (t != NULL)
    {
        dsp_iir_reset(&t->iir);
    }
}

void dsp_tier2_process(dsp_tier2_t *t,
                       const int32_t *rx,
                       int32_t *tx,
                       uint32_t frames)
{
    /*
     * Stride of the receive frame. The fault injection reads it at the DSP
     * channel count instead of the slot count, which is exactly the shape of
     * a real TDM stride bug: it still produces a signal, just the wrong one.
     */
    const uint32_t in_stride = (t->fault == DSP_TIER2_FAULT_IN_STRIDE)
                                   ? DSP_TIER2_DSP_CHANNELS
                                   : DSP_TIER2_IN_SLOTS;

    const float32_t pre = t->pre_gain;

    /* ---- int32 -> float, the active input channels, channel-major ---- */
    for (uint32_t n = 0u; n < frames; n++)
    {
        for (uint32_t c = 0u; c < DSP_TIER2_IN_CHANNELS; c++)
        {
            const int32_t raw = mask24(rx[(n * in_stride) + c]);

            t->stage_in[(c * DSP_IIR_FRAME_STRIDE) + n] =
                ((float32_t)raw * DSP_TIER2_Q31_SCALE) * pre;
        }
    }

    /* ---- 2 -> 4 expansion ------------------------------------------- */
    /*
     * L1 R1 L2 R2, the layout Sonora's ch_expand_2to4 produces: the stereo
     * pair duplicated, each of the four with its own gain. The two copies are
     * not the same signal downstream, because each channel carries its own
     * cascade state.
     */
    {
        const float32_t *const in_l = &t->stage_in[0 * DSP_IIR_FRAME_STRIDE];
        const float32_t *const in_r = &t->stage_in[1 * DSP_IIR_FRAME_STRIDE];

        float32_t *const l1 = &t->stage_out[0 * DSP_IIR_FRAME_STRIDE];
        float32_t *const r1 = &t->stage_out[1 * DSP_IIR_FRAME_STRIDE];
        float32_t *const l2 = &t->stage_out[2 * DSP_IIR_FRAME_STRIDE];
        float32_t *const r2 = &t->stage_out[3 * DSP_IIR_FRAME_STRIDE];

        if (t->fault == DSP_TIER2_FAULT_NO_EXPAND)
        {
            /* Degenerate: the gains are not applied at all. */
            for (uint32_t n = 0u; n < frames; n++)
            {
                l1[n] = in_l[n];
                r1[n] = in_r[n];
                l2[n] = in_l[n];
                r2[n] = in_r[n];
            }
        }
        else
        {
            const float32_t g0 = t->expand_gain[0];
            const float32_t g1 = t->expand_gain[1];
            const float32_t g2 = t->expand_gain[2];
            const float32_t g3 = t->expand_gain[3];

            for (uint32_t n = 0u; n < frames; n++)
            {
                const float32_t l = in_l[n];
                const float32_t r = in_r[n];

                l1[n] = l * g0;
                r1[n] = r * g1;
                l2[n] = l * g2;
                r2[n] = r * g3;
            }
        }
    }

    /* ---- the cascade, 4 channels ------------------------------------ */
    /* stage_out holds the expansion result; the cascade reads it and writes
     * stage_in, which is free by now. Two buffers because the CMSIS kernel
     * requires source and destination not to overlap. */
    dsp_iir_process(&t->iir, t->stage_out, t->stage_in, frames);

    /* ---- float -> int32, interleaved into the transmit frame -------- */
    const float32_t post = t->post_gain;

    for (uint32_t n = 0u; n < frames; n++)
    {
        for (uint32_t c = 0u; c < DSP_TIER2_OUT_SLOTS; c++)
        {
            const float32_t v =
                t->stage_in[(c * DSP_IIR_FRAME_STRIDE) + n] * post;

            tx[(n * DSP_TIER2_OUT_SLOTS) + c] =
                mask24(clip_q31(v * 2147483648.0f));
        }
    }
}

const char *dsp_tier2_fault_name(dsp_tier2_fault_t fault)
{
    switch (fault)
    {
    case DSP_TIER2_FAULT_NONE:      return "none";
    case DSP_TIER2_FAULT_IN_STRIDE: return "rx read at the wrong stride";
    case DSP_TIER2_FAULT_NO_EXPAND: return "expansion gains not applied";
    default:                        return "?";
    }
}

#endif /* APP_ENABLE_DSP_BENCH */
