/*
 * sonora_drc_path.c - minimal Arm port of Sonora's Classic/DRC block.
 *
 * Authoritative Sonora order:
 *   convert_codec_int_to_float()
 *   app_ch_expand_2to4_process()
 *   app_biquad_cascade_4ch_process()
 *   convert_codec_float_to_int()
 *   copy_to_codec(A, 0), copy_to_codec(B, 2)
 *
 * The only intended algorithmic substitution is inside
 * app_biquad_cascade_4ch_process(): the legacy DF2T call point is replaced by
 * Arm CMSIS-DSP arm_biquad_cascade_df2T_f32().
 */
#include "app_config.h"

#if APP_ENABLE_AUDIO_DRC

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "dsp/filtering_functions.h"
#if !defined(SONORA_DRC_HOST_TEST)
#include "dsp_cycles.h"
#endif
#include "app_specific_config_defs.h"
#include "audio_sample_delay.h"
#include "ch_expand_2to4.h"
#include "sonora_drc_path.h"

#define STAGE_1_PROC_CH                    2u
#define STAGE_2_PROC_CH                    4u
#define BIQUAD_CASCADE_4CH_NUM_STAGE       APP_AUDIO_DRC_STAGES
#define BIQUAD_CASCADE_NUM_CH              STAGE_2_PROC_CH
#define CLASSIC_AUDIO_PATH_TMP_TX_CH       4u
#define DSPLOAD_WINDOW_US                   10000u
#define DSPLOAD_WINDOW_BLOCKS \
    ((AUDIO_FS_HZ * DSPLOAD_WINDOW_US) / \
     (AUDIO_FRAMES_PER_BLOCK * 1000000u))

#define Q31_SCALE_FLOAT                    (1.0f / 2147483648.0f)
#define Q31_SCALE_INT                      2147483648.0f
#define PRE_GAIN_CODEC_DB                  0.0f
#define POST_GAIN_CODEC_DB                 12.0f

typedef struct
{
    float b0;
    float b1;
    float b2;
    float a1;
    float a2;
} biquad_t;

typedef enum
{
    APP_BIQUAD_CASCADE_4CH_PROC_NORMAL_ACTIVE = 0,
    APP_BIQUAD_CASCADE_4CH_PROC_BYPASS_REQUESTED,
    APP_BIQUAD_CASCADE_4CH_PROC_BYPASS_ACTIVE,
} app_biquad_cascade_4ch_proc_state_t;

static float f_a_data[STAGE_1_PROC_CH][AUDIO_FRAMES_PER_BLOCK];
static float f_b_data[STAGE_2_PROC_CH][AUDIO_FRAMES_PER_BLOCK];
static int32_t tmp_tx[CLASSIC_AUDIO_PATH_TMP_TX_CH * AUDIO_FRAMES_PER_BLOCK];

static float Pre_Gain_CODEC;
static float Post_Gain_CODEC;

static arm_biquad_cascade_df2T_instance_f32
    g_biquad_cascade_4ch_cmsis_inst[BIQUAD_CASCADE_NUM_CH];
static float32_t
    g_biquad_cascade_4ch_cmsis_state[BIQUAD_CASCADE_NUM_CH]
                                      [2u * BIQUAD_CASCADE_4CH_NUM_STAGE];
static float32_t
    g_biquad_cascade_4ch_cmsis_coeff[BIQUAD_CASCADE_NUM_CH]
                                      [5u * BIQUAD_CASCADE_4CH_NUM_STAGE];

/* Sonora currently defines ENA_BIQUAD_LOAD_TEST_COEFF in
 * biquad_cascade_4ch_init().  Its live test cascade alternates these two
 * sections, identically for each channel.  Local a1/a2 use the subtraction
 * convention; cmsis_prepare_coeff() negates them for CMSIS's +a1/+a2 API. */
static const biquad_t biquad_1khz_3dB_1ch = {
    1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f
};
static const biquad_t biquad_1khz_m3dB_1ch = {
    0.97112681f, -1.78656936f, 0.83114562f, -1.78656936f, 0.80227183f
};

static volatile bool s_timing_available;
static volatile app_biquad_cascade_4ch_proc_state_t s_biquad_proc_state =
    APP_BIQUAD_CASCADE_4CH_PROC_NORMAL_ACTIVE;
static volatile sonora_drc_path_stats_t s_stats;
static volatile uint32_t s_callback_peak_cycles;
static volatile uint32_t s_callback_deadline_misses;
static volatile uint32_t s_load_current_cycles;
static volatile uint32_t s_load_current_blocks;
static volatile uint64_t s_load_report_sum_cycles;
static volatile uint32_t s_load_report_max_cycles;
static volatile uint32_t s_load_report_windows;

static float db_to_lin(float db)
{
    return powf(10.0f, db / 20.0f);
}

static void convert_codec_int_to_float(const int32_t *restrict int_in,
                                       int channels_in,
                                       float *restrict float_out,
                                       int channels_out,
                                       int frame_size)
{
    const int read_ch = (channels_in < channels_out) ? channels_in : channels_out;
    const float combined_scale = Q31_SCALE_FLOAT * Pre_Gain_CODEC;

    for (int ch = 0; ch < read_ch; ch++)
    {
        const int32_t *restrict in_ch = &int_in[ch];
        float *restrict out_ch = &float_out[ch * frame_size];

        for (int n = 0; n < frame_size; n++)
        {
            const int32_t raw_val = (int32_t)((uint32_t)(*in_ch) & 0xFFFFFF00u);
            float x = (float)raw_val * combined_scale;

            if (x < -1.0f)
            {
                x = -1.0f;
            }
            else if (x > 0.99999994f)
            {
                x = 0.99999994f;
            }

            *out_ch++ = x;
            in_ch += channels_in;
        }
    }

    for (int ch = read_ch; ch < channels_out; ch++)
    {
        float *restrict out_ch = &float_out[ch * frame_size];
        for (int n = 0; n < frame_size; n++)
        {
            out_ch[n] = 0.0f;
        }
    }
}

static void app_biquad_cascade_4ch_process(const float *in, float *out)
{
    if (s_biquad_proc_state != APP_BIQUAD_CASCADE_4CH_PROC_NORMAL_ACTIVE)
    {
        if (in != out)
        {
            memcpy(out, in,
                   sizeof(float) * BIQUAD_CASCADE_NUM_CH * AUDIO_FRAMES_PER_BLOCK);
        }
        s_biquad_proc_state = APP_BIQUAD_CASCADE_4CH_PROC_BYPASS_ACTIVE;
        return;
    }

    for (uint32_t ch = 0u; ch < BIQUAD_CASCADE_NUM_CH; ch++)
    {
        const float32_t *const p_src = &in[ch * AUDIO_FRAMES_PER_BLOCK];
        float32_t *const p_dst = &out[ch * AUDIO_FRAMES_PER_BLOCK];
#if !defined(SONORA_DRC_HOST_TEST)
        const uint32_t t0 = s_timing_available ? dsp_cycles_now() : 0u;
#endif

        arm_biquad_cascade_df2T_f32(&g_biquad_cascade_4ch_cmsis_inst[ch],
                                    p_src, p_dst, AUDIO_FRAMES_PER_BLOCK);

#if !defined(SONORA_DRC_HOST_TEST)
        /* Match Sonora's historical CMSIS-IIR telemetry exactly: its timer
         * bracket is inside the channel loop and `dt` is overwritten on every
         * iteration, so the displayed value is the final ONE-channel API call,
         * not the sum of all four calls.  The full-path/DSPload measurement
         * remains the authoritative four-channel deadline figure. */
        if (s_timing_available)
        {
            uint32_t cycles = dsp_cycles_delta(t0, dsp_cycles_now());
            const uint32_t overhead = dsp_cycles_overhead();
            cycles = (cycles > overhead) ? (cycles - overhead) : 0u;

            s_stats.iir_cycles_last = cycles;
            if (cycles < s_stats.iir_cycles_min)
            {
                s_stats.iir_cycles_min = cycles;
            }
            if (cycles > s_stats.iir_cycles_max)
            {
                s_stats.iir_cycles_max = cycles;
            }
        }
#endif
    }
}

static void convert_codec_float_to_int(const float *restrict float_in,
                                       int channels_in,
                                       int32_t *restrict int_out,
                                       int channels_out,
                                       int frame_size)
{
    const int read_ch = (channels_in < channels_out) ? channels_in : channels_out;
    const float scale = Post_Gain_CODEC * Q31_SCALE_INT;
    const float q31_max_f = 2147483520.0f;
    const float q31_min_f = -2147483648.0f;

    for (int n = 0; n < frame_size; n++)
    {
        int32_t *restrict out_frame = &int_out[n * channels_out];
        const float *restrict in_ch = float_in;

        for (int ch = 0; ch < read_ch; ch++)
        {
            float q31_f = in_ch[n] * scale;

            if ((q31_f > q31_max_f) || (q31_f < q31_min_f))
            {
                q31_f = (q31_f > 0.0f) ? q31_max_f : q31_min_f;
            }

            out_frame[ch] = (int32_t)((uint32_t)(int32_t)q31_f & 0xFFFFFF00u);
            in_ch += frame_size;
        }

        for (int ch = read_ch; ch < channels_out; ch++)
        {
            out_frame[ch] = 0;
        }
    }
}

static inline void copy_to_codec(const int32_t *src_ptr,
                                 uint8_t src_ch,
                                 int32_t *dest_ptr,
                                 uint8_t offset)
{
    if (dest_ptr == NULL)
    {
        return;
    }

    const int32_t *src_offs = src_ptr + offset;
    int32_t *dest = dest_ptr;
    uint16_t smpl = AUDIO_FRAMES_PER_BLOCK;

    while (smpl-- > 0u)
    {
        dest[0] = src_offs[0];
        dest[1] = src_offs[1];
        dest += AUDIO_SLOTS_PER_FRAME;
        src_offs += src_ch;
    }
}

#if !defined(SONORA_DRC_HOST_TEST)
static void record_cycles(uint32_t cycles)
{
    s_stats.cycles_last = cycles;
    if (cycles < s_stats.cycles_min)
    {
        s_stats.cycles_min = cycles;
    }
    if (cycles > s_stats.cycles_max)
    {
        s_stats.cycles_max = cycles;
    }
    if (cycles >= APP_AUDIO_BLOCK_CYCLES)
    {
        if (s_stats.deadline_misses != UINT32_MAX)
        {
            s_stats.deadline_misses++;
        }
    }
}
#endif

bool sonora_drc_path_init(void)
{
    Pre_Gain_CODEC = db_to_lin(PRE_GAIN_CODEC_DB);
    Post_Gain_CODEC = db_to_lin(POST_GAIN_CODEC_DB);

    app_ch_expand_2to4_init();
#if defined(ENA_SAMPLE_DELAY)
    app_audio_sample_delay_init(AUDIO_FS_HZ);
#endif

    for (uint32_t ch = 0u; ch < BIQUAD_CASCADE_NUM_CH; ch++)
    {
        for (uint32_t stage = 0u; stage < BIQUAD_CASCADE_4CH_NUM_STAGE; stage++)
        {
            const biquad_t *const bq = ((stage & 1u) != 0u)
                                              ? &biquad_1khz_m3dB_1ch
                                              : &biquad_1khz_3dB_1ch;
            float32_t *const pc = &g_biquad_cascade_4ch_cmsis_coeff[ch][stage * 5u];

            pc[0] = bq->b0;
            pc[1] = bq->b1;
            pc[2] = bq->b2;
            pc[3] = -bq->a1;
            pc[4] = -bq->a2;
        }

        arm_biquad_cascade_df2T_init_f32(&g_biquad_cascade_4ch_cmsis_inst[ch],
                                         (uint8_t)BIQUAD_CASCADE_4CH_NUM_STAGE,
                                         g_biquad_cascade_4ch_cmsis_coeff[ch],
                                         g_biquad_cascade_4ch_cmsis_state[ch]);
    }

    s_biquad_proc_state = APP_BIQUAD_CASCADE_4CH_PROC_NORMAL_ACTIVE;
    s_timing_available = false;
    sonora_drc_path_reset();
    return true;
}

void sonora_drc_path_reset(void)
{
    memset(g_biquad_cascade_4ch_cmsis_state, 0,
           sizeof(g_biquad_cascade_4ch_cmsis_state));
    memset(f_a_data, 0, sizeof(f_a_data));
    memset(f_b_data, 0, sizeof(f_b_data));
    memset(tmp_tx, 0, sizeof(tmp_tx));

    s_stats.timing_available = s_timing_available;
    s_stats.processed_blocks = 0u;
    s_stats.cycles_last = 0u;
    s_stats.cycles_min = UINT32_MAX;
    s_stats.cycles_max = 0u;
    s_stats.iir_cycles_last = 0u;
    s_stats.iir_cycles_min = UINT32_MAX;
    s_stats.iir_cycles_max = 0u;
    s_stats.deadline_misses = 0u;
    s_callback_peak_cycles = 0u;
    s_callback_deadline_misses = 0u;
    s_load_current_cycles = 0u;
    s_load_current_blocks = 0u;
    s_load_report_sum_cycles = 0u;
    s_load_report_max_cycles = 0u;
    s_load_report_windows = 0u;
}

void app_biquad_cascade_4ch_clear_state(void)
{
    memset(g_biquad_cascade_4ch_cmsis_state, 0,
           sizeof(g_biquad_cascade_4ch_cmsis_state));
}

bool app_biquad_cascade_4ch_load_coeff_from_uart_csv(const float *coeff,
                                                      uint16_t stage_num,
                                                      uint16_t coeff_num,
                                                      uint16_t ch_num)
{
    if (coeff == NULL)
    {
        return false;
    }
    if (coeff_num != 5u)
    {
        return false;
    }
    if (ch_num != BIQUAD_CASCADE_NUM_CH)
    {
        return false;
    }
    if ((stage_num == 0u) || (stage_num > BIQUAD_CASCADE_4CH_NUM_STAGE))
    {
        return false;
    }
    if (!app_biquad_cascade_4ch_is_bypass_active())
    {
        return false;
    }

    /* Sonora validates every value before touching the active table. */
    for (uint16_t stage = 0u; stage < stage_num; stage++)
    {
        for (uint16_t coeff_idx = 0u; coeff_idx < coeff_num; coeff_idx++)
        {
            for (uint16_t ch = 0u; ch < ch_num; ch++)
            {
                const uint32_t index =
                    ((uint32_t)stage * coeff_num * ch_num) +
                    ((uint32_t)coeff_idx * ch_num) + ch;
                if (!isfinite(coeff[index]))
                {
                    return false;
                }
            }
        }
    }

    /* As in Sonora, stages not present in a shorter CSV become bypass. */
    for (uint16_t ch = 0u; ch < BIQUAD_CASCADE_NUM_CH; ch++)
    {
        for (uint16_t stage = 0u; stage < BIQUAD_CASCADE_4CH_NUM_STAGE; stage++)
        {
            float32_t *const pc =
                &g_biquad_cascade_4ch_cmsis_coeff[ch][stage * 5u];
            pc[0] = 1.0f;
            pc[1] = 0.0f;
            pc[2] = 0.0f;
            pc[3] = 0.0f;
            pc[4] = 0.0f;
        }
    }

    for (uint16_t stage = 0u; stage < stage_num; stage++)
    {
        for (uint16_t ch = 0u; ch < ch_num; ch++)
        {
            const uint32_t base =
                ((uint32_t)stage * coeff_num * ch_num) + ch;
            float32_t *const pc =
                &g_biquad_cascade_4ch_cmsis_coeff[ch][stage * 5u];

            pc[0] = coeff[base + (0u * ch_num)];
            pc[1] = coeff[base + (1u * ch_num)];
            pc[2] = coeff[base + (2u * ch_num)];
            /* Sonora coefficient tables use the subtraction convention;
             * CMSIS DF2T expects the feedback signs inverted. */
            pc[3] = -coeff[base + (3u * ch_num)];
            pc[4] = -coeff[base + (4u * ch_num)];
        }
    }

    for (uint16_t ch = 0u; ch < BIQUAD_CASCADE_NUM_CH; ch++)
    {
        arm_biquad_cascade_df2T_init_f32(&g_biquad_cascade_4ch_cmsis_inst[ch],
                                         (uint8_t)BIQUAD_CASCADE_4CH_NUM_STAGE,
                                         g_biquad_cascade_4ch_cmsis_coeff[ch],
                                         g_biquad_cascade_4ch_cmsis_state[ch]);
    }

    return true;
}

void app_biquad_cascade_4ch_request_bypass(void)
{
    if (s_biquad_proc_state == APP_BIQUAD_CASCADE_4CH_PROC_NORMAL_ACTIVE)
    {
        s_biquad_proc_state = APP_BIQUAD_CASCADE_4CH_PROC_BYPASS_REQUESTED;
    }
}

bool app_biquad_cascade_4ch_is_bypass_active(void)
{
    return s_biquad_proc_state == APP_BIQUAD_CASCADE_4CH_PROC_BYPASS_ACTIVE;
}

/* Imported Sonora CSV policy: once the transport has authoritatively stopped,
 * no DMA callback can still read the coefficient table.  Promote a pending
 * bypass explicitly so an offline CSV apply cannot wait forever for an ISR
 * acknowledgement that will never arrive. */
void app_biquad_cascade_4ch_confirm_bypass_stopped(void)
{
    if (s_biquad_proc_state == APP_BIQUAD_CASCADE_4CH_PROC_BYPASS_REQUESTED)
    {
        s_biquad_proc_state = APP_BIQUAD_CASCADE_4CH_PROC_BYPASS_ACTIVE;
    }
}

void app_biquad_cascade_4ch_request_normal(void)
{
    s_biquad_proc_state = APP_BIQUAD_CASCADE_4CH_PROC_NORMAL_ACTIVE;
}

bool sonora_drc_path_timing_init(void)
{
#if defined(SONORA_DRC_HOST_TEST)
    s_timing_available = false;
#else
    s_timing_available = dsp_cycles_init() &&
                         (dsp_cycles_source() == DSP_CYCLES_SRC_DWT);
#endif
    s_stats.timing_available = s_timing_available;
    return s_timing_available;
}

void sonora_drc_path_process(const int32_t *src_ptr,
                             int32_t *dest_ptr_a,
                             int32_t *dest_ptr_b)
{
#if !defined(SONORA_DRC_HOST_TEST)
    const uint32_t t0 = s_timing_available ? dsp_cycles_now() : 0u;
#endif

    convert_codec_int_to_float(src_ptr, AUDIO_SLOTS_PER_FRAME,
                               &f_a_data[0][0], STAGE_1_PROC_CH,
                               AUDIO_FRAMES_PER_BLOCK);

    app_ch_expand_2to4_process(&f_a_data[0][0], &f_b_data[0][0]);
    app_biquad_cascade_4ch_process(&f_b_data[0][0], &f_b_data[0][0]);
#if defined(ENA_SAMPLE_DELAY)
    app_audio_sample_delay_process(&f_b_data[0][0]);
#endif

    convert_codec_float_to_int(&f_b_data[0][0], STAGE_2_PROC_CH,
                               tmp_tx, CLASSIC_AUDIO_PATH_TMP_TX_CH,
                               AUDIO_FRAMES_PER_BLOCK);

    copy_to_codec(tmp_tx, CLASSIC_AUDIO_PATH_TMP_TX_CH, dest_ptr_a, 0u);
    copy_to_codec(tmp_tx, CLASSIC_AUDIO_PATH_TMP_TX_CH, dest_ptr_b, 2u);

    if (s_stats.processed_blocks != UINT32_MAX)
    {
        s_stats.processed_blocks++;
    }

    if (s_timing_available)
    {
#if !defined(SONORA_DRC_HOST_TEST)
        const uint32_t t1 = dsp_cycles_now();
        uint32_t cycles = dsp_cycles_delta(t0, t1);
        const uint32_t overhead = dsp_cycles_overhead();
        cycles = (cycles > overhead) ? (cycles - overhead) : 0u;
        record_cycles(cycles);
#endif
    }
}

void sonora_drc_path_get_stats(sonora_drc_path_stats_t *out)
{
    if (out != NULL)
    {
        out->timing_available = s_stats.timing_available;
        out->processed_blocks = s_stats.processed_blocks;
        out->cycles_last = s_stats.cycles_last;
        out->cycles_min = (s_stats.cycles_min == UINT32_MAX) ? 0u : s_stats.cycles_min;
        out->cycles_max = s_stats.cycles_max;
        out->iir_cycles_last = s_stats.iir_cycles_last;
        out->iir_cycles_min = (s_stats.iir_cycles_min == UINT32_MAX)
                                  ? 0u : s_stats.iir_cycles_min;
        out->iir_cycles_max = s_stats.iir_cycles_max;
        out->deadline_misses = s_stats.deadline_misses;
    }
}

uint32_t sonora_drc_path_callback_begin(void)
{
#if defined(SONORA_DRC_HOST_TEST)
    return 0u;
#else
    return s_timing_available ? dsp_cycles_now() : 0u;
#endif
}

void sonora_drc_path_callback_end(uint32_t started)
{
#if defined(SONORA_DRC_HOST_TEST)
    (void)started;
#else
    if (!s_timing_available)
    {
        return;
    }

    uint32_t cycles = dsp_cycles_delta(started, dsp_cycles_now());
    const uint32_t overhead = dsp_cycles_overhead();
    cycles = (cycles > overhead) ? (cycles - overhead) : 0u;

    if (cycles > s_callback_peak_cycles)
    {
        s_callback_peak_cycles = cycles;
    }
    if ((cycles >= APP_AUDIO_BLOCK_CYCLES) &&
        (s_callback_deadline_misses != UINT32_MAX))
    {
        s_callback_deadline_misses++;
    }

    s_load_current_cycles += cycles;
    s_load_current_blocks++;
    if (s_load_current_blocks >= DSPLOAD_WINDOW_BLOCKS)
    {
        s_load_report_sum_cycles += s_load_current_cycles;
        if (s_load_current_cycles > s_load_report_max_cycles)
        {
            s_load_report_max_cycles = s_load_current_cycles;
        }
        if (s_load_report_windows != UINT32_MAX)
        {
            s_load_report_windows++;
        }
        s_load_current_cycles = 0u;
        s_load_current_blocks = 0u;
    }
#endif
}

void sonora_drc_path_take_telemetry(sonora_drc_telemetry_t *out)
{
    if (out == NULL)
    {
        return;
    }

#if defined(SONORA_DRC_HOST_TEST)
    memset(out, 0, sizeof(*out));
#else
    const uint32_t primask = __get_PRIMASK();
    __disable_irq();

    out->timing_available = s_timing_available;
    out->callback_peak_cycles = s_callback_peak_cycles;
    out->callback_deadline_misses = s_callback_deadline_misses;
    out->load_window_cycles = APP_CPU_CLOCK_HZ / (1000000u / DSPLOAD_WINDOW_US);
    out->load_windows = s_load_report_windows;
    out->load_mean_cycles = (s_load_report_windows != 0u)
                                ? (uint32_t)(s_load_report_sum_cycles /
                                             s_load_report_windows)
                                : 0u;
    out->load_max_cycles = s_load_report_max_cycles;

    s_callback_peak_cycles = 0u;
    s_load_report_sum_cycles = 0u;
    s_load_report_max_cycles = 0u;
    s_load_report_windows = 0u;

    if (primask == 0u)
    {
        __enable_irq();
    }
#endif
}

_Static_assert(AUDIO_FRAMES_PER_BLOCK == 32u,
               "Classic/DRC reference block is 32 frames");
_Static_assert(AUDIO_SLOTS_PER_FRAME == 8u,
               "Classic/DRC reference transport is TDM8");
_Static_assert((APP_AUDIO_DRC_STAGES == 22u) ||
                   (APP_ALLOW_NONREFERENCE_DRC_STAGES != 0),
                "reference profile is 22 stages; explicitly allow stage overrides");
_Static_assert((AUDIO_FS_HZ * DSPLOAD_WINDOW_US) %
                   (AUDIO_FRAMES_PER_BLOCK * 1000000u) == 0u,
               "DSPload window must contain a whole number of audio blocks");
_Static_assert(DSPLOAD_WINDOW_BLOCKS > 0u,
               "DSPload window must contain at least one audio block");

#endif /* APP_ENABLE_AUDIO_DRC */
