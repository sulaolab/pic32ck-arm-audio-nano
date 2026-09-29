/*
 * audio_transport.h - the boundary between "how bits get on and off the
 *                     wire" and "what we do with the samples".
 *
 * Nothing above this header may mention SERCOM, DMA, DOPO, BYTORD or a
 * register name. Everything above it sees only:
 *
 *      transport  ->  32-frame block  ->  (future) DSP
 *
 * A block is AUDIO_WORDS_PER_BLOCK (256) int32_t words laid out
 * frame-major: word[f * 8 + s] is slot s of frame f.
 *
 * The implementation configures real hardware
 * and is sequenced against the codec by the app layer, but nothing here has
 * run on silicon yet. APP_ENABLE_AUDIO_TRANSPORT still defaults to 0.
 *
 * This layer deliberately knows NOTHING about the WM8904. It never starts or
 * stops the codec clock: the app layer owns that ordering, because the
 * required sequence (transport fully armed BEFORE the codec starts clocking)
 * is a statement about two independent devices and belongs where both are
 * visible.
 */
#ifndef AUDIO_TRANSPORT_H
#define AUDIO_TRANSPORT_H

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"

/*
 * Error and progress counters. All zero until the transport actually runs on
 * hardware. Updated from the DMA ISR - counter increments only, never any
 * printing.
 *
 * Every counter SATURATES at UINT32_MAX rather than wrapping. For the error
 * counters that matters: a wrapped error count can read 0, and "0 errors" is
 * exactly the thing this phase's PASS criterion depends on, so it must never
 * be a lie. The block counters saturate for the same reason (a wrap would
 * make a one-second delta read as a huge negative jump); at 1500 blocks/s
 * saturation is ~33 days of continuous running.
 */
typedef struct
{
    uint32_t audio_rx_blocks;       /* RX blocks completed                */
    uint32_t audio_tx_blocks;       /* TX blocks completed                */

    uint32_t sercom_tur_count;      /* SPI client transmit underrun       */
    uint32_t sercom_bufovf_count;   /* SPI client receive buffer overflow */
    uint32_t sercom_lenerr_count;   /* frame length mismatch (LENGTH)     */

    uint32_t dma_rx_rde_count;      /* RX channel read error              */
    uint32_t dma_rx_wre_count;      /* RX channel write error             */
    uint32_t dma_tx_rde_count;      /* TX channel read error              */
    uint32_t dma_tx_wre_count;      /* TX channel write error             */

    /* Transfer abort. The DMAC clears CHCTRLA.ENABLE on TA, so one abort means
     * that channel has STOPPED - its block counter freezes from then on. Nothing
     * in this transport configures an abort source (no EVAUXIE, no PATEN), so
     * both must stay 0; a non-zero value is a finding, not noise. */
    uint32_t dma_rx_ta_count;
    uint32_t dma_tx_ta_count;

    /* Start-up window only (see audio_transport_start). What the SERCOM flagged
     * in the first frames after it was enabled into a possibly already running
     * BCLK/FS. Kept apart from the steady-state counters above, which start
     * from zero once the stream is locked. */
    uint32_t startup_lenerr;        /* 1 = FSYNC arrived inside a frame      */
    uint32_t startup_tur;
    uint32_t startup_bufovf;
    uint32_t startup_rx_stall;      /* no completed RX DMA block in proof window */
    uint32_t startup_tx_stall;      /* no completed TX DMA block in proof window */
    uint32_t sync_attempts;         /* audio_transport_start() calls         */
    uint32_t sync_fail;             /* starts refused as misaligned/stalled  */
} audio_stats_t;

/* Outcome of the last audio_transport_start(). */
typedef enum
{
    AUDIO_SYNC_IDLE = 0,            /* never started, or stopped             */
    AUDIO_SYNC_LOCKED,              /* no LENERR / repeat TUR in the window  */
    AUDIO_SYNC_MISALIGNED,          /* refused: see startup_* counters       */
    AUDIO_SYNC_STALLED,             /* refused: RX or TX made no DMA progress */
} audio_sync_state_t;

/*
 * Which half of each ping/pong pair the hardware and the app are on, plus the
 * running flag. Snapshot taken with the DMA interrupts masked, so the fields
 * are mutually consistent rather than sampled at different instants.
 */
typedef struct
{
    bool     running;
    uint32_t rx_blocks;         /* saturating, from the stats block          */
    uint32_t tx_blocks;
    uint32_t rx_filling_half;   /* half the RX DMA is writing into NOW       */
    uint32_t tx_reading_half;   /* half the TX DMA is reading from NOW       */
    uint32_t rx_last_done_half; /* half the RX DMA most recently completed   */
    uint32_t tx_last_done_half;
    bool     rx_block_pending;  /* a completed RX block not yet released     */
    bool     rx_dma_enabled;    /* CHCTRLA.ENABLE read back from the channel */
    bool     tx_dma_enabled;    /* (the hardware clears it on a TA)          */
    audio_sync_state_t sync;
    int32_t  rx_first_half;     /* half the FIRST RX block landed in; -1 = none yet */
    int32_t  tx_first_half;
} audio_transport_state_t;

/*
 * Firmware loop-through state.  This is deliberately expressed in terms of
 * raw transport blocks rather than codec channels: a loop-through test must
 * prove the exact words received on the serial wire are the words sent back.
 *
 * Only an APP_AUDIO_LOOPTHROUGH build drives this state machine.  Other
 * configurations report OFF and leave their existing TX behaviour unchanged.
 */
typedef enum
{
    AUDIO_LOOPTHROUGH_OFF = 0,
    AUDIO_LOOPTHROUGH_PRIMING,
    AUDIO_LOOPTHROUGH_RUNNING,
    AUDIO_LOOPTHROUGH_DRAINING,
    AUDIO_LOOPTHROUGH_FAULT,
} audio_loopthrough_state_t;

typedef struct
{
    audio_loopthrough_state_t state;
    uint32_t copied_blocks;      /* completed RX -> TX raw block copies     */
    uint32_t silenced_blocks;    /* TX halves zero-filled after stop/fault  */
    uint32_t pair_faults;        /* lost/overrun RX/TX completion pairing   */
} audio_loopthrough_status_t;

/*
 * First-transfer-abort register image.  The DMA ISR captures this before it
 * acknowledges CHINTF, so it remains available after the start-up code stops
 * the transport.  It is a diagnostic record, not an audio data-path API.
 */
typedef struct
{
    bool     valid;
    uint32_t chctrla;
    uint32_t chctrlb;
    uint32_t chevctrl;
    uint32_t chinten;
    uint32_t chintf;
    uint32_t chssa;
    uint32_t chdsa;
    uint32_t chnxt;
    uint32_t chxsiz;
    uint32_t chllcfgstat;
    uint32_t chstatbc;
    uint32_t chstatcc;
    uint32_t chstat;
} audio_dma_ta_snapshot_t;

typedef struct
{
    uint32_t dma_intstat3;
    uint32_t dma_intstat2;
    uint32_t dma_intstat1;
    uint16_t sercom_status;
    uint8_t  sercom_intflag;
    uint16_t sercom_fifospace;
    audio_dma_ta_snapshot_t rx;
    audio_dma_ta_snapshot_t tx;
} audio_dma_diagnostics_t;

/*
 * Configure SERCOM4 (framed SPI client) and the two DMA channels, reset the
 * buffers to their start-of-run contents, and clear the counters. Leaves the
 * SERCOM disabled and both DMA channels disarmed: nothing can move yet.
 */
bool audio_transport_init(void);

/*
 * Arm both DMA channels, enable the SERCOM LAST, then check alignment.
 *
 * The codec's BCLK/FS may already be running - that is the intended case. Two
 * frames after the enable the SERCOM status is read: an FSYNC inside a frame
 * (LENERR) means the stream may be shifted, so the start is refused and the
 * transport stopped (returns false, sync = MISALIGNED). A one-off TUR/BUFOVF
 * is recorded and cleared; a TUR that survives one clear is refused the same
 * way. Otherwise the start-up flags are moved to the startup_* counters and
 * the steady-state counters begin at zero.  A final 2 ms proof window then
 * requires both DMA directions to complete at least one block; an enabled TX
 * channel with no TX BC is a stalled serial stream, not a successful lock.
 *
 * audio_transport_init() must have run since the last stop.
 */
bool audio_transport_start(void);

/*
 * Disable the SERCOM, then the DMA channels, drop any pending block. The codec
 * clock may keep running: nothing here depends on it being stopped.
 */
void audio_transport_stop(void);

bool audio_transport_is_running(void);

/* Consistent snapshot of the ownership state, for the console diagnostic. */
void audio_transport_get_state(audio_transport_state_t *out);

/*
 * Start/stop the real-time firmware loop-through.  On a loop-through build,
 * the DMA BC ISR copies each completed RX half into a TX half that has just
 * finished transmitting.  It never uses the foreground block API: a block is
 * only 666.7 us, whereas the console/LED foreground cadence is 10 ms.
 *
 * start() returns false unless the transport is already LOCKED and TX is not
 * a wire-test-pattern build.  It enters PRIMING; after two safe pairs the ISR
 * enters RUNNING.  The application owns the WM8904 analogue mute and must not
 * unmute HPOUT before it observes RUNNING.  stop() starts a two-half silence
 * drain; stopping the transport itself remains the application's decision.
 */
bool audio_transport_loopthrough_start(void);
void audio_transport_loopthrough_stop(void);
void audio_transport_loopthrough_get_status(audio_loopthrough_status_t *out);

/* First `count` words of the most recently completed RX block, copied out for
 * a foreground dump. Returns the number of words actually copied (0 when no
 * block has completed). Never called from an ISR. */
uint32_t audio_transport_peek_rx(int32_t *dst, uint32_t count);

/* True when a fresh RX block is available for processing. */
bool audio_transport_rx_block_ready(void);

/* The RX block just filled, and the TX block that must be filled before
 * the next block boundary. Both are AUDIO_WORDS_PER_BLOCK words. */
int32_t *audio_transport_get_rx_block(void);
int32_t *audio_transport_get_tx_block(void);

/* Hand the pair of blocks back to the transport. */
void audio_transport_release_block(void);

/* Snapshot of the counters. */
const audio_stats_t *audio_transport_stats(void);

/* Snapshot the first-TA diagnostic image, if either DMA channel aborted. */
void audio_transport_get_dma_diagnostics(audio_dma_diagnostics_t *out);

/* Poll the SERCOM error flags from the foreground. Cheap; call it from the
 * idle loop when the transport is running. */
void audio_transport_poll_errors(void);

#endif /* AUDIO_TRANSPORT_H */
