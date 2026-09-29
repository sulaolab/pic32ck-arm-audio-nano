/*
 * dsp_tier2.h - the DRC-equivalent DSP block (protocol Tier 2).
 *
 * Tier 1 times the DF2T kernel and nothing else. Tier 2 adds the format
 * conversions and channel expansion paid by the signal chain, but not the
 * transport, DMA, or ISR cost of the live image.
 *
 * THE CHAIN, exactly, per block:
 *
 *   DSP_TIER2_IN_SLOTS-slot interleaved int32 receive frame
 *          |  DSP_TIER2_IN_CHANNELS active slots
 *          |  24-bit mask, Q31 scale, pre-gain
 *          v
 *   channel-major float32, DSP_TIER2_IN_CHANNELS channels
 *          |  2 -> 4 expansion, four independent gains
 *          v
 *   channel-major float32, DSP_TIER2_DSP_CHANNELS channels
 *          |  N-section DF2T cascade, one call per channel,
 *          |  per-channel state, shared coefficients
 *          v
 *   channel-major float32
 *          |  post-gain, Q31 scale, truncation, 24-bit mask, clip
 *          v
 *   DSP_TIER2_OUT_SLOTS-slot interleaved int32 transmit frame
 *
 * and NOT: FIR, sample delay, level meter, or keepalive. Those application
 * features are outside this optional measurement scope.
 *
 * WHY 4 CHANNELS AND NOT 8: TDM8 is a wire format, not a DSP channel count.
 * Sonora's Classic/DRC path takes two active slots, expands them to four DSP
 * channels and runs the cascade on those four. Reproducing that is the whole
 * point - an 8-channel workload would be a different amount of work.
 *
 * Host-portable on purpose: no device header, no register, no board
 * dependency, so the project host-parity test builds this exact source with a host
 * compiler and the known-answer test does not depend on hardware existing.
 */
#ifndef DSP_TIER2_H
#define DSP_TIER2_H

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "dsp_iir.h"

/*
 * Tier-2-specific deliberate defects.
 *
 * The kernel faults in dsp_iir_fault_t cover the cascade. These cover what is
 * new in Tier 2 and what a plausible implementation slip would actually be:
 * reading the receive frame with the wrong stride, and letting the expansion
 * degenerate so the two extra channels are not independent work. Neither is
 * hypothetical - a wrong stride is the classic TDM bug, and an expansion that
 * quietly copies would make a 4-channel measurement into a 2-channel one.
 */
typedef enum
{
    DSP_TIER2_FAULT_NONE = 0,
    DSP_TIER2_FAULT_IN_STRIDE,      /* read the rx frame at the DSP channel
                                     * count instead of the slot count */
    DSP_TIER2_FAULT_NO_EXPAND,      /* skip the expansion gains entirely   */
    DSP_TIER2_FAULT_COUNT,
} dsp_tier2_fault_t;

typedef struct
{
    dsp_iir_t iir;

    /*
     * Gains held as mutable float32 fields, initialised from the macros in
     * dsp_tier2_configure(). Deliberately not const literals: a compiler is
     * entitled to fold a multiply by a known 1.0f away, and then the block
     * being measured would be missing work that the real one performs.
     * Sonora holds these as runtime globals for the same practical reason.
     */
    float32_t pre_gain;
    float32_t post_gain;
    float32_t expand_gain[DSP_TIER2_DSP_CHANNELS];

    dsp_tier2_fault_t fault;

    /* Working buffers, channel-major with DSP_IIR_FRAME_STRIDE so the IIR
     * wrapper can walk them directly. Two of them because
     * arm_biquad_cascade_df2T_f32() requires source and destination not to
     * overlap. */
    float32_t stage_in[DSP_TIER2_DSP_CHANNELS * DSP_IIR_FRAME_STRIDE];
    float32_t stage_out[DSP_TIER2_DSP_CHANNELS * DSP_IIR_FRAME_STRIDE];
} dsp_tier2_t;

/* Binds the cascade and installs the gains. `coeff_stages` is the length of
 * the bank at `coeffs`, and `stages > coeff_stages` is refused - see the note
 * on dsp_iir_configure(). */
bool dsp_tier2_configure(dsp_tier2_t *t,
                         uint32_t stages,
                         const float32_t *coeffs,
                         uint32_t coeff_stages,
                         dsp_iir_fault_t iir_fault,
                         dsp_tier2_fault_t fault);

/* Zero the cascade state. The DF2T recursion carries state across blocks, so
 * a known-answer run has to start from a known state. */
void dsp_tier2_reset(dsp_tier2_t *t);

/*
 * One block, end to end.
 *
 * `rx` is DSP_TIER2_IN_SLOTS * frames interleaved int32; `tx` is
 * DSP_TIER2_OUT_SLOTS * frames interleaved int32. This is the function the
 * Tier 2 measurement times, and it contains the whole chain and nothing else.
 */
void dsp_tier2_process(dsp_tier2_t *t,
                       const int32_t *rx,
                       int32_t *tx,
                       uint32_t frames);

const char *dsp_tier2_fault_name(dsp_tier2_fault_t fault);

#endif /* DSP_TIER2_H */
