#ifndef SONORA_PIC32CK_APP_SPECIFIC_CONFIG_DEFS_H
#define SONORA_PIC32CK_APP_SPECIFIC_CONFIG_DEFS_H

/*
 * PIC32CK platform seam for Sonora Classic sources.
 *
 * Keep the imported source files on their original feature names.  The
 * mapping lives here so a source comparison shows one local include seam,
 * rather than platform edits spread through each DSP implementation.
 */
#include "app_config.h"

#define SONORA_PLATFORM_PIC32CK          1
#define ENA_DRC_DF2T_CASCADE             (APP_ENABLE_AUDIO_DRC)
#define APP_BLOCK_FRAMES                 (AUDIO_FRAMES_PER_BLOCK)
#define SAMPLE_RATE                      (AUDIO_FS_HZ)
#define APP_ASRC_MEAS                     0
#define APP_ASRC_MEAS_UART2_STREAM        0

#if APP_ENABLE_AUDIO_DRC
#define ENA_BIQUAD_IIR_CASCADE
#endif

#if APP_ENABLE_SONORA_SAMPLE_DELAY
#define ENA_SAMPLE_DELAY
#endif

#endif /* SONORA_PIC32CK_APP_SPECIFIC_CONFIG_DEFS_H */
