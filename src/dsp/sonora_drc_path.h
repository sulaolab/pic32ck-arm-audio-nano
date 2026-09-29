/*
 * sonora_drc_path.h - Sonora Classic/DRC live audio path.
 *
 * This is intentionally a narrow port.  The conversion, 2-to-4 expansion,
 * four independent DF2T instances and codec scatter follow Sonora's call
 * order.  The platform-specific difference begins at the DF2T API: this
 * target calls Arm CMSIS-DSP.
 */
#ifndef SONORA_DRC_PATH_H
#define SONORA_DRC_PATH_H

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"

#if APP_ENABLE_AUDIO_DRC

typedef struct
{
    bool     timing_available;
    uint32_t processed_blocks;
    uint32_t cycles_last;
    uint32_t cycles_min;
    uint32_t cycles_max;
    uint32_t iir_cycles_last;
    uint32_t iir_cycles_min;
    uint32_t iir_cycles_max;
    uint32_t deadline_misses;
} sonora_drc_path_stats_t;

/* Sonora-format transport telemetry sources.  The callback peak is the
 * DMA-audio ISR response time for the current print interval.  DSP load uses
 * completed fixed 10 ms windows, independent of the print period. */
typedef struct
{
    bool     timing_available;
    uint32_t callback_peak_cycles;
    uint32_t callback_deadline_misses;
    uint32_t load_window_cycles;
    uint32_t load_mean_cycles;
    uint32_t load_max_cycles;
    uint32_t load_windows;
} sonora_drc_telemetry_t;

/* Prepare Sonora gains, expansion gains, coefficients, CMSIS instances and
 * state.  Call before audio DMA can enter the processing path. */
bool sonora_drc_path_init(void);

/* Clear filter state and live timing counters at a mute-bounded restart. */
void sonora_drc_path_reset(void);

/* Enable the live cycle measurement after transport start-up has finished
 * using delay/SysTick.  Live timing deliberately requires DWT CYCCNT. */
bool sonora_drc_path_timing_init(void);

/* Sonora Classic/DRC shape.  dest_ptr_b may be NULL on the one-codec PIC32CK
 * bench; copy_to_codec() has the same NULL rule as Sonora. */
void sonora_drc_path_process(const int32_t *src_ptr,
                             int32_t *dest_ptr_a,
                             int32_t *dest_ptr_b);

void sonora_drc_path_get_stats(sonora_drc_path_stats_t *out);

uint32_t sonora_drc_path_callback_begin(void);
void sonora_drc_path_callback_end(uint32_t started);
void sonora_drc_path_take_telemetry(sonora_drc_telemetry_t *out);

/* Sonora-compatible runtime coefficient-load boundary.  The CSV receiver
 * requests bypass first and may update coefficients only after the audio ISR
 * has acknowledged BYPASS_ACTIVE on a complete block boundary. */
void app_biquad_cascade_4ch_request_bypass(void);
bool app_biquad_cascade_4ch_is_bypass_active(void);
void app_biquad_cascade_4ch_confirm_bypass_stopped(void);
void app_biquad_cascade_4ch_request_normal(void);
bool app_biquad_cascade_4ch_load_coeff_from_uart_csv(const float *coeff,
                                                      uint16_t stage_num,
                                                      uint16_t coeff_num,
                                                      uint16_t ch_num);
void app_biquad_cascade_4ch_clear_state(void);

#endif /* APP_ENABLE_AUDIO_DRC */
#endif /* SONORA_DRC_PATH_H */
