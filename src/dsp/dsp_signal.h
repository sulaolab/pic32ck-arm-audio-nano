/*
 * dsp_signal.h - deterministic synthetic stimulus for the isolated benchmark,
 *                and the bit-pattern <-> float32 conversions the test data
 *                needs.
 *
 * There is no audio transport in this benchmark. The bench feeds the
 * kernel from a buffer it fills itself, so that the number it produces is
 * the kernel's and nothing else's.
 *
 * The generator is a plain 32-bit LCG, reproduced exactly by the host
 * reference generator. Two separate consumers:
 *
 *   - the known-answer test uses the EMBEDDED bit patterns in
 *     dsp_vectors.c, never this generator, so a divergence between the C
 *     and Python versions cannot quietly change the expected answer;
 *   - the benchmark uses this generator, because it needs arbitrary
 *     geometry and does not care about the exact sample values.
 *
 * Why not silence, or a constant? Because a DF2T cascade fed zeros has
 * every state variable at zero, and on some cores that is measurably
 * faster than real data. A benchmark on flush-to-zero denormals or on
 * all-zero operands is not a benchmark of anything useful.
 */
#ifndef DSP_SIGNAL_H
#define DSP_SIGNAL_H

#include <stdint.h>

#include "arm_math_types.h"

/* Must match gen_reference.py. */
#define DSP_SIGNAL_LCG_MUL      1664525u
#define DSP_SIGNAL_LCG_ADD      1013904223u
#define DSP_SIGNAL_SEED0        0x12345677u
#define DSP_SIGNAL_SEED_STRIDE  0x9E3779B9u
#define DSP_SIGNAL_GAIN         0.5f

/* Reinterpret a stored bit pattern as float32, and back. Done with memcpy
 * inside, not a pointer cast: the cast is a strict-aliasing violation that
 * -O3 is entitled to act on. */
float32_t dsp_signal_bits_to_f32(uint32_t bits);
uint32_t  dsp_signal_f32_to_bits(float32_t v);

/* Fill `n` samples for channel `channel` with the LCG stimulus, scaled to
 * +/-DSP_SIGNAL_GAIN of full scale. Each channel gets its own seed, so no
 * two channels carry the same signal - a channel-crossing bug in the
 * kernel wrapper shows up as a wrong answer rather than as a coincidence. */
void dsp_signal_fill_lcg(float32_t *dst, uint32_t n, uint32_t channel);

#endif /* DSP_SIGNAL_H */
