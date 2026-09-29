/*
 * audio_buffers.h - the raw DMA ping/pong buffers.
 *
 * Geometry (fixed by app_config.h):
 *   int32_t rx[2][32][8]   32 frames x 8 slots x 32 bit
 *   int32_t tx[2][32][8]
 *
 * Per half-buffer: 32 * 8 * 4 = 1024 bytes.
 * Total raw audio DMA memory: 4 x 1024 = 4096 bytes.
 *
 * Statically allocated: no malloc anywhere in this project.
 */
#ifndef AUDIO_BUFFERS_H
#define AUDIO_BUFFERS_H

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"

#define AUDIO_BUFFER_ALIGN  32      /* >= 32-byte alignment, as specified */

extern int32_t audio_rx_buffer[AUDIO_BLOCK_COUNT][AUDIO_FRAMES_PER_BLOCK][AUDIO_SLOTS_PER_FRAME];
extern int32_t audio_tx_buffer[AUDIO_BLOCK_COUNT][AUDIO_FRAMES_PER_BLOCK][AUDIO_SLOTS_PER_FRAME];

/* Flat pointer to one half-buffer, for the transport and for future DSP. */
int32_t *audio_buffers_rx_block(uint32_t half);
int32_t *audio_buffers_tx_block(uint32_t half);

/* Reset both buffer pairs to their start-of-run state:
 *   RX -> all zero
 *   TX -> all zero, or APP_AUDIO_TX_TEST_WORD in every slot when
 *         APP_AUDIO_TX_TEST_PATTERN is 1.
 * Called from audio_transport_init(). */
void audio_buffers_reset(void);

/* True when this build fills TX with the debug pattern instead of silence.
 * Exposed so the console banner can say so out loud - a build that is
 * deliberately transmitting a test word must never look like a silent one. */
bool audio_buffers_tx_is_test_pattern(void);
uint32_t audio_buffers_tx_test_word(void);

#endif /* AUDIO_BUFFERS_H */
