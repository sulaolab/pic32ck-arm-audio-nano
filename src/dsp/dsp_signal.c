/*
 * dsp_signal.c - see dsp_signal.h.
 */
#include "app_config.h"

/*
 * This is measurement-only code, so the whole translation unit is behind
 * APP_ENABLE_DSP_BENCH. It compiles to nothing in the live audio
 * configurations.
 */

#if APP_ENABLE_DSP_BENCH

#include <string.h>

#include "dsp_signal.h"

float32_t dsp_signal_bits_to_f32(uint32_t bits)
{
    float32_t v;

    memcpy(&v, &bits, sizeof(v));
    return v;
}

uint32_t dsp_signal_f32_to_bits(float32_t v)
{
    uint32_t bits;

    memcpy(&bits, &v, sizeof(bits));
    return bits;
}

void dsp_signal_fill_lcg(float32_t *dst, uint32_t n, uint32_t channel)
{
    uint32_t s = DSP_SIGNAL_SEED0 + (channel * DSP_SIGNAL_SEED_STRIDE);

    for (uint32_t i = 0u; i < n; i++)
    {
        s = (DSP_SIGNAL_LCG_MUL * s) + DSP_SIGNAL_LCG_ADD;

        /*
         * The int32 -> float32 conversion rounds; both multiplications are
         * by exact powers of two and so are exact. That is what makes this
         * reproducible in Python: one rounding, in a defined place.
         */
        dst[i] = (float32_t)(int32_t)s * (1.0f / 2147483648.0f) * DSP_SIGNAL_GAIN;
    }
}

#endif /* APP_ENABLE_DSP_BENCH */
