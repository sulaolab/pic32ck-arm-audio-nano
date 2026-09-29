/*
 * audio_transport_pic32ck.c - SERCOM4 framed-SPI client + DMA0 ping-pong.
 *
 * Every register field used here is a symbol from PIC32CK-SG DFP 1.6.199
 * (component/sercom.h, component/dma.h). No invented bit names, no bare
 * hex written into a control register.
 *
 * ---------------------------------------------------------------------
 * What this file claims, and what it does not
 * ---------------------------------------------------------------------
 * Claims: it compiles against the DFP, the field names exist, the frame
 * arithmetic is consistent, and the descriptor layout comes from the DFP's
 * own dma_descriptor_registers_t.
 *
 * Does NOT claim: that it has ever clocked a bit. APP_ENABLE_AUDIO_TRANSPORT
 * is 0 by default and main() never starts it.
 *
 * Every register-level question this file used to carry has now been
 * settled against PIC32CK Family Datasheet DS60001795H (byte ordering,
 * FIFO thresholds, FSES, DMA priority interrupts). What is left is
 * genuinely physical - bit alignment against a live BCLK/FSYNC.
 *
 * ---------------------------------------------------------------------
 * Frame arithmetic
 * ---------------------------------------------------------------------
 *   character size      = 8 bit  (CTRLB.CHSIZE only offers 8 or 9 bit)
 *   slot                = 32 bit = 4 characters
 *   frame               = 8 slots = 32 characters = 32 bytes = 256 bit
 *   LENGTH.LEN          = 32 characters, LENEN = 1
 *   DMA cell            = 4 bytes = one 32-bit DATA access (DATA32B)
 *   DMA block           = 1024 bytes = 32 frames  -> one descriptor
 *   block time          = 32 / 48 kHz = 666.666... us
 */
#include "audio_transport.h"

#include <stddef.h>

#include "audio_buffers.h"
#include "board.h"
#if APP_ENABLE_AUDIO_DRC
#include "dsp/sonora_drc_path.h"
#endif

#define AUDIO_SERCOM_REGS   SERCOM4_REGS
#define AUDIO_DMA_REGS      DMA0_REGS

#define AUDIO_SPIS          (&AUDIO_SERCOM_REGS->SPIS)
#define AUDIO_DMA_CH(n)     (&AUDIO_DMA_REGS->CHANNEL[(n)])

/* ------------------------------------------------------------------ */
/* State                                                              */
/* ------------------------------------------------------------------ */

/* Linked-list descriptors. The DFP defines a .hsram memory section macro
 * (SECTION_DMA_DESCRIPTOR) but the XC32 linker script for this device has
 * no .hsram output section, so ordinary RAM with explicit alignment is
 * used instead. dma_descriptor_registers_t is itself declared
 * __attribute__((aligned(4))). */
static volatile dma_descriptor_registers_t dma_desc_rx[AUDIO_BLOCK_COUNT]
    __attribute__((aligned(16)));
static volatile dma_descriptor_registers_t dma_desc_tx[AUDIO_BLOCK_COUNT]
    __attribute__((aligned(16)));

static audio_stats_t   audio_stats;
static volatile audio_dma_diagnostics_t dma_ta_diag;

/*
 * Buffer ownership.
 *
 * Each sequence counter is incremented by the ISR once per completed block, so
 * seq & 1 is the half the DMA has MOVED ON TO and (seq - 1) & 1 is the half it
 * just finished. Only the ISR writes them; the foreground only reads.
 *
 * rx_block_consumed is the mirror image: only the foreground writes it, in
 * audio_transport_release_block(). "A block is pending" is therefore the
 * single comparison rx_block_seq != rx_block_consumed, with no shared flag to
 * race over.
 */
static volatile uint32_t rx_block_seq;      /* ISR writes, foreground reads */
static volatile uint32_t tx_block_seq;      /* ISR writes, foreground reads */
static uint32_t          rx_block_consumed; /* foreground writes only       */
static volatile bool     transport_running;

/* Sequence value before the first completed block. Normally 0; 1 when the DMA
 * turned out to start on half 1 (see audio_transport_start). */
static uint32_t           rx_block_seq_start;
static audio_sync_state_t sync_state;
static int32_t            rx_first_half = -1;
static int32_t            tx_first_half = -1;

#if APP_AUDIO_LOOPTHROUGH
/*
 * The foreground may only request or observe this state.  The data path lives
 * entirely in dma0_service(): a 32-frame block lasts 666.7 us, so the 10 ms
 * console/LED cadence is deliberately never part of the loop-through timing.
 */
#define AUDIO_LOOPTHROUGH_PRIME_BLOCKS  2u
#define AUDIO_LOOPTHROUGH_DRAIN_BLOCKS  2u

typedef struct
{
    volatile audio_loopthrough_state_t state;
    volatile uint32_t copied_blocks;
    volatile uint32_t silenced_blocks;
    volatile uint32_t pair_faults;
} audio_loopthrough_runtime_t;

static audio_loopthrough_runtime_t audio_loopthrough;
static volatile bool     loop_rx_pending;
static volatile bool     loop_tx_pending;
static volatile uint32_t loop_rx_pending_half;
static volatile uint32_t loop_tx_pending_half;
static volatile uint32_t loop_drain_blocks;

/* Definitions live with the other loop-through helpers below, but DMA ISR and
 * foreground error harvest occur earlier in this translation unit. */
static void loop_latch_fault(void);
static bool loop_service_completions(bool rx_complete, uint32_t rx_half,
                                     bool tx_complete, uint32_t tx_half,
                                     bool dma_fault);
#endif

/* TUR is sticky until cleared, and clearing it while running flushes the TX
 * FIFO (DS60001795 "Framed SPI Errors"), which would shift every later TX slot.
 * So it is counted once per run and only cleared by the SWRST in init. */
static bool               tur_seen;

/* Start-up window: >= 2 frames (2 x 20.8 us at 48 kHz) so at least one full
 * FSYNC-to-FSYNC period has passed since the SERCOM was enabled. */
#define AUDIO_SYNC_WINDOW_US    100u

/* A 32-frame DMA block is 666.7 us.  Three periods prove both linked-list
 * channels are actually completing blocks after the alignment/TUR checks;
 * CHCTRLA.ENABLE by itself is not evidence that TX has begun moving. */
#define AUDIO_START_PROGRESS_WINDOW_US  2000u

/*
 * Saturating increment. Used for every counter, including the block counters.
 *
 * A wrapped error counter can read 0, and "all error counters are 0" is the
 * The status criterion must not be able to lie. Saturation turns an
 * overflow into a stuck maximum, which is obviously wrong instead of subtly
 * wrong.
 */
static inline void stat_inc(uint32_t *c)
{
    if (*c != 0xFFFFFFFFu)
    {
        (*c)++;
    }
}

/* ------------------------------------------------------------------ */
/* Field groups shared by the RX and TX channels                      */
/* ------------------------------------------------------------------ */

#if AUDIO_DMA_BYTE_SWIZZLE
/* BYTORD = 01 (WORD_SWIZZLE): 3->0, 2->1, 1->2, 0->3 within each 32-bit
 * word. See the derivation at AUDIO_DMA_BYTE_SWIZZLE in app_config.h. */
#define AUDIO_DMA_BYTEORDER_BITS                                              \
    (DMA_CHCTRLB_WBOEN_Msk | DMA_CHCTRLB_BYTORD_BYTORD_WORD_SWIZZLE)

/*
 * DMAC applies the active write byte order while it loads linked-list
 * descriptor words too, including BDNXT.  The CPU-written initial CHNXT stays
 * in native address order; every word inside a descriptor must be
 * pre-reversed so the load leaves its channel register in native order.
 * Without this, a TX descriptor value of 0x002a6225 becomes 0x25622a00: its
 * RAS/WAS fields are wrong and its PATEN bit is inadvertently set, immediately
 * aborting a silence-filled first transfer.  The same rule keeps the second
 * descriptor address from becoming an invalid byte-reversed pointer.
 */
static inline uint32_t dma_descriptor_sfr_word(uint32_t value)
{
    return ((value & 0x000000FFu) << 24) |
           ((value & 0x0000FF00u) << 8)  |
           ((value & 0x00FF0000u) >> 8)  |
           ((value & 0xFF000000u) >> 24);
}
#else
#define AUDIO_DMA_BYTEORDER_BITS  (DMA_CHCTRLB_BYTORD_BYTORD_NONE)

static inline uint32_t dma_descriptor_sfr_word(uint32_t value)
{
    return value;
}
#endif

/* Which channel registers each descriptor reloads, plus "keep going".
 * CTRLB / EVCTRL / SSA / DSA / XSIZ are the five that differ per block;
 * ENABLE and LLEN keep the channel armed and the list walking.  Loading
 * EVCTRL is deliberate even though its value is zero: TA is generated by an
 * enabled auxiliary event, so a descriptor must not inherit an event setting
 * from any prior channel user. */
#define AUDIO_DMA_BDCFG_BITS                                                  \
    (DMA_BDCFG_CTRLB_Msk | DMA_BDCFG_EVCTRL_Msk | DMA_BDCFG_SSA_Msk |         \
     DMA_BDCFG_DSA_Msk | DMA_BDCFG_XSIZ_Msk | DMA_BDCFG_ENABLE_Msk |          \
     DMA_BDCFG_LLEN_Msk)

/* XSIZ: cell = one 32-bit DATA access, block = one 1024-byte half buffer. */
#define AUDIO_DMA_XSIZ_BITS                                                   \
    (DMA_CHXSIZ_CSZ(AUDIO_DMA_CELL_BYTES) | DMA_CHXSIZ_BLKSZ(AUDIO_DMA_BLOCK_BYTES))
#define AUDIO_DMA_BDXSIZ_BITS                                                 \
    (DMA_BDXSIZ_CSZ(AUDIO_DMA_CELL_BYTES) | DMA_BDXSIZ_BLKSZ(AUDIO_DMA_BLOCK_BYTES))

/*
 * Address sequencing. The DFP offers BYTE_ADDR_INCR (+1),
 * HALF_WORD_ADDR_INCR (+2), AUTO_ADDR_INCR ("Auto Increment Address and
 * Transfer Size"), and the three FIXED_* variants. There is no explicit
 * "+4 word increment", so AUTO_ADDR_INCR is the only option that can walk
 * a 4-byte cell through RAM.
 *
 * The SERCOM DATA register end is a single word address that must not
 * advance: FIXED_WORD_ADDR_INCR ("Fixed Address Word Burst Transfer").
 */
#define AUDIO_DMA_RX_CTRLB                                                    \
    (DMA_CHCTRLB_RAS_FIXED_WORD_ADDR_INCR |   /* read  SERCOM4 DATA      */   \
     DMA_CHCTRLB_WAS_AUTO_ADDR_INCR |         /* write RAM, advancing    */   \
     DMA_CHCTRLB_PRI_PRI_3 |                  /* audio is the deadline   */   \
     AUDIO_DMA_BYTEORDER_BITS |                                               \
     DMA_CHCTRLB_TRIG(AUDIO_DMA_TRIG_RX))

#define AUDIO_DMA_TX_CTRLB                                                    \
    (DMA_CHCTRLB_RAS_AUTO_ADDR_INCR |         /* read  RAM, advancing    */   \
     DMA_CHCTRLB_WAS_FIXED_WORD_ADDR_INCR |   /* write SERCOM4 DATA      */   \
     DMA_CHCTRLB_PRI_PRI_3 |                                                  \
     AUDIO_DMA_BYTEORDER_BITS |                                               \
     DMA_CHCTRLB_TRIG(AUDIO_DMA_TRIG_TX))

/* Same field positions exist in the descriptor image (BDCTRLB), so the
 * channel value can be reused verbatim: DMA_BDCTRLB_* and DMA_CHCTRLB_*
 * share WAS/RAS/PRI/WBOEN/BYTORD/TRIG at identical offsets. */
/*
 * Interrupt enables. CHINTENSET only covers bits 0..5
 * (DMA_CHINTENSET_Msk = 0x0000003F): SD, TA, CC, BC, BH, LL. The read and
 * write error flags WRE (bit 17) and RDE (bit 18) exist in CHINTF
 * (DMA_CHINTF_Msk = 0x0006003F) but have NO enable bit, so they are
 * status-only. They are therefore harvested two ways: opportunistically
 * inside the ISR (which BC and TA do bring us into) and by
 * audio_transport_poll_errors() from the foreground, so a bus error that
 * never coincides with a block boundary is still counted.
 *
 * BC = one 1024-byte block (32 frames) finished - the ping-pong tick.
 * TA = transfer aborted, how a channel-level failure surfaces.
 */
#define AUDIO_DMA_INTEN_BITS                                                  \
    (DMA_CHINTENSET_BC_Msk | DMA_CHINTENSET_TA_Msk)

/* ------------------------------------------------------------------ */
/* SERCOM4: framed SPI client                                         */
/* ------------------------------------------------------------------ */

static void sercom4_init(void)
{
    sercom_spis_registers_t *const s = AUDIO_SPIS;

    board_periph_clock_enable(BOARD_MCLK_ID_SERCOM4_APB);

    /* The SPI client takes SCK and SS from outside, so no baud generator
     * is involved. The core generic clock is enabled anyway: it costs
     * nothing and removes any doubt about internal logic that needs it. */
    board_gclk_channel_enable(BOARD_GCLK_ID_SERCOM4_CORE, APP_GCLK_GEN_SERCOM_CORE);
    board_gclk_channel_enable(BOARD_GCLK_ID_SERCOM_SLOW, APP_GCLK_GEN_SERCOM_SLOW);

    /* PB09 = DO is an output; PB10 = SCK, PC00 = SS and PC01 = DI are all
     * inputs and need PINCFG.INEN. */
    board_pin_mux(BOARD_AUDIO_DO_PORT,  BOARD_AUDIO_DO_PIN,  BOARD_AUDIO_DO_MUX,  false);
    board_pin_mux(BOARD_AUDIO_SCK_PORT, BOARD_AUDIO_SCK_PIN, BOARD_AUDIO_SCK_MUX, true);
    board_pin_mux(BOARD_AUDIO_FS_PORT,  BOARD_AUDIO_FS_PIN,  BOARD_AUDIO_FS_MUX,  true);
    board_pin_mux(BOARD_AUDIO_DI_PORT,  BOARD_AUDIO_DI_PIN,  BOARD_AUDIO_DI_MUX,  true);

    s->SERCOM_CTRLA = SERCOM_SPIS_CTRLA_SWRST_Msk;
    while ((s->SERCOM_SYNCBUSY & SERCOM_SPIS_SYNCBUSY_SWRST_Msk) != 0u)
    {
        /* wait for software reset */
    }

    s->SERCOM_CTRLA =
        SERCOM_SPIS_CTRLA_MODE_SPI_SLAVE |            /* SPI client        */
        SERCOM_SPIS_CTRLA_FORM_SPI |                  /* plain SPI frame   */
        SERCOM_SPIS_CTRLA_DOPO(BOARD_AUDIO_DOPO) |    /* DO=PAD0 SCK=PAD1 SS=PAD2 */
        SERCOM_SPIS_CTRLA_DIPO(BOARD_AUDIO_DIPO) |    /* DI=PAD3           */
        SERCOM_SPIS_CTRLA_CPOL(AUDIO_SPI_CPOL) |      /* 0: SCK idles low  */
        SERCOM_SPIS_CTRLA_CPHA(AUDIO_SPI_CPHA) |      /* 0: sample leading */
        SERCOM_SPIS_CTRLA_DORD_MSB |                  /* MSB first         */
        SERCOM_SPIS_CTRLA_IBON_Msk;                   /* immediate BUFOVF  */

    s->SERCOM_CTRLB =
        SERCOM_SPIS_CTRLB_CHSIZE_8_BIT |              /* 8-bit characters  */
        SERCOM_SPIS_CTRLB_RXEN_Msk |                  /* full duplex       */
        SERCOM_SPIS_CTRLB_PLOADEN_Msk;                /* preload first TX  */
    while ((s->SERCOM_SYNCBUSY & SERCOM_SPIS_SYNCBUSY_CTRLB_Msk) != 0u)
    {
        /* wait for CTRLB */
    }

    s->SERCOM_CTRLC =
        SERCOM_SPIS_CTRLC_FRMEN_Msk |                 /* framed SPI on     */
        SERCOM_SPIS_CTRLC_FMODE_SLAVE |               /* frame client      */
        SERCOM_SPIS_CTRLC_FSPOL(AUDIO_SPI_FS_POLARITY) |
        SERCOM_SPIS_CTRLC_FSLEN(AUDIO_SPI_FSLEN) |    /* 0 = one SCK pulse */
        SERCOM_SPIS_CTRLC_FSES(AUDIO_SPI_FSES) |
        SERCOM_SPIS_CTRLC_DATA32B_Msk |               /* 32-bit DATA access */
        SERCOM_SPIS_CTRLC_FIFOEN_Msk |                /* RX/TX FIFO on     */
        SERCOM_SPIS_CTRLC_RXTRHOLD_DEFAULT |
        SERCOM_SPIS_CTRLC_TXTRHOLD_DEFAULT;
    /*
     * DEFAULT is the correct choice, per DS60001795H:
     *   TXTRHOLD = DEFAULT -> TX DMA trigger while the FIFO is not full
     *   RXTRHOLD = DEFAULT -> RX DMA trigger while data is in the FIFO
     * and, with DATA32B plus the length counter in use, "DMA trigger is
     * generated each time there is 32-bit internal place to store new
     * data". So the trigger already lands on 4-byte boundaries, which is
     * exactly the 4-byte DMA cell size used here. No threshold tuning
     * needed. (SERCOM4 FIFO_SIZE is 8 bytes per the ATDF.)
     */

    /* Hardware frame length policing: 32 characters per frame. A frame
     * that is longer or shorter raises STATUS.LENERR, which is one of the
     * counters this project tracks. */
    s->SERCOM_LENGTH = (uint16_t)(SERCOM_SPIS_LENGTH_LEN(AUDIO_SPI_LENGTH_CHARS) |
                                  SERCOM_SPIS_LENGTH_LENEN_Msk);
    while ((s->SERCOM_SYNCBUSY & SERCOM_SPIS_SYNCBUSY_LENGTH_Msk) != 0u)
    {
        /* wait for LENGTH */
    }

    /* Clear sticky errors so the counters start from a clean slate. */
    s->SERCOM_STATUS = (uint16_t)(SERCOM_SPIS_STATUS_BUFOVF_Msk |
                                  SERCOM_SPIS_STATUS_TUR_Msk |
                                  SERCOM_SPIS_STATUS_LENERR_Msk);
    s->SERCOM_INTFLAG = (uint8_t)SERCOM_SPIS_INTFLAG_ERROR_Msk;
}

static void sercom4_enable(bool on)
{
    sercom_spis_registers_t *const s = AUDIO_SPIS;

    if (on)
    {
        s->SERCOM_CTRLA |= SERCOM_SPIS_CTRLA_ENABLE_Msk;
    }
    else
    {
        s->SERCOM_CTRLA &= ~SERCOM_SPIS_CTRLA_ENABLE_Msk;
    }

    while ((s->SERCOM_SYNCBUSY & SERCOM_SPIS_SYNCBUSY_ENABLE_Msk) != 0u)
    {
        /* wait for the enable bit to synchronize */
    }
}

/* ------------------------------------------------------------------ */
/* DMA                                                                */
/* ------------------------------------------------------------------ */

static uint32_t sercom4_data_address(void)
{
    return (uint32_t)(&AUDIO_SPIS->SERCOM_DATA);
}

/*
 * One descriptor describes exactly one 1024-byte half buffer = 32 frames,
 * and points at the descriptor for the other half. Two descriptors chained
 * to each other give an endless ping -> pong -> ping walk with no CPU work
 * per block.
 *
 * The channel registers themselves are preloaded with the FIRST block, and
 * CHNXT points at the descriptor for the SECOND block. So:
 *
 *   channel regs (ping) -> desc[1] (pong) -> desc[0] (ping) -> desc[1] ...
 */
static void dma_build_descriptors(void)
{
    const uint32_t data_addr = sercom4_data_address();

    for (uint32_t h = 0u; h < AUDIO_BLOCK_COUNT; h++)
    {
        const uint32_t other = (h + 1u) % AUDIO_BLOCK_COUNT;

        /* RX: SERCOM4 DATA -> audio_rx_buffer[h] */
        dma_desc_rx[h].DMA_BDNXT    = dma_descriptor_sfr_word((uint32_t)&dma_desc_rx[other]);
        dma_desc_rx[h].DMA_BDCFG    = dma_descriptor_sfr_word(AUDIO_DMA_BDCFG_BITS);
        dma_desc_rx[h].DMA_BDCTRLB  = dma_descriptor_sfr_word(AUDIO_DMA_RX_CTRLB);
        dma_desc_rx[h].DMA_BDSSA    = dma_descriptor_sfr_word(data_addr);
        dma_desc_rx[h].DMA_BDDSA    = dma_descriptor_sfr_word((uint32_t)audio_buffers_rx_block(h));
        dma_desc_rx[h].DMA_BDSSTRD  = dma_descriptor_sfr_word(0u);
        dma_desc_rx[h].DMA_BDDSTRD  = dma_descriptor_sfr_word(0u);
        dma_desc_rx[h].DMA_BDXSIZ   = dma_descriptor_sfr_word(AUDIO_DMA_BDXSIZ_BITS);
        dma_desc_rx[h].DMA_BDEVCTRL = dma_descriptor_sfr_word(0u);
        dma_desc_rx[h].DMA_BDCTRLCRC = dma_descriptor_sfr_word(0u);
        dma_desc_rx[h].DMA_BDPDAT   = dma_descriptor_sfr_word(0u);
        dma_desc_rx[h].DMA_BDCRCDAT = dma_descriptor_sfr_word(0u);

        /* TX: audio_tx_buffer[h] -> SERCOM4 DATA */
        dma_desc_tx[h].DMA_BDNXT    = dma_descriptor_sfr_word((uint32_t)&dma_desc_tx[other]);
        dma_desc_tx[h].DMA_BDCFG    = dma_descriptor_sfr_word(AUDIO_DMA_BDCFG_BITS);
        dma_desc_tx[h].DMA_BDCTRLB  = dma_descriptor_sfr_word(AUDIO_DMA_TX_CTRLB);
        dma_desc_tx[h].DMA_BDSSA    = dma_descriptor_sfr_word((uint32_t)audio_buffers_tx_block(h));
        dma_desc_tx[h].DMA_BDDSA    = dma_descriptor_sfr_word(data_addr);
        dma_desc_tx[h].DMA_BDSSTRD  = dma_descriptor_sfr_word(0u);
        dma_desc_tx[h].DMA_BDDSTRD  = dma_descriptor_sfr_word(0u);
        dma_desc_tx[h].DMA_BDXSIZ   = dma_descriptor_sfr_word(AUDIO_DMA_BDXSIZ_BITS);
        dma_desc_tx[h].DMA_BDEVCTRL = dma_descriptor_sfr_word(0u);
        dma_desc_tx[h].DMA_BDCTRLCRC = dma_descriptor_sfr_word(0u);
        dma_desc_tx[h].DMA_BDPDAT   = dma_descriptor_sfr_word(0u);
        dma_desc_tx[h].DMA_BDCRCDAT = dma_descriptor_sfr_word(0u);
    }
}

static void dma_init(void)
{
    dma_channel_registers_t *const rx = AUDIO_DMA_CH(AUDIO_DMA_CH_RX);
    dma_channel_registers_t *const tx = AUDIO_DMA_CH(AUDIO_DMA_CH_TX);

    board_periph_clock_enable(BOARD_MCLK_ID_DMA0_AHB);
    board_periph_clock_enable(BOARD_MCLK_ID_DMA0_APB);

    /* Channels off while they are reprogrammed. */
    rx->DMA_CHCTRLA = 0u;
    tx->DMA_CHCTRLA = 0u;

    /*
     * A TA event is generated only by an auxiliary event abort or by pattern
     * matching.  CTRLB below disables PATEN; explicitly clear the event
     * control here, and have every descriptor reload this same zero through
     * BDCFG.EVCTRL.  BDEVCTRL is otherwise only descriptor storage and is not
     * applied to CHEVCTRL.
     */
    rx->DMA_CHEVCTRL = 0u;
    tx->DMA_CHEVCTRL = 0u;

    dma_build_descriptors();

    /* Channel registers hold the FIRST block (half 0). */
    rx->DMA_CHCTRLB = AUDIO_DMA_RX_CTRLB;
    rx->DMA_CHSSA   = sercom4_data_address();
    rx->DMA_CHDSA   = (uint32_t)audio_buffers_rx_block(0u);
    rx->DMA_CHSSTRD = 0u;
    rx->DMA_CHDSTRD = 0u;
    rx->DMA_CHXSIZ  = AUDIO_DMA_XSIZ_BITS;
    rx->DMA_CHNXT   = (uint32_t)&dma_desc_rx[1];

    tx->DMA_CHCTRLB = AUDIO_DMA_TX_CTRLB;
    tx->DMA_CHSSA   = (uint32_t)audio_buffers_tx_block(0u);
    tx->DMA_CHDSA   = sercom4_data_address();
    tx->DMA_CHSSTRD = 0u;
    tx->DMA_CHDSTRD = 0u;
    tx->DMA_CHXSIZ  = AUDIO_DMA_XSIZ_BITS;
    tx->DMA_CHNXT   = (uint32_t)&dma_desc_tx[1];

    /* Clear then arm the interrupt sources we count. */
    rx->DMA_CHINTENCLR = 0xFFFFFFFFu;
    tx->DMA_CHINTENCLR = 0xFFFFFFFFu;
    rx->DMA_CHINTF     = 0xFFFFFFFFu;
    tx->DMA_CHINTF     = 0xFFFFFFFFu;
    rx->DMA_CHINTENSET = AUDIO_DMA_INTEN_BITS;
    tx->DMA_CHINTENSET = AUDIO_DMA_INTEN_BITS;

    AUDIO_DMA_REGS->DMA_CTRLA |= DMA_CTRLA_ENABLE_Msk;

    /*
     * DMA0 raises one interrupt per channel-priority level, not per
     * channel. Lining the three sets of names up:
     *
     *   CHCTRLB.PRI : PRI_1 = 0, PRI_2 = 1, PRI_3 = 2 (highest)
     *   DMA_INTSTATn: INTSTAT1/2/3, captioned "DMA Channel active
     *                 interrupt at priority 1 / 2 / 3"
     *   NVIC        : DMA0_PRI0_IRQn (33), DMA0_PRI1_IRQn (34),
     *                 DMA0_PRI2_IRQn (35) - zero-based vector names
     *
     * So DMA0_PRI0 most likely serves priority 1 / INTSTAT1, and by that
     * reading our PRI_3 channels would land on DMA0_PRI2_IRQn. The
     * one-off between "PRI0" and "priority 1" is precisely the kind of
     * naming seam worth not betting on, and enabling all three vectors
     * costs two extra table entries and nothing else - the shared handler
     * scans both channels' CHINTF regardless of which vector fired.
     *
     * Narrowing this to a single vector is a safe cleanup once first light
     * shows which one actually fires.
     */
    /* Keep the short 230400-baud USART RX handler one NVIC level above the
     * comparatively long DRC callback so command bytes cannot overrun. */
    NVIC_SetPriority(DMA0_PRI0_IRQn, 1u);
    NVIC_SetPriority(DMA0_PRI1_IRQn, 1u);
    NVIC_SetPriority(DMA0_PRI2_IRQn, 1u);
    NVIC_EnableIRQ(DMA0_PRI0_IRQn);
    NVIC_EnableIRQ(DMA0_PRI1_IRQn);
    NVIC_EnableIRQ(DMA0_PRI2_IRQn);
}

static void dma_arm(bool on)
{
    dma_channel_registers_t *const rx = AUDIO_DMA_CH(AUDIO_DMA_CH_RX);
    dma_channel_registers_t *const tx = AUDIO_DMA_CH(AUDIO_DMA_CH_TX);

    if (on)
    {
        /* LLEN makes the channel follow CHNXT at every block boundary. */
        tx->DMA_CHCTRLA = DMA_CHCTRLA_LLEN_Msk | DMA_CHCTRLA_ENABLE_Msk;
        rx->DMA_CHCTRLA = DMA_CHCTRLA_LLEN_Msk | DMA_CHCTRLA_ENABLE_Msk;
    }
    else
    {
        rx->DMA_CHCTRLA = 0u;
        tx->DMA_CHCTRLA = 0u;
    }
}

/* ------------------------------------------------------------------ */
/* ISR - counters only, never any I/O                                 */
/* ------------------------------------------------------------------ */

/* Preserve the actual hardware state before CHINTF is acknowledged. */
static void dma_capture_ta(volatile audio_dma_ta_snapshot_t *out,
                           dma_channel_registers_t *ch,
                           uint32_t flags)
{
    if (out->valid)
    {
        return;                 /* first abort is the decisive one */
    }

    dma_ta_diag.dma_intstat3    = AUDIO_DMA_REGS->DMA_INTSTAT3;
    dma_ta_diag.dma_intstat2    = AUDIO_DMA_REGS->DMA_INTSTAT2;
    dma_ta_diag.dma_intstat1    = AUDIO_DMA_REGS->DMA_INTSTAT1;
    dma_ta_diag.sercom_status   = AUDIO_SPIS->SERCOM_STATUS;
    dma_ta_diag.sercom_intflag  = AUDIO_SPIS->SERCOM_INTFLAG;
    dma_ta_diag.sercom_fifospace = AUDIO_SPIS->SERCOM_FIFOSPACE;

    out->chctrla    = ch->DMA_CHCTRLA;
    out->chctrlb    = ch->DMA_CHCTRLB;
    out->chevctrl   = ch->DMA_CHEVCTRL;
    out->chinten    = ch->DMA_CHINTENSET;
    out->chintf     = flags;
    out->chssa      = ch->DMA_CHSSA;
    out->chdsa      = ch->DMA_CHDSA;
    out->chnxt      = ch->DMA_CHNXT;
    out->chxsiz     = ch->DMA_CHXSIZ;
    out->chllcfgstat = ch->DMA_CHLLCFGSTAT;
    out->chstatbc   = ch->DMA_CHSTATBC;
    out->chstatcc   = ch->DMA_CHSTATCC;
    out->chstat     = ch->DMA_CHSTAT;
    out->valid      = true;
}

static void dma0_service(void)
{
#if APP_ENABLE_AUDIO_DRC
    const uint32_t callback_started = sonora_drc_path_callback_begin();
#endif
    dma_channel_registers_t *const rx = AUDIO_DMA_CH(AUDIO_DMA_CH_RX);
    dma_channel_registers_t *const tx = AUDIO_DMA_CH(AUDIO_DMA_CH_TX);

    const uint32_t rf = rx->DMA_CHINTF;
    const uint32_t tf = tx->DMA_CHINTF;

#if APP_AUDIO_LOOPTHROUGH
    bool     rx_complete = false;
    bool     tx_complete = false;
    uint32_t rx_half = 0u;
    uint32_t tx_half = 0u;
#endif

    /*
     * BC is not proof of a completed block: DS60001795 26.7 "Set the BC bit when
     * a block transfer completes or is aborted", and on an abort TA is set in the
     * same CHINTF. An aborted block was not filled, so it must neither count as a
     * completed block nor advance the ping/pong sequence - that would hand the
     * foreground a half the DMA never wrote.
     */
    if (rf != 0u)
    {
        if ((rf & DMA_CHINTF_TA_Msk) != 0u)
        {
            dma_capture_ta(&dma_ta_diag.rx, rx, rf);
            stat_inc(&audio_stats.dma_rx_ta_count);
        }
        else if ((rf & DMA_CHINTF_BC_Msk) != 0u)
        {
#if APP_AUDIO_LOOPTHROUGH
            /* seq & 1 is the half the channel had been filling until this BC. */
            rx_half = rx_block_seq & 1u;
            rx_complete = true;
#endif
            stat_inc(&audio_stats.audio_rx_blocks);
            rx_block_seq++;
        }
        if ((rf & DMA_CHINTF_RDE_Msk) != 0u)
        {
            stat_inc(&audio_stats.dma_rx_rde_count);
        }
        if ((rf & DMA_CHINTF_WRE_Msk) != 0u)
        {
            stat_inc(&audio_stats.dma_rx_wre_count);
        }
        rx->DMA_CHINTF = rf;
    }

    if (tf != 0u)
    {
        if ((tf & DMA_CHINTF_TA_Msk) != 0u)
        {
            dma_capture_ta(&dma_ta_diag.tx, tx, tf);
            stat_inc(&audio_stats.dma_tx_ta_count);
        }
        else if ((tf & DMA_CHINTF_BC_Msk) != 0u)
        {
#if APP_AUDIO_LOOPTHROUGH
            /* This half has just been transmitted and is now the safe target. */
            tx_half = tx_block_seq & 1u;
            tx_complete = true;
#endif
            stat_inc(&audio_stats.audio_tx_blocks);
            tx_block_seq++;
        }
        if ((tf & DMA_CHINTF_RDE_Msk) != 0u)
        {
            stat_inc(&audio_stats.dma_tx_rde_count);
        }
        if ((tf & DMA_CHINTF_WRE_Msk) != 0u)
        {
            stat_inc(&audio_stats.dma_tx_wre_count);
        }
        tx->DMA_CHINTF = tf;
    }

#if APP_AUDIO_LOOPTHROUGH
    /* A transfer abort or DMA bus error invalidates the real-time pair even
     * when the other channel happened to signal BC in this same invocation. */
    const bool dma_fault = ((rf | tf) & (DMA_CHINTF_TA_Msk |
                                         DMA_CHINTF_RDE_Msk |
                                         DMA_CHINTF_WRE_Msk)) != 0u;
#if APP_ENABLE_AUDIO_DRC
    const bool processed = loop_service_completions(
        rx_complete, rx_half, tx_complete, tx_half, dma_fault);
    if (processed)
    {
        sonora_drc_path_callback_end(callback_started);
    }
#else
    (void)loop_service_completions(
        rx_complete, rx_half, tx_complete, tx_half, dma_fault);
#endif
#endif
}

/* Foreground sweep of the two flags that cannot raise an interrupt. */
static void dma_error_harvest(void)
{
    dma_channel_registers_t *const rx = AUDIO_DMA_CH(AUDIO_DMA_CH_RX);
    dma_channel_registers_t *const tx = AUDIO_DMA_CH(AUDIO_DMA_CH_TX);

    const uint32_t err_msk = DMA_CHINTF_RDE_Msk | DMA_CHINTF_WRE_Msk;

#if APP_AUDIO_LOOPTHROUGH
    bool dma_error = false;
#endif

    const uint32_t rf = rx->DMA_CHINTF & err_msk;
    if (rf != 0u)
    {
#if APP_AUDIO_LOOPTHROUGH
        dma_error = true;
#endif
        if ((rf & DMA_CHINTF_RDE_Msk) != 0u)
        {
            stat_inc(&audio_stats.dma_rx_rde_count);
        }
        if ((rf & DMA_CHINTF_WRE_Msk) != 0u)
        {
            stat_inc(&audio_stats.dma_rx_wre_count);
        }
        rx->DMA_CHINTF = rf;
    }

    const uint32_t tf = tx->DMA_CHINTF & err_msk;
    if (tf != 0u)
    {
#if APP_AUDIO_LOOPTHROUGH
        dma_error = true;
#endif
        if ((tf & DMA_CHINTF_RDE_Msk) != 0u)
        {
            stat_inc(&audio_stats.dma_tx_rde_count);
        }
        if ((tf & DMA_CHINTF_WRE_Msk) != 0u)
        {
            stat_inc(&audio_stats.dma_tx_wre_count);
        }
        tx->DMA_CHINTF = tf;
    }

#if APP_AUDIO_LOOPTHROUGH
    if (dma_error)
    {
        loop_latch_fault();
    }
#endif
}

void DMA0_PRI0_Handler(void) { dma0_service(); }
void DMA0_PRI1_Handler(void) { dma0_service(); }
void DMA0_PRI2_Handler(void) { dma0_service(); }

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

bool audio_transport_init(void)
{
    /* TX gets its start-of-run contents here (silence, or the debug pattern),
     * so the very first frame the codec clocks out is already defined. */
    audio_buffers_reset();

    /* The start-up history survives a re-init, so repeated starts (Test B/C)
     * can be counted; everything else starts from zero. */
    const uint32_t attempts = audio_stats.sync_attempts;
    const uint32_t fails    = audio_stats.sync_fail;
    audio_stats               = (audio_stats_t){ 0 };
    audio_stats.sync_attempts = attempts;
    audio_stats.sync_fail     = fails;
    dma_ta_diag               = (audio_dma_diagnostics_t){ 0 };

    rx_block_seq       = 0u;
    tx_block_seq       = 0u;
    rx_block_consumed  = 0u;
    rx_block_seq_start = 0u;
    rx_first_half      = -1;
    tx_first_half      = -1;
    tur_seen           = false;
    transport_running  = false;

#if APP_AUDIO_LOOPTHROUGH
    audio_loopthrough.state           = AUDIO_LOOPTHROUGH_OFF;
    audio_loopthrough.copied_blocks   = 0u;
    audio_loopthrough.silenced_blocks = 0u;
    audio_loopthrough.pair_faults     = 0u;
    loop_rx_pending                   = false;
    loop_tx_pending                   = false;
    loop_rx_pending_half              = 0u;
    loop_tx_pending_half              = 0u;
    loop_drain_blocks                 = 0u;
#if APP_ENABLE_AUDIO_DRC
    sonora_drc_path_reset();
#endif
#endif

    sercom4_init();
    dma_init();

    return true;
}

/* Which half a channel's start-address register points at: 0, 1, or -1. */
static int32_t half_of(uint32_t addr, int32_t *(*block)(uint32_t))
{
    if (addr == (uint32_t)block(0u)) { return 0; }
    if (addr == (uint32_t)block(1u)) { return 1; }
    return -1;
}

bool audio_transport_start(void)
{
    if (transport_running)
    {
        return true;
    }

    sercom_spis_registers_t *const s  = AUDIO_SPIS;
    dma_channel_registers_t *const rx = AUDIO_DMA_CH(AUDIO_DMA_CH_RX);
    dma_channel_registers_t *const tx = AUDIO_DMA_CH(AUDIO_DMA_CH_TX);

    stat_inc(&audio_stats.sync_attempts);

    /*
     * Order: DMA armed first, SERCOM enabled LAST - and the codec's BCLK/FS may
     * already be running. The SERCOM is the only gate that can come last: with
     * the DMA still off, an enabled client fills its RX FIFO, hits BUFOVF and
     * then "will not respond to SCK transitions" (DS60001795 FIFO, client mode).
     * No CPU write to DATA here: it would queue one word ahead of the DMA block
     * and shift every TX slot by one for the rest of the run.
     */
    s->SERCOM_STATUS  = (uint16_t)(SERCOM_SPIS_STATUS_BUFOVF_Msk |
                                   SERCOM_SPIS_STATUS_TUR_Msk |
                                   SERCOM_SPIS_STATUS_LENERR_Msk);
    s->SERCOM_INTFLAG = (uint8_t)SERCOM_SPIS_INTFLAG_ERROR_Msk;

    dma_arm(true);
    sercom4_enable(true);
    transport_running = true;

    /*
     * Alignment check. What the SERCOM does when enabled in the middle of a
     * frame is not documented. If it waits for the next FSYNC nothing below
     * fires. If it counts from the current BCLK, the next FSYNC lands inside
     * its frame and sets LENERR - and the partial frame has already shifted
     * the DMA's word count permanently. LENERR is therefore the alignment
     * verdict. A one-off initial TUR is separately handled below: at enable,
     * the first frame clock can reach the empty TX FIFO before the first DMA
     * request is serviced even when all later frames are healthy.
     */
    board_delay_us(AUDIO_SYNC_WINDOW_US);

    const uint16_t st = s->SERCOM_STATUS;
    if ((st & SERCOM_SPIS_STATUS_LENERR_Msk) != 0u) { stat_inc(&audio_stats.startup_lenerr); }
    if ((st & SERCOM_SPIS_STATUS_TUR_Msk)    != 0u) { stat_inc(&audio_stats.startup_tur); }
    if ((st & SERCOM_SPIS_STATUS_BUFOVF_Msk) != 0u) { stat_inc(&audio_stats.startup_bufovf); }

    if ((st & SERCOM_SPIS_STATUS_LENERR_Msk) != 0u)
    {
        audio_transport_stop();
        stat_inc(&audio_stats.sync_fail);
        sync_state = AUDIO_SYNC_MISALIGNED;
        return false;
    }

    /*
     * DS60001795 says that clearing TUR flushes the TX FIFO.  That is not
     * safe in a steady stream, but here it is still inside the start-up
     * window. Clear a one-off first-frame TUR/BUFOVF, then give the DMA two
     * more frame periods to refill and prove that TUR does not recur.  A
     * repeating TUR is a real missing-word failure and is refused.
     */
    const uint16_t startup_fifo_flags =
        (uint16_t)(st & (SERCOM_SPIS_STATUS_TUR_Msk |
                         SERCOM_SPIS_STATUS_BUFOVF_Msk));
    if (startup_fifo_flags != 0u)
    {
        s->SERCOM_STATUS  = startup_fifo_flags;
        s->SERCOM_INTFLAG = (uint8_t)SERCOM_SPIS_INTFLAG_ERROR_Msk;
        board_delay_us(AUDIO_SYNC_WINDOW_US);

        const uint16_t after_clear = s->SERCOM_STATUS;
        if ((after_clear & SERCOM_SPIS_STATUS_TUR_Msk) != 0u)
        {
            stat_inc(&audio_stats.startup_tur);
            audio_transport_stop();
            stat_inc(&audio_stats.sync_fail);
            sync_state = AUDIO_SYNC_MISALIGNED;
            return false;
        }
        if ((after_clear & SERCOM_SPIS_STATUS_BUFOVF_Msk) != 0u)
        {
            stat_inc(&audio_stats.startup_bufovf);
            s->SERCOM_STATUS = SERCOM_SPIS_STATUS_BUFOVF_Msk;
            s->SERCOM_INTFLAG = (uint8_t)SERCOM_SPIS_INTFLAG_ERROR_Msk;
        }
    }

    /*
     * Which half is the first block? dma_arm() writes ENABLE|LLEN together, and
     * the datasheet does not say whether the channel runs the half-0 registers
     * first or loads desc[1] first. The start-address registers hold whichever
     * block is running now (the first one: a block is 32 frames, the window is
     * about 5), so read them instead of assuming. The ping/pong sequence is
     * then seeded so that the first BC reports the half that really completed.
     */
    const bool nvic_on = (NVIC_GetEnableIRQ(DMA0_PRI0_IRQn) != 0u);
    NVIC_DisableIRQ(DMA0_PRI0_IRQn);
    NVIC_DisableIRQ(DMA0_PRI1_IRQn);
    NVIC_DisableIRQ(DMA0_PRI2_IRQn);

    rx_first_half = half_of(rx->DMA_CHDSA, audio_buffers_rx_block);
    tx_first_half = half_of(tx->DMA_CHSSA, audio_buffers_tx_block);
    if ((rx_block_seq == 0u) && (rx_first_half == 1))
    {
        rx_block_seq       = 1u;
        rx_block_consumed  = 1u;
        rx_block_seq_start = 1u;
    }
    if ((tx_block_seq == 0u) && (tx_first_half == 1))
    {
        tx_block_seq = 1u;
    }
    const uint32_t rx_seq_at_proof = rx_block_seq;
    const uint32_t tx_seq_at_proof = tx_block_seq;

    if (nvic_on)
    {
        NVIC_EnableIRQ(DMA0_PRI0_IRQn);
        NVIC_EnableIRQ(DMA0_PRI1_IRQn);
        NVIC_EnableIRQ(DMA0_PRI2_IRQn);
    }

    /* A one-off TUR can clear cleanly yet leave the TX side without another
     * request.  Do not call that LOCKED: prove that each direction reports a
     * completed DMA block before exposing this start to the application. */
    board_delay_us(AUDIO_START_PROGRESS_WINDOW_US);
    const bool rx_stalled = (rx_block_seq == rx_seq_at_proof);
    const bool tx_stalled = (tx_block_seq == tx_seq_at_proof);
    if (rx_stalled || tx_stalled)
    {
        if (rx_stalled) { stat_inc(&audio_stats.startup_rx_stall); }
        if (tx_stalled) { stat_inc(&audio_stats.startup_tx_stall); }
        audio_transport_stop();
        stat_inc(&audio_stats.sync_fail);
        sync_state = AUDIO_SYNC_STALLED;
        return false;
    }

    sync_state = AUDIO_SYNC_LOCKED;
    return true;
}

void audio_transport_stop(void)
{
    /*
     * Order: SERCOM off first, then DMA.
     *
     * Disabling the SERCOM removes the trigger source, so after it no channel
     * can be re-triggered while it is being torn down. The codec clock may keep
     * running: a disabled SERCOM ignores BCLK/FS. A channel stopped mid-block
     * is only suspended; the reprogramming in the next audio_transport_init()
     * resets it (DS60001795 26.8.2).
     */
    sercom4_enable(false);
    dma_arm(false);
    sync_state = AUDIO_SYNC_IDLE;

    /* Drop anything the ISR latched during the tear-down, so a later restart
     * does not immediately look like it has a block ready. */
    rx_block_consumed = rx_block_seq;
    transport_running = false;

#if APP_AUDIO_LOOPTHROUGH
    loop_rx_pending = false;
    loop_tx_pending = false;
    audio_loopthrough.state = AUDIO_LOOPTHROUGH_OFF;
#endif
}

bool audio_transport_is_running(void)
{
    return transport_running;
}

bool audio_transport_loopthrough_start(void)
{
#if APP_AUDIO_LOOPTHROUGH
    if (!transport_running || (sync_state != AUDIO_SYNC_LOCKED) ||
        audio_buffers_tx_is_test_pattern())
    {
        return false;
    }

    const bool nvic_on = (NVIC_GetEnableIRQ(DMA0_PRI0_IRQn) != 0u);
    NVIC_DisableIRQ(DMA0_PRI0_IRQn);
    NVIC_DisableIRQ(DMA0_PRI1_IRQn);
    NVIC_DisableIRQ(DMA0_PRI2_IRQn);

    if (audio_loopthrough.state != AUDIO_LOOPTHROUGH_OFF)
    {
        if (nvic_on)
        {
            NVIC_EnableIRQ(DMA0_PRI0_IRQn);
            NVIC_EnableIRQ(DMA0_PRI1_IRQn);
            NVIC_EnableIRQ(DMA0_PRI2_IRQn);
        }
        return false;
    }

    audio_loopthrough.state           = AUDIO_LOOPTHROUGH_PRIMING;
    audio_loopthrough.copied_blocks   = 0u;
    audio_loopthrough.silenced_blocks = 0u;
    audio_loopthrough.pair_faults     = 0u;
    loop_rx_pending                   = false;
    loop_tx_pending                   = false;
    loop_rx_pending_half              = 0u;
    loop_tx_pending_half              = 0u;
    loop_drain_blocks                 = 0u;

    if (nvic_on)
    {
        NVIC_EnableIRQ(DMA0_PRI0_IRQn);
        NVIC_EnableIRQ(DMA0_PRI1_IRQn);
        NVIC_EnableIRQ(DMA0_PRI2_IRQn);
    }
    return true;
#else
    return false;
#endif
}

void audio_transport_loopthrough_stop(void)
{
#if APP_AUDIO_LOOPTHROUGH
    const bool nvic_on = (NVIC_GetEnableIRQ(DMA0_PRI0_IRQn) != 0u);
    NVIC_DisableIRQ(DMA0_PRI0_IRQn);
    NVIC_DisableIRQ(DMA0_PRI1_IRQn);
    NVIC_DisableIRQ(DMA0_PRI2_IRQn);

    if (audio_loopthrough.state != AUDIO_LOOPTHROUGH_OFF)
    {
        /* The app has already asserted HPOUT analogue mute.  Keep transport
         * clocks running long enough to refill both ping/pong TX halves with
         * zero before declaring the data path off. */
        audio_loopthrough.state = AUDIO_LOOPTHROUGH_DRAINING;
        loop_rx_pending = false;
        loop_tx_pending = false;
        loop_drain_blocks = 0u;
    }

    if (nvic_on)
    {
        NVIC_EnableIRQ(DMA0_PRI0_IRQn);
        NVIC_EnableIRQ(DMA0_PRI1_IRQn);
        NVIC_EnableIRQ(DMA0_PRI2_IRQn);
    }
#endif
}

void audio_transport_loopthrough_get_status(audio_loopthrough_status_t *out)
{
    if (out == NULL)
    {
        return;
    }

#if APP_AUDIO_LOOPTHROUGH
    const bool nvic_on = (NVIC_GetEnableIRQ(DMA0_PRI0_IRQn) != 0u);
    NVIC_DisableIRQ(DMA0_PRI0_IRQn);
    NVIC_DisableIRQ(DMA0_PRI1_IRQn);
    NVIC_DisableIRQ(DMA0_PRI2_IRQn);

    out->state           = audio_loopthrough.state;
    out->copied_blocks   = audio_loopthrough.copied_blocks;
    out->silenced_blocks = audio_loopthrough.silenced_blocks;
    out->pair_faults     = audio_loopthrough.pair_faults;

    if (nvic_on)
    {
        NVIC_EnableIRQ(DMA0_PRI0_IRQn);
        NVIC_EnableIRQ(DMA0_PRI1_IRQn);
        NVIC_EnableIRQ(DMA0_PRI2_IRQn);
    }
#else
    out->state           = AUDIO_LOOPTHROUGH_OFF;
    out->copied_blocks   = 0u;
    out->silenced_blocks = 0u;
    out->pair_faults     = 0u;
#endif
}

void audio_transport_get_state(audio_transport_state_t *out)
{
    if (out == NULL)
    {
        return;
    }

    /*
     * Mask the three DMA0 vectors for the read so every field comes from the
     * same instant. Without this, rx_blocks and rx_filling_half could easily
     * straddle a block boundary and disagree, which is exactly the kind of
     * inconsistency that wastes a debugging session.
     */
    const bool was_enabled = (NVIC_GetEnableIRQ(DMA0_PRI0_IRQn) != 0u);

    NVIC_DisableIRQ(DMA0_PRI0_IRQn);
    NVIC_DisableIRQ(DMA0_PRI1_IRQn);
    NVIC_DisableIRQ(DMA0_PRI2_IRQn);

    const uint32_t rxs = rx_block_seq;
    const uint32_t txs = tx_block_seq;

    out->running           = transport_running;
    out->rx_blocks         = audio_stats.audio_rx_blocks;
    out->tx_blocks         = audio_stats.audio_tx_blocks;
    out->rx_filling_half   = rxs & 1u;
    out->tx_reading_half   = txs & 1u;
    out->rx_last_done_half = (rxs - 1u) & 1u;
    out->tx_last_done_half = (txs - 1u) & 1u;
    out->rx_block_pending  = (rxs != rx_block_consumed);
    out->rx_dma_enabled    = (AUDIO_DMA_CH(AUDIO_DMA_CH_RX)->DMA_CHCTRLA & DMA_CHCTRLA_ENABLE_Msk) != 0u;
    out->tx_dma_enabled    = (AUDIO_DMA_CH(AUDIO_DMA_CH_TX)->DMA_CHCTRLA & DMA_CHCTRLA_ENABLE_Msk) != 0u;
    out->sync              = sync_state;
    out->rx_first_half     = rx_first_half;
    out->tx_first_half     = tx_first_half;

    if (was_enabled)
    {
        NVIC_EnableIRQ(DMA0_PRI0_IRQn);
        NVIC_EnableIRQ(DMA0_PRI1_IRQn);
        NVIC_EnableIRQ(DMA0_PRI2_IRQn);
    }
}

uint32_t audio_transport_peek_rx(int32_t *dst, uint32_t count)
{
    if ((dst == NULL) || (count == 0u) || (rx_block_seq == rx_block_seq_start))
    {
        return 0u;
    }

    if (count > AUDIO_WORDS_PER_BLOCK)
    {
        count = AUDIO_WORDS_PER_BLOCK;
    }

    /*
     * The half the DMA has just finished. This is a foreground read of a
     * buffer the DMA is not writing, so no masking is needed - but it IS a
     * race against the next block boundary if the caller dawdles, which is why
     * this only ever copies a handful of words for a dump and is never used as
     * a data path.
     */
    const int32_t *const src = audio_buffers_rx_block((rx_block_seq - 1u) & 1u);

    for (uint32_t i = 0u; i < count; i++)
    {
        dst[i] = src[i];
    }
    return count;
}

bool audio_transport_rx_block_ready(void)
{
    return rx_block_seq != rx_block_consumed;
}

int32_t *audio_transport_get_rx_block(void)
{
    /* The ISR has just finished filling this half, so the half the DMA is
     * writing now is the other one. */
    return audio_buffers_rx_block((rx_block_seq - 1u) & 1u);
}

int32_t *audio_transport_get_tx_block(void)
{
    /* Fill the half the DMA is not currently reading. */
    return audio_buffers_tx_block(rx_block_seq & 1u);
}

void audio_transport_release_block(void)
{
    rx_block_consumed = rx_block_seq;
}

const audio_stats_t *audio_transport_stats(void)
{
    return &audio_stats;
}

void audio_transport_get_dma_diagnostics(audio_dma_diagnostics_t *out)
{
    if (out == NULL)
    {
        return;
    }

    const bool was_enabled = (NVIC_GetEnableIRQ(DMA0_PRI0_IRQn) != 0u);
    NVIC_DisableIRQ(DMA0_PRI0_IRQn);
    NVIC_DisableIRQ(DMA0_PRI1_IRQn);
    NVIC_DisableIRQ(DMA0_PRI2_IRQn);

    /* The source is volatile because the DMA ISR writes it. With all three
     * DMA vectors masked it is stable, so copy its complete byte image. */
    const volatile uint8_t *const src = (const volatile uint8_t *)&dma_ta_diag;
    uint8_t *const dst = (uint8_t *)out;
    for (size_t i = 0u; i < sizeof(*out); i++)
    {
        dst[i] = src[i];
    }

    if (was_enabled)
    {
        NVIC_EnableIRQ(DMA0_PRI0_IRQn);
        NVIC_EnableIRQ(DMA0_PRI1_IRQn);
        NVIC_EnableIRQ(DMA0_PRI2_IRQn);
    }
}

#if APP_AUDIO_LOOPTHROUGH
static inline void loop_stat_inc(volatile uint32_t *c)
{
    if (*c != 0xFFFFFFFFu)
    {
        (*c)++;
    }
}

static void loop_latch_fault(void)
{
    const audio_loopthrough_state_t state = audio_loopthrough.state;

    if ((state == AUDIO_LOOPTHROUGH_PRIMING) ||
        (state == AUDIO_LOOPTHROUGH_RUNNING))
    {
        loop_stat_inc(&audio_loopthrough.pair_faults);
        audio_loopthrough.state = AUDIO_LOOPTHROUGH_FAULT;
    }

    /* A half not paired before the next same-direction completion is no
     * longer safe to use.  Drop both pending records rather than risking a
     * write into a TX half which the DMA has resumed reading. */
    loop_rx_pending = false;
    loop_tx_pending = false;
}

static void loop_zero_tx_half(uint32_t half)
{
    int32_t *const dst = audio_buffers_tx_block(half);

    for (uint32_t w = 0u; w < AUDIO_WORDS_PER_BLOCK; w++)
    {
        dst[w] = 0;
    }
    __DMB();
}

static void loop_copy_rx_to_tx(uint32_t rx_half, uint32_t tx_half)
{
    const int32_t *const src = audio_buffers_rx_block(rx_half);
    int32_t *const       dst = audio_buffers_tx_block(tx_half);

#if APP_ENABLE_AUDIO_DRC
    /* One physical WM8904/SERCOM leg exists on this bench, so Sonora's codec-B
     * destination is NULL.  The full four-channel cascade still executes;
     * codec-A receives L1/R1 in TDM slots 0/1 exactly as Sonora does. */
    sonora_drc_path_process(src, dst, NULL);
#else
    /* Raw 32-bit words are intentional.  This transport layer neither knows
     * nor assumes a WM8904 channel map; the test proves the received serial
     * data is sent back byte-for-byte through the same DMA byte-order path. */
    for (uint32_t w = 0u; w < AUDIO_WORDS_PER_BLOCK; w++)
    {
        dst[w] = src[w];
    }
#endif
    __DMB();
}

/*
 * Pair completion events in time order, not by physical half number.  RX and
 * TX normally finish together, but their interrupts can arrive in different
 * invocations and their initial ping/pong halves are not guaranteed equal.
 * A TX BC identifies a half which is now free; it is the only safe target.
 */
static bool loop_service_completions(bool rx_complete, uint32_t rx_half,
                                     bool tx_complete, uint32_t tx_half,
                                     bool dma_fault)
{
    audio_loopthrough_state_t state = audio_loopthrough.state;
    bool processed = false;

    if (state == AUDIO_LOOPTHROUGH_OFF)
    {
        return false;
    }

    if (dma_fault)
    {
        loop_latch_fault();
        state = audio_loopthrough.state;
    }

    if ((state == AUDIO_LOOPTHROUGH_PRIMING) ||
        (state == AUDIO_LOOPTHROUGH_RUNNING))
    {
        if (rx_complete)
        {
            if (loop_rx_pending)
            {
                loop_latch_fault();
            }
            else
            {
                loop_rx_pending_half = rx_half;
                loop_rx_pending = true;
            }
        }

        state = audio_loopthrough.state;
        if (((state == AUDIO_LOOPTHROUGH_PRIMING) ||
             (state == AUDIO_LOOPTHROUGH_RUNNING)) && tx_complete)
        {
            if (loop_tx_pending)
            {
                loop_latch_fault();
            }
            else
            {
                loop_tx_pending_half = tx_half;
                loop_tx_pending = true;
            }
        }

        state = audio_loopthrough.state;
        if (((state == AUDIO_LOOPTHROUGH_PRIMING) ||
             (state == AUDIO_LOOPTHROUGH_RUNNING)) &&
            loop_rx_pending && loop_tx_pending)
        {
            loop_copy_rx_to_tx(loop_rx_pending_half, loop_tx_pending_half);
            processed = true;
            loop_rx_pending = false;
            loop_tx_pending = false;
            loop_stat_inc(&audio_loopthrough.copied_blocks);

            if ((state == AUDIO_LOOPTHROUGH_PRIMING) &&
                (audio_loopthrough.copied_blocks >= AUDIO_LOOPTHROUGH_PRIME_BLOCKS))
            {
                audio_loopthrough.state = AUDIO_LOOPTHROUGH_RUNNING;
            }
        }
    }

    state = audio_loopthrough.state;
    if (((state == AUDIO_LOOPTHROUGH_DRAINING) ||
         (state == AUDIO_LOOPTHROUGH_FAULT)) && tx_complete)
    {
        /* Once a TX half reaches BC, it is free until the other half finishes;
         * overwrite it with silence before the linked-list returns to it. */
        loop_zero_tx_half(tx_half);
        loop_stat_inc(&audio_loopthrough.silenced_blocks);

        if (state == AUDIO_LOOPTHROUGH_DRAINING)
        {
            loop_stat_inc(&loop_drain_blocks);
            if (loop_drain_blocks >= AUDIO_LOOPTHROUGH_DRAIN_BLOCKS)
            {
                audio_loopthrough.state = AUDIO_LOOPTHROUGH_OFF;
            }
        }
    }

    return processed;
}
#endif /* APP_AUDIO_LOOPTHROUGH */

void audio_transport_poll_errors(void)
{
    sercom_spis_registers_t *const s = AUDIO_SPIS;

    /* WRE / RDE have no interrupt enable bit, so sweep them here too. */
    dma_error_harvest();

    const uint16_t st = s->SERCOM_STATUS;

    if (st == 0u)
    {
        return;
    }

#if APP_AUDIO_LOOPTHROUGH
    /* A serial-interface error makes the stream untrustworthy even if DMA is
     * still moving.  The ISR will zero the next free TX half; the foreground
     * application sees FAULT and asserts the verified HPOUT analogue mute. */
    loop_latch_fault();
#endif

    if (((st & SERCOM_SPIS_STATUS_TUR_Msk) != 0u) && !tur_seen)
    {
        stat_inc(&audio_stats.sercom_tur_count);
        tur_seen = true;
    }
    if ((st & SERCOM_SPIS_STATUS_BUFOVF_Msk) != 0u)
    {
        stat_inc(&audio_stats.sercom_bufovf_count);
    }
    if ((st & SERCOM_SPIS_STATUS_LENERR_Msk) != 0u)
    {
        stat_inc(&audio_stats.sercom_lenerr_count);
    }

    /* Write-1-to-clear the flags we just accounted for - except TUR: clearing
     * it flushes the TX FIFO mid-stream and shifts every later TX slot. It
     * stays set (TX then sends zeros, visibly) until the next init's SWRST. */
    s->SERCOM_STATUS  = (uint16_t)(st & (uint16_t)~SERCOM_SPIS_STATUS_TUR_Msk);
    s->SERCOM_INTFLAG = (uint8_t)SERCOM_SPIS_INTFLAG_ERROR_Msk;
}
