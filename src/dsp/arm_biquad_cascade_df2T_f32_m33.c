/*
 * Cortex-M33 scalar candidate for arm_biquad_cascade_df2T_f32().
 *
 * The public CMSIS-DSP API and state/coefficient layout are unchanged.  Three
 * adjacent stages are fused internally so two intermediate blocks remain in
 * FP registers instead of being stored to and reloaded from pDst.  Dedicated
 * two-stage and one-stage tails preserve arbitrary nonzero stage counts;
 * blockSize is unrestricted and exact in-place operation remains supported.
 *
 * This file is a project-owned implementation, not a modification of vendored
 * CMSIS-DSP.  dsp_cmsis_dsp_unity.c selects it only when
 * DSP_USE_M33_DF2T_OPT is explicitly defined.
 */
#include "arm_compiler_specific.h"
#include "dsp/filtering_functions.h"

#ifndef DSP_M33_DF2T_UNROLL
#define DSP_M33_DF2T_UNROLL 16
#endif

#define DSP_M33_PRAGMA_IMPL(x) _Pragma(#x)
#define DSP_M33_PRAGMA(x) DSP_M33_PRAGMA_IMPL(x)

ARM_DSP_ATTRIBUTE void arm_biquad_cascade_df2T_f32(
    const arm_biquad_cascade_df2T_instance_f32 *S,
    const float32_t *pSrc,
    float32_t *pDst,
    uint32_t blockSize)
{
    const float32_t *pIn = pSrc;
    float32_t *pOut = pDst;
    float32_t *pState = S->pState;
    const float32_t *pCoeffs = S->pCoeffs;
    uint32_t stages = S->numStages;

    while (stages >= 3u)
    {
        const float32_t b00 = pCoeffs[0];
        const float32_t b10 = pCoeffs[1];
        const float32_t b20 = pCoeffs[2];
        const float32_t a10 = pCoeffs[3];
        const float32_t a20 = pCoeffs[4];
        const float32_t b01 = pCoeffs[5];
        const float32_t b11 = pCoeffs[6];
        const float32_t b21 = pCoeffs[7];
        const float32_t a11 = pCoeffs[8];
        const float32_t a21 = pCoeffs[9];
        const float32_t b02 = pCoeffs[10];
        const float32_t b12 = pCoeffs[11];
        const float32_t b22 = pCoeffs[12];
        const float32_t a12 = pCoeffs[13];
        const float32_t a22 = pCoeffs[14];

        float32_t d10 = pState[0];
        float32_t d20 = pState[1];
        float32_t d11 = pState[2];
        float32_t d21 = pState[3];
        float32_t d12 = pState[4];
        float32_t d22 = pState[5];

        DSP_M33_PRAGMA(GCC unroll DSP_M33_DF2T_UNROLL)
        for (uint32_t n = 0u; n < blockSize; n++)
        {
            const float32_t x = *pIn++;
            const float32_t y0 = (b00 * x) + d10;
            d10 = (b10 * x) + d20;
            d10 += a10 * y0;
            d20 = b20 * x;
            d20 += a20 * y0;

            const float32_t y1 = (b01 * y0) + d11;
            d11 = (b11 * y0) + d21;
            d11 += a11 * y1;
            d21 = b21 * y0;
            d21 += a21 * y1;

            const float32_t y2 = (b02 * y1) + d12;
            d12 = (b12 * y1) + d22;
            d12 += a12 * y2;
            d22 = b22 * y1;
            d22 += a22 * y2;
            *pOut++ = y2;
        }

        pState[0] = d10;
        pState[1] = d20;
        pState[2] = d11;
        pState[3] = d21;
        pState[4] = d12;
        pState[5] = d22;
        pState += 6u;
        pCoeffs += 15u;
        stages -= 3u;
        pIn = pDst;
        pOut = pDst;
    }

    if (stages == 2u)
    {
        const float32_t b00 = pCoeffs[0];
        const float32_t b10 = pCoeffs[1];
        const float32_t b20 = pCoeffs[2];
        const float32_t a10 = pCoeffs[3];
        const float32_t a20 = pCoeffs[4];
        const float32_t b01 = pCoeffs[5];
        const float32_t b11 = pCoeffs[6];
        const float32_t b21 = pCoeffs[7];
        const float32_t a11 = pCoeffs[8];
        const float32_t a21 = pCoeffs[9];
        float32_t d10 = pState[0];
        float32_t d20 = pState[1];
        float32_t d11 = pState[2];
        float32_t d21 = pState[3];

        DSP_M33_PRAGMA(GCC unroll DSP_M33_DF2T_UNROLL)
        for (uint32_t n = 0u; n < blockSize; n++)
        {
            const float32_t x = *pIn++;
            const float32_t y0 = (b00 * x) + d10;
            d10 = (b10 * x) + d20;
            d10 += a10 * y0;
            d20 = b20 * x;
            d20 += a20 * y0;

            const float32_t y1 = (b01 * y0) + d11;
            d11 = (b11 * y0) + d21;
            d11 += a11 * y1;
            d21 = b21 * y0;
            d21 += a21 * y1;
            *pOut++ = y1;
        }

        pState[0] = d10;
        pState[1] = d20;
        pState[2] = d11;
        pState[3] = d21;
    }
    else if (stages != 0u)
    {
        const float32_t b0 = pCoeffs[0];
        const float32_t b1 = pCoeffs[1];
        const float32_t b2 = pCoeffs[2];
        const float32_t a1 = pCoeffs[3];
        const float32_t a2 = pCoeffs[4];
        float32_t d1 = pState[0];
        float32_t d2 = pState[1];

        DSP_M33_PRAGMA(GCC unroll DSP_M33_DF2T_UNROLL)
        for (uint32_t n = 0u; n < blockSize; n++)
        {
            const float32_t x = *pIn++;
            const float32_t y = (b0 * x) + d1;
            d1 = (b1 * x) + d2;
            d1 += a1 * y;
            d2 = b2 * x;
            d2 += a2 * y;
            *pOut++ = y;
        }

        pState[0] = d1;
        pState[1] = d2;
    }
}

#undef DSP_M33_PRAGMA
#undef DSP_M33_PRAGMA_IMPL
