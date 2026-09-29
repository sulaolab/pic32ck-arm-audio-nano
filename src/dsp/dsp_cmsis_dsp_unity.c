/*
 * dsp_cmsis_dsp_unity.c - the one project-owned translation unit that
 *                         compiles the vendored CMSIS-DSP sources.
 *
 * Two things had to be true at once:
 *
 *   1. The vendored files under src/dsp/cmsis_dsp/ must stay BYTE-VERBATIM.
 *      They are the thing being measured. Editing them - even to add a
 *      build gate - would mean the number belongs to a local variant
 *      rather than to CMSIS-DSP 1.17.0, and nobody could compare it to
 *      anything.
 *
 *   2. The live audio configuration must not grow. It is the
 *      firmware that gets flashed first when the board arrives, and this
 *      project has no -ffunction-sections/--gc-sections, so an unreferenced
 *      kernel added to the file list would be linked in regardless.
 *
 * Listing this file in the project and #including the vendor .c files from
 * inside the gate satisfies both. Including a .c is unusual, and it is done
 * here deliberately and exactly twice; the alternative was per-
 * configuration file exclusions in configurations.xml, which hides the same
 * decision somewhere no one reads.
 *
 * Everything compiled here inherits this file's compile options, which is
 * also what makes the -O0 / -O3 comparison honest: the kernel is built with
 * the same flags as the harness that times it.
 *
 * Vendored version and provenance: see src/dsp/cmsis_dsp/VENDOR.md.
 */
#include "app_config.h"

#if APP_ENABLE_DSP_BENCH || APP_ENABLE_AUDIO_DRC

/* The vendor kernel remains byte-verbatim and is the default baseline.  The
 * M33 implementation exports the same CMSIS symbol and is selected only by an
 * explicit build define, so baseline and candidate cannot be confused. */
#if defined(DSP_USE_M33_DF2T_OPT) && (DSP_USE_M33_DF2T_OPT != 0)
#if defined(DSP_M33_NO_FMA_CONTRACT) && (DSP_M33_NO_FMA_CONTRACT != 0)
#pragma GCC push_options
#pragma GCC optimize ("fp-contract=off")
#endif
#include "arm_biquad_cascade_df2T_f32_m33.c"
#if defined(DSP_M33_NO_FMA_CONTRACT) && (DSP_M33_NO_FMA_CONTRACT != 0)
#pragma GCC pop_options
#endif
#elif defined(DSP_M33_NO_FMA_CONTRACT) && (DSP_M33_NO_FMA_CONTRACT != 0)
/* Measurement candidate: retain the byte-verbatim CMSIS C source but ask GCC
 * not to contract multiply-plus-add expressions into VFMA. */
#pragma GCC push_options
#pragma GCC optimize ("fp-contract=off")
#include "cmsis_dsp/Source/FilteringFunctions/arm_biquad_cascade_df2T_f32.c"
#pragma GCC pop_options
#else
#include "cmsis_dsp/Source/FilteringFunctions/arm_biquad_cascade_df2T_f32.c"
#endif
#include "cmsis_dsp/Source/FilteringFunctions/arm_biquad_cascade_df2T_init_f32.c"

#endif /* APP_ENABLE_DSP_BENCH || APP_ENABLE_AUDIO_DRC */
