#ifndef APP_BIQUAD_COEFF_CSV_H
#define APP_BIQUAD_COEFF_CSV_H

#include <stdbool.h>
#include <stdint.h>

#define APPDBG_BIQUAD_CSV_STAGE_NUM 30u
#define APPDBG_BIQUAD_CSV_COEFF_NUM 5u
#define APPDBG_BIQUAD_CSV_CH_NUM    4u

typedef enum
{
    APP_BIQUAD_COEFF_CSV_MARKER_NOT_MATCHED = 0,
    APP_BIQUAD_COEFF_CSV_MARKER_CONSUMED,
    APP_BIQUAD_COEFF_CSV_MARKER_STARTED,
} app_biquad_coeff_csv_marker_result_t;

app_biquad_coeff_csv_marker_result_t
app_biquad_coeff_csv_process_marker_line(const char *line);
bool app_biquad_coeff_csv_is_begin_marker(const char *line);
bool app_biquad_coeff_csv_feed_char(uint8_t c);
void app_biquad_coeff_csv_task(void);
bool app_biquad_coeff_csv_is_receiving(void);

/* Sonora CSV state machine owns the apply transaction; keep these hooks in
 * the shared interface so the copied module can remain platform-neutral. */
bool app_biquad_coeff_csv_copy_to_active(
    const float coeff[APPDBG_BIQUAD_CSV_STAGE_NUM]
                     [APPDBG_BIQUAD_CSV_COEFF_NUM]
                     [APPDBG_BIQUAD_CSV_CH_NUM],
    uint16_t stage_num,
    uint16_t coeff_num,
    uint16_t ch_num);
void app_biquad_coeff_csv_clear_iir_state(void);

#endif /* APP_BIQUAD_COEFF_CSV_H */
