/*
 * dsp_coeffs_check.c - verify the compiler parsed the generated
 *                      coefficient literals into the intended binary32
 *                      values.
 *
 * Separate from dsp_coeffs.c because that file is generated: a check that
 * lives in the generated output is regenerated along with the thing it
 * checks, which is the wrong direction of dependency.
 *
 * The comparison is on bit patterns, not on values, so a NaN or a signed
 * zero cannot pass by accident.
 */
#include "app_config.h"

/*
 * This is measurement-only code, so the whole translation unit is behind
 * APP_ENABLE_DSP_BENCH. It compiles to nothing in the live audio
 * configurations.
 */

#if APP_ENABLE_DSP_BENCH

#include <string.h>

#include "dsp_coeffs.h"

static bool check_bank(const float32_t *values,
                       const uint32_t *bits_expected,
                       uint32_t count,
                       uint32_t *first_bad)
{
    for (uint32_t i = 0u; i < count; i++)
    {
        uint32_t bits = 0u;

        /* memcpy, not a pointer cast: the cast would be a strict-aliasing
         * violation and -O3 is entitled to act on that. */
        memcpy(&bits, &values[i], sizeof(bits));

        if (bits != bits_expected[i])
        {
            if (first_bad != NULL)
            {
                *first_bad = i;
            }
            return false;
        }
    }

    return true;
}

bool dsp_coeffs_self_check(uint32_t *first_bad)
{
    return check_bank(dsp_coeffs_bw12_lp, dsp_coeffs_bw12_lp_bits,
                      DSP_COEFFS_BW12_LP_STAGES * 5u, first_bad);
}

bool dsp_coeffs_self_check_all(uint32_t *first_bad, const char **bad_bank)
{
    if (!check_bank(dsp_coeffs_bw12_lp, dsp_coeffs_bw12_lp_bits,
                    DSP_COEFFS_BW12_LP_STAGES * 5u, first_bad))
    {
        if (bad_bank != NULL) { *bad_bank = "bw12_lp"; }
        return false;
    }

    if (!check_bank(dsp_coeffs_ap84, dsp_coeffs_ap84_bits,
                    DSP_COEFFS_AP_STAGES * 5u, first_bad))
    {
        if (bad_bank != NULL) { *bad_bank = "ap84"; }
        return false;
    }

    return true;
}

#endif /* APP_ENABLE_DSP_BENCH */
