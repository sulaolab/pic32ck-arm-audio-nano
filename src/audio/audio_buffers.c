/*
 * audio_buffers.c - static ping/pong audio DMA buffers.
 */
#include "audio_buffers.h"

/* The two halves of each buffer are contiguous, so one DMA linked list can
 * walk ping -> pong -> ping with two 1024-byte descriptors. */
int32_t audio_rx_buffer[AUDIO_BLOCK_COUNT][AUDIO_FRAMES_PER_BLOCK][AUDIO_SLOTS_PER_FRAME]
    __attribute__((aligned(AUDIO_BUFFER_ALIGN)));

int32_t audio_tx_buffer[AUDIO_BLOCK_COUNT][AUDIO_FRAMES_PER_BLOCK][AUDIO_SLOTS_PER_FRAME]
    __attribute__((aligned(AUDIO_BUFFER_ALIGN)));

int32_t *audio_buffers_rx_block(uint32_t half)
{
    return &audio_rx_buffer[half & 1u][0][0];
}

int32_t *audio_buffers_tx_block(uint32_t half)
{
    return &audio_tx_buffer[half & 1u][0][0];
}

bool audio_buffers_tx_is_test_pattern(void)
{
#if APP_AUDIO_TX_TEST_PATTERN
    return true;
#else
    return false;
#endif
}

uint32_t audio_buffers_tx_test_word(void)
{
    return (uint32_t)APP_AUDIO_TX_TEST_WORD;
}

void audio_buffers_reset(void)
{
#if APP_AUDIO_TX_TEST_PATTERN
    /* Debug build: a known word in every slot of every frame, so a logic
     * analyser sees the same 4 bytes repeat and any byte permutation or
     * bit shift stands out immediately. */
    const int32_t tx_fill = (int32_t)(uint32_t)APP_AUDIO_TX_TEST_WORD;
#else
    /* Normal build: send silence to the codec DAC until an audio path supplies TX data. */
    const int32_t tx_fill = 0;
#endif

    for (uint32_t h = 0u; h < AUDIO_BLOCK_COUNT; h++)
    {
        int32_t *const rx = audio_buffers_rx_block(h);
        int32_t *const tx = audio_buffers_tx_block(h);

        for (uint32_t w = 0u; w < AUDIO_WORDS_PER_BLOCK; w++)
        {
            rx[w] = 0;
            tx[w] = tx_fill;
        }
    }
}
