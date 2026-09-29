/*
 * app_config.h - single place for every compile-time option of the
 *                PIC32CK ARM Audio Nano firmware.
 *
 * Target : PIC32CK2051SG01064
 * Board  : EV33V87A (PIC32CK SG01 Curiosity Nano + Touch)
 * Carrier: AC164162 (Curiosity Nano Base for Click boards), mikroBUS slot 1
 * Codec  : WM8904 mikroBUS Rev5, audio clock master
 *
 * Target scope: standalone codec and transport firmware.
 * No RTOS, no Harmony middleware, no DSP, no dynamic allocation.
 */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/*
 * MPLAB X owns only two permanent images.  Their one-switch profiles are
 * deliberately expanded here, rather than into a list of independent IDE
 * flags, so a command-line -D can override an individual optional setting without
 * redefinition order deciding the resulting ROM image.
 *
 *   APP_PROFILE_CMSIS_DRC          vendor CMSIS-DSP DF2T baseline
 *   APP_PROFILE_CMSIS_M33OPT_DRC   project M33 kernel, CMSIS-DF2T API
 *
 * The high-load M33 profile is selected on the command line rather than as a
 * third permanent MPLAB configuration.
 */
#if defined(APP_PROFILE_AUDIO_DRC_M33OPT_99) && \
    (APP_PROFILE_AUDIO_DRC_M33OPT_99 != 0)
#ifndef APP_PROFILE_CMSIS_M33OPT_DRC
#define APP_PROFILE_CMSIS_M33OPT_DRC        1
#endif
#ifndef APP_AUDIO_DRC_STAGES
#define APP_AUDIO_DRC_STAGES                54u
#endif
#ifndef APP_ALLOW_NONREFERENCE_DRC_STAGES
#define APP_ALLOW_NONREFERENCE_DRC_STAGES   1
#endif
#ifndef APP_ENABLE_SONORA_SAMPLE_DELAY
#define APP_ENABLE_SONORA_SAMPLE_DELAY      0
#endif
#endif

#if defined(APP_PROFILE_CMSIS_M33OPT_DRC) && \
    (APP_PROFILE_CMSIS_M33OPT_DRC != 0)
#ifndef APP_PROFILE_CMSIS_DRC
#define APP_PROFILE_CMSIS_DRC               1
#endif
#ifndef DSP_USE_M33_DF2T_OPT
#define DSP_USE_M33_DF2T_OPT                1
#endif
#ifndef DSP_M33_NO_FMA_CONTRACT
#define DSP_M33_NO_FMA_CONTRACT             1
#endif
#endif

#if defined(APP_PROFILE_CMSIS_DRC) && (APP_PROFILE_CMSIS_DRC != 0)
#ifndef APP_ENABLE_AUDIO_TRANSPORT
#define APP_ENABLE_AUDIO_TRANSPORT          1
#endif
#ifndef APP_ENABLE_WM8904_AUDIO_STARTUP
#define APP_ENABLE_WM8904_AUDIO_STARTUP     1
#endif
#ifndef APP_AUDIO_LOOPTHROUGH
#define APP_AUDIO_LOOPTHROUGH               1
#endif
#ifndef APP_AUDIO_TX_TEST_PATTERN
#define APP_AUDIO_TX_TEST_PATTERN           0
#endif
#ifndef APP_ENABLE_AUDIO_DRC
#define APP_ENABLE_AUDIO_DRC                1
#endif
#ifndef APP_ENABLE_SONORA_SAMPLE_DELAY
#define APP_ENABLE_SONORA_SAMPLE_DELAY      1
#endif
#ifndef APP_AUDIO_DRC_STAGES
#define APP_AUDIO_DRC_STAGES                35u
#endif
#ifndef APP_ALLOW_NONREFERENCE_DRC_STAGES
#define APP_ALLOW_NONREFERENCE_DRC_STAGES   1
#endif
#endif

/* The CMSIS source unity includes this header before the vendored source.
 * Keep its normal loop-unrolled implementation as the baseline; -D...=0
 * selects the non-unrolled implementation for controlled comparisons. */
#ifndef APP_ENABLE_CMSIS_LOOPUNROLL
#define APP_ENABLE_CMSIS_LOOPUNROLL         1
#endif
#if APP_ENABLE_CMSIS_LOOPUNROLL
#ifndef ARM_MATH_LOOPUNROLL
#define ARM_MATH_LOOPUNROLL
#endif
#endif

/* ------------------------------------------------------------------ */
/* Feature gates                                                       */
/* ------------------------------------------------------------------ */

/* WM8904 I2C probe. Safe to leave on: it never blocks forever, and a
 * missing codec only prints a failure line. */
#ifndef APP_ENABLE_WM8904_PROBE
#define APP_ENABLE_WM8904_PROBE     1
#endif

/* SERCOM4 framed-SPI + DMA audio transport.
 * Defaults to 0: the code is compiled and linked but
 * main() never starts it, so the firmware boots safely with no codec,
 * no BCLK and no FSYNC present. */
#ifndef APP_ENABLE_AUDIO_TRANSPORT
#define APP_ENABLE_AUDIO_TRANSPORT  0
#endif

/* WM8904 48 kHz / TDM8 / codec-master audio start-up.
 * Default 0 for the same reason the transport defaults off: with this at 0 the
 * firmware boots on a bare board and never writes an audio-clock register.
 * At 1 the codec is configured AND its BCLK/FS are started, which only makes
 * sense with the WM8904 Click actually fitted. */
#ifndef APP_ENABLE_WM8904_AUDIO_STARTUP
#define APP_ENABLE_WM8904_AUDIO_STARTUP  0
#endif

/*
 * TX test pattern (diagnostic only, default OFF).
 *
 * Normally the TX ping/pong buffers are zero-filled, so the codec DAC
 * receives continuous silence. With this at 1 every slot of every frame is
 * filled with APP_AUDIO_TX_TEST_WORD instead, which exists for exactly one
 * purpose: putting a known word on the wire so a logic analyser can settle
 * DATA32B ordering, the DMAC byte swizzle and bit alignment in one capture.
 *
 * It is not an audio feature and nothing reads it back.
 */
#ifndef APP_AUDIO_TX_TEST_PATTERN
#define APP_AUDIO_TX_TEST_PATTERN   0
#endif

/*
 * Firmware loop-through (audio-path diagnostic, default OFF).
 *
 * At 1, the transport copies completed codec RX DMA blocks to an available TX
 * DMA block.  This gate deliberately does not make sound by itself: HPOUT
 * remains analogue-muted until the foreground safety state machine has primed
 * valid blocks. The live DRC image requests playback at boot and unmutes only
 * after the existing two-block prime gate; `l` remains a manual re-start key.
 */
#ifndef APP_AUDIO_LOOPTHROUGH
#define APP_AUDIO_LOOPTHROUGH        0
#endif

/*
 * Classic/DRC path in the live audio transport.
 *
 * This gate changes only what happens at the already-paired RX/free-TX point
 * in the DMA ISR.  Transport ownership, priming, mute, drain and fault rules
 * remain the verified loop-through implementation. The standard CMSIS profile
 * uses 36 stages; an alternative stage count is selected explicitly through a
 * guarded -D override.
 */
#ifndef APP_ENABLE_AUDIO_DRC
#define APP_ENABLE_AUDIO_DRC          0
#endif

#ifndef APP_AUDIO_DRC_STAGES
#define APP_AUDIO_DRC_STAGES          36u
#endif

/* Classic/DRC CSV receiver policy. The normal receiver parses each
 * complete row as it arrives; deferred whole-file parsing remains an explicit
 * diagnostic build option and is not silently enabled. */
#ifndef APP_CSV_DEFER_PARSE
#define APP_CSV_DEFER_PARSE            0
#endif

#ifndef APP_CSV_RAW_BUF_BYTES
#define APP_CSV_RAW_BUF_BYTES          (8u * 1024u)
#endif

/* Optional Classic DSP blocks. The live DRC profiles enable the Sonora
 * sample-delay feature above; non-DRC and isolated builds keep it off. */
#ifndef APP_ENABLE_SONORA_SAMPLE_DELAY
#define APP_ENABLE_SONORA_SAMPLE_DELAY  0
#endif

#ifndef AUDIO_SAMPLE_DELAY_POOL_BYTES
#define AUDIO_SAMPLE_DELAY_POOL_BYTES   (22u * 1024u)
#endif

#if APP_ENABLE_SONORA_SAMPLE_DELAY && !APP_ENABLE_AUDIO_DRC
#error "sample delay requires the Classic/DRC audio path"
#endif

/* The permanent live image is the 35-stage, feature-complete CMSIS baseline.
 * Stage-count overrides remain explicit so the selected operating point is
 * reproducible. */
#ifndef APP_ALLOW_NONREFERENCE_DRC_STAGES
#define APP_ALLOW_NONREFERENCE_DRC_STAGES 1
#endif

/* Foreground telemetry. Zero disables the periodic
 * report; `*tq0001` restores this resolved default at runtime. */
#ifndef APP_DBG_PERIOD_MS
#define APP_DBG_PERIOD_MS             2000u
#endif

/*
 * The word the test pattern writes into every slot. 0x12345600 is chosen
 * because all four bytes differ, so any byte permutation on the wire is
 * unambiguous: MSB-first the expected wire order is 12 34 56 00.
 * The transport validation uses this word to expose every byte position.
 */
#ifndef APP_AUDIO_TX_TEST_WORD
#define APP_AUDIO_TX_TEST_WORD      0x12345600
#endif

/* A wire-test image must never also be an audible firmware loop-through
 * image.  Keep the test pattern and the data path as separate configurations
 * so a stale logic-analyser build cannot be mistaken for a headphone test. */
#if APP_AUDIO_LOOPTHROUGH && APP_AUDIO_TX_TEST_PATTERN
#error "audio loop-through and TX wire-test pattern are mutually exclusive"
#endif

#if APP_AUDIO_LOOPTHROUGH && \
    (!APP_ENABLE_AUDIO_TRANSPORT || !APP_ENABLE_WM8904_AUDIO_STARTUP)
#error "audio loop-through requires transport and WM8904 audio startup"
#endif

#if APP_ENABLE_AUDIO_DRC && !APP_AUDIO_LOOPTHROUGH
#error "live audio DRC requires the verified ISR loop-through state machine"
#endif

#if APP_ENABLE_AUDIO_DRC && APP_ENABLE_DSP_BENCH
#error "live audio and the isolated auto-benchmark are separate images"
#endif

/* Instruction cache. D-cache stays OFF for first light (DMA coherency);
 * see board_cache_init(). */
#ifndef APP_ENABLE_ICACHE
#define APP_ENABLE_ICACHE           1
#endif

/* ------------------------------------------------------------------ */
/* Clock profile                                                       */
/* ------------------------------------------------------------------ */
/*
 * PLL120 : FDPLL0 (PLL0) fed from DFLL48M -> GCLK0 -> CPU = 120 MHz.
 *          This is the project's final target clock.
 * DFLL48 : no PLL at all, GCLK0 = DFLL48M -> CPU = 48 MHz.
 *          One-line fallback if 120 MHz ever needs to be taken out of
 *          the picture while debugging.
 *
 * Derivation of every PLL number is in board.c; there are no hand-picked
 * register values.
 */
#define APP_CLOCK_PROFILE_PLL120    1
#define APP_CLOCK_PROFILE_DFLL48    2

#ifndef APP_CLOCK_PROFILE
#define APP_CLOCK_PROFILE           APP_CLOCK_PROFILE_PLL120
#endif

/* DFLL48M in open loop. This is the reset state of the device
 * (OSCCTRL.DFLLCTRLA reset value = 0x82 -> ENABLE | ONDEMAND, and
 * DFLLCTRLB reset value = 0x00 -> LOOPEN = 0 = open loop), so the
 * firmware never programs the DFLL. */
#define APP_DFLL48M_HZ              48000000u

#if (APP_CLOCK_PROFILE == APP_CLOCK_PROFILE_PLL120)
  /* fPFD  = fREF / REFDIV  = 48 MHz / 48 =   1 MHz
   * fVCO  = fPFD * FBDIV   =  1 MHz * 240 = 240 MHz
   * fOUT0 = fVCO / POSTDIV0 = 240 MHz / 2 = 120 MHz
   *
   * fPFD = 1 MHz and fVCO = 240 MHz are exactly the operating point
   * MCC/Harmony's clk_pic32ck_gc_sg configurator ships as its default
   * (REFDIV 12 / FBDIV 240 from a 12 MHz XOSC); only REFDIV (to suit a
   * 48 MHz reference) and POSTDIV0 differ here. */
  #define APP_PLL0_REF_HZ           APP_DFLL48M_HZ
  #define APP_PLL0_REFDIV           48u
  #define APP_PLL0_FBDIV            240u
  #define APP_PLL0_POSTDIV0         2u
  #define APP_PLL0_BWSEL            1u    /* MCC default bandwidth      */
  #define APP_PLL0_VCO_HZ           ((APP_PLL0_REF_HZ / APP_PLL0_REFDIV) * APP_PLL0_FBDIV)
  #define APP_PLL0_OUT0_HZ          (APP_PLL0_VCO_HZ / APP_PLL0_POSTDIV0)
  #define APP_MAINCK_HZ             APP_PLL0_OUT0_HZ
#elif (APP_CLOCK_PROFILE == APP_CLOCK_PROFILE_DFLL48)
  #define APP_MAINCK_HZ             APP_DFLL48M_HZ
#else
  #error "APP_CLOCK_PROFILE must be APP_CLOCK_PROFILE_PLL120 or APP_CLOCK_PROFILE_DFLL48"
#endif

/*
 * Operating range, from EV33V87A User Guide DS50004178A / PIC32CK Family
 * Datasheet DS60001795H for PIC32CK2051SG01064:
 *
 *      -40 .. +85 degC   : DC .. 120 MHz
 *      -40 .. +125 degC  : DC .. 100 MHz
 *
 * 120 MHz is therefore inside the device specification for bench work at
 * room temperature. If this design is ever taken above +85 degC, the CPU
 * must drop to 100 MHz or below - switch APP_CLOCK_PROFILE, or change
 * APP_PLL0_POSTDIV0 (240 MHz VCO / 3 = 80 MHz is the next step down that
 * keeps the same MCC-default VCO point).
 */

/* MCLK.CLKDIV divides MAINCK to make the CPU clock. DIV1 keeps them equal.
 * A measurement build may override this with 2u to run the core at 60 MHz
 * while leaving the 48 MHz SERCOM GCLKs and codec clocks unchanged. */
#ifndef APP_MCLK_CLKDIV
#define APP_MCLK_CLKDIV             1u
#endif
#define APP_CPU_CLOCK_HZ            (APP_MAINCK_HZ / APP_MCLK_CLKDIV)

/* Peripheral generic clocks.
 * GCLK2 = DFLL48M / 1: SERCOM core clock. Deliberately independent of the
 *   CPU profile so that UART baud and I2C SCL do not move when the CPU
 *   clock changes.
 * GCLK3 = OSCULP32K / 1: SERCOM slow clock (I2C SDA hold timing).
 *   OSCULP32K is an always-available internal oscillator, so no crystal
 *   is required anywhere in this design. */
#define APP_GCLK_GEN_CPU            0u
#define APP_GCLK_GEN_SERCOM_CORE    2u
#define APP_GCLK_GEN_SERCOM_SLOW    3u
#define APP_GCLK_SERCOM_CORE_HZ     APP_DFLL48M_HZ
#define APP_GCLK_SERCOM_SLOW_HZ     32768u

/* ------------------------------------------------------------------ */
/* Console (SERCOM5 USART, CDC through the on-board debugger)           */
/* ------------------------------------------------------------------ */
#define APP_CONSOLE_BAUD            230400u

/* ------------------------------------------------------------------ */
/* WM8904 control interface (SERCOM0 I2C host)                         */
/* ------------------------------------------------------------------ */
#define APP_I2C_SCL_HZ              400000u
#define APP_I2C_TRISE_NS            100u    /* MCC default for this SERCOM */

/* ------------------------------------------------------------------ */
/* Audio frame / transport geometry                                    */
/* ------------------------------------------------------------------ */
#define AUDIO_FS_HZ                 48000u
#define AUDIO_BCLK_HZ               12288000u
#define AUDIO_SLOTS_PER_FRAME       8u
#define AUDIO_SLOT_BITS             32u
#define AUDIO_FRAME_BITS            (AUDIO_SLOTS_PER_FRAME * AUDIO_SLOT_BITS)   /* 256 */
#define AUDIO_FRAME_BYTES           (AUDIO_FRAME_BITS / 8u)                     /* 32  */
#define AUDIO_FRAMES_PER_BLOCK      32u
#define AUDIO_WORDS_PER_BLOCK       (AUDIO_FRAMES_PER_BLOCK * AUDIO_SLOTS_PER_FRAME) /* 256 */
#define AUDIO_BLOCK_BYTES           (AUDIO_FRAMES_PER_BLOCK * AUDIO_FRAME_BYTES)     /* 1024 */
#define AUDIO_BLOCK_COUNT           2u      /* ping / pong */

/* 32 frames / 48 kHz = 666.666... us per DMA block. */
#define AUDIO_BLOCK_TIME_NS         ((1000000000ull * AUDIO_FRAMES_PER_BLOCK) / AUDIO_FS_HZ)
#define APP_AUDIO_BLOCK_CYCLES      ((APP_CPU_CLOCK_HZ * AUDIO_FRAMES_PER_BLOCK) / AUDIO_FS_HZ)

/* ------------------------------------------------------------------ */
/* WM8904 codec clock tree (codec is the audio clock master)           */
/* ------------------------------------------------------------------ */
/*
 * The WM8904 mikroBUS board carries its own 12.288 MHz crystal on MCLK, so
 * the PIC32CK supplies no clock to the codec at all.
 *
 *   MCLK   = 12.288 MHz  (board crystal)
 *   SYSCLK = MCLK        (R20.MCLK_DIV = 0, R22.SYSCLK_SRC = 0, FLL off)
 *   BCLK   = SYSCLK / 1  (R26.BCLK_DIV = 00000 = "SYSCLK")   = 12.288 MHz
 *   LRCLK  = BCLK / 256  (R27.LRCLK_RATE = 256)              = 48 kHz
 *   SYSCLK/fs = 256      (R21.CLK_SYS_RATE = 0011 = 256)
 *
 * 48 kHz divides exactly from 12.288 MHz, which is why no FLL is needed.
 * The 44.1 kHz family would need one; it is out of scope here.
 */
#define WM8904_MCLK_HZ              12288000u
#define WM8904_SYSCLK_HZ            WM8904_MCLK_HZ
#define WM8904_SYSCLK_PER_FS        256u
#define WM8904_BCLK_DIV_RATIO       1u
#define WM8904_BCLK_HZ              (WM8904_SYSCLK_HZ / WM8904_BCLK_DIV_RATIO)
#define WM8904_LRCLK_RATE_BCLKS     (AUDIO_SLOTS_PER_FRAME * AUDIO_SLOT_BITS)   /* 256 */

/* ------------------------------------------------------------------ */
/* Compile-time arithmetic checks (no test framework, no runtime cost) */
/* ------------------------------------------------------------------ */
/* The frame geometry and the codec clock tree must agree, or the codec would
 * be told to make a BCLK the transport cannot frame. These are the two
 * identities required by the codec configuration. */
_Static_assert(WM8904_SYSCLK_HZ / WM8904_SYSCLK_PER_FS == AUDIO_FS_HZ,
               "SYSCLK / (SYSCLK/fs) must equal fs: 12288000 / 256 == 48000");
_Static_assert(AUDIO_FS_HZ * AUDIO_SLOTS_PER_FRAME * AUDIO_SLOT_BITS == WM8904_BCLK_HZ,
               "fs * slots * slot_bits must equal BCLK: 48000 * 8 * 32 == 12288000");
_Static_assert(WM8904_BCLK_HZ == AUDIO_BCLK_HZ,
               "codec BCLK and the transport's expected BCLK must be the same number");
_Static_assert(WM8904_BCLK_HZ / WM8904_LRCLK_RATE_BCLKS == AUDIO_FS_HZ,
               "BCLK / LRCLK_RATE must equal fs");
_Static_assert(WM8904_LRCLK_RATE_BCLKS == AUDIO_FRAME_BITS,
               "one LRCLK period must be exactly one 256-bit frame");
/* R27[10:0] LRCLK_RATE valid range is 8..2047 per the datasheet. */
_Static_assert((WM8904_LRCLK_RATE_BCLKS >= 8u) && (WM8904_LRCLK_RATE_BCLKS <= 2047u),
               "LRCLK_RATE must fit the datasheet's 8..2047 range");
_Static_assert(WM8904_SYSCLK_HZ == WM8904_MCLK_HZ,
               "this build takes SYSCLK straight from MCLK: MCLK_DIV=0 and FLL off");

/* ------------------------------------------------------------------ */
/* SERCOM4 framed-SPI client configuration                             */
/* ------------------------------------------------------------------ */
/*
 * Clock phase, derived rather than copied.
 *
 * WM8904 Rev 4.1 timing tables (both master and slave mode) state:
 *   - "ADCDAT propagation delay from BCLK falling edge" (tDDA / tDD)
 *     -> the codec CHANGES its output on the BCLK falling edge, so a
 *        receiver must SAMPLE on the rising edge.
 *   - "DACDAT setup time to BCLK rising edge" (tDST / tDS) and
 *     "DACDAT hold time from BCLK rising edge" (tDHT / tDH)
 *     -> the codec SAMPLES its input on the BCLK rising edge, so a
 *        transmitter must CHANGE on the falling edge.
 *
 * PIC32CK SERCOM (PIC32CK-SG DFP 1.6.199 ATDF value groups):
 *   - CPOL = 0 (IDLE_LOW)     : "SCK is low when idle"
 *                               => leading edge = rising, trailing = falling
 *   - CPHA = 0 (LEADING_EDGE) : "The data is sampled on a leading SCK edge
 *                               and changed on a trailing SCK edge"
 *
 * CPOL=0 / CPHA=0 therefore samples on rising and changes on falling -
 * exactly what the WM8904 requires. This is SPI mode 0.
 */
#define AUDIO_SPI_CPOL              0u
#define AUDIO_SPI_CPHA              0u

/*
 * Frame sync polarity. WM8904 DSP mode marks the start of a frame with a
 * rising edge on LRCLK ("the left channel MSB is available on either the
 * 1st (mode B) or 2nd (mode A) rising edge of BCLK ... following a rising
 * edge of LRCLK"), i.e. the pulse is high-active.
 * SERCOM CTRLC.FSPOL: 0 = HIGH = "VCC-level valid polarity".
 */
#define AUDIO_SPI_FS_POLARITY       0u    /* SERCOM_SPIS_CTRLC_FSPOL_HIGH */

/*
 * WM8904 DSP mode select (register AIF_LRCLK_INV):
 *   0 = mode A : MSB on the 2nd BCLK rising edge after LRCLK
 *   1 = mode B : MSB on the 1st BCLK rising edge after LRCLK
 *
 * This firmware selects mode A so that FSYNC precedes the MSB by one BCLK.
 * The SERCOM framed-SPI settings and the codec setting must be changed as a
 * pair; a different frame-phase convention changes every received word.
 */
#define AUDIO_WM8904_DSP_MODE       0u    /* 0 = DSP mode A */

/*
 * Frame sync length. DFP enum values and their ATDF captions:
 *   SERCOM_SPIS_CTRLC_FSLEN_STROBE_Val = 0  "One SCK pulse"
 *   SERCOM_SPIS_CTRLC_FSLEN_LEVEL_Val  = 1  "One frame duration"
 *
 * WM8904 in device-master mode emits a one-BCLK frame pulse: datasheet
 * figure 48 (DSP mode A, master) is annotated "1 BCLK" on LRCLK. That is
 * STROBE, i.e. value 0.
 *
 * Still to confirm on hardware: the datasheet also says that in device
 * *slave* mode "it is possible to use any length of frame pulse less than
 * 1/fs", so a WM8904 configured differently could present a wider pulse.
 * With the codec as clock master this is the one-BCLK case.
 */
#define AUDIO_SPI_FSLEN             0u    /* SERCOM_SPIS_CTRLC_FSLEN_STROBE */

/*
 * CTRLC.FSES ("Frame Synch Edge Select"), per DS60001795H:
 *   0 = frame sync active edge is BEFORE the first bit clock
 *   1 = frame sync active edge COINCIDES with the first bit clock
 *
 * The datasheet wording describes when the pulse is *generated*, which is
 * Frame Host behaviour. The Frame Client section is written independently:
 * on detecting FSYNC the client transmits from the subsequent transmit
 * edge. Only the FSES = 0 case is drawn in the Frame Client timing
 * figures.
 *
 * So FSES most likely does nothing in Frame Client mode. Left at 0, which
 * is both the reset value and the documented client case. Kept as a
 * first-light sanity check only - not a blocker.
 */
#define AUDIO_SPI_FSES              0u

/* LENGTH.LEN counts CHARACTERS, and CTRLB.CHSIZE on this SERCOM only
 * offers 8-bit and 9-bit characters (DFP: SERCOM_SPIS_CTRLB_CHSIZE_8_BIT
 * / _9_BIT). One 256-bit audio frame is therefore 32 eight-bit
 * characters. DATA32B only changes how the CPU/DMA *accesses* the DATA
 * register (4 characters per 32-bit access); it does not make the
 * shift register 32 bits wide. */
#define AUDIO_SPI_LENGTH_CHARS      AUDIO_FRAME_BYTES   /* 32 */

/* ------------------------------------------------------------------ */
/* DMA                                                                 */
/* ------------------------------------------------------------------ */
/* SERCOM4 DMA trigger indices. Taken straight from the DFP instance
 * header (include/instance/sercom4.h), which defines
 * SERCOM4_DMAC_ID_RX = 41 and SERCOM4_DMAC_ID_TX = 42 - the same
 * numbers the project specification gives. */
#define AUDIO_DMA_TRIG_RX           SERCOM4_DMAC_ID_RX
#define AUDIO_DMA_TRIG_TX           SERCOM4_DMAC_ID_TX

#define AUDIO_DMA_UNIT              0u    /* DMA0 */
#define AUDIO_DMA_CH_RX             0u
#define AUDIO_DMA_CH_TX             1u

/* One cell = one 32-bit DATA access = 4 characters on the wire. */
#define AUDIO_DMA_CELL_BYTES        4u
/* One block = one descriptor = exactly 32 audio frames. */
#define AUDIO_DMA_BLOCK_BYTES       AUDIO_BLOCK_BYTES   /* 1024 */

/*
 * Byte reorder (CHCTRLB.WBOEN + CHCTRLB.BYTORD).
 *
 * *** RESOLVED against PIC32CK Family Datasheet DS60001795H. ***
 *
 * The SERCOM "32-bit Extension" section states that with DATA32B set,
 * "Bytes are transmitted or received and stored in order from 0 to 3",
 * i.e. DATA[7:0] (byte 0) is the FIRST character on the wire. That is
 * Case A below, so the swizzle is REQUIRED and is now the adopted
 * setting, not a placeholder.
 *
 * The whole question was which byte of a 32-bit DATA access is the first
 * character on the wire. Both outcomes need the same single boolean,
 * applied identically to the RX and TX channels:
 *
 *  Logical sample in RAM: int32_t s = 0x12345600
 *  Little-endian memory : [+0]=0x00 [+1]=0x56 [+2]=0x34 [+3]=0x12
 *  Required wire order (DORD = MSB first): 12 34 56 00
 *
 *  Case A - DATA32B transmits DATA[7:0] first:
 *    TX raw   : DATA = 0x12345600 -> DATA[7:0]=0x00 out first -> 00 56 34 12  WRONG
 *    TX + swz : DATA = 0x00563412 -> DATA[7:0]=0x12 out first -> 12 34 56 00  OK
 *    RX       : wire 12 34 56 00 -> DATA reads 0x00563412; the write-side
 *               swizzle stores 0x12345600 in RAM                            OK
 *    => WBOEN = 1, BYTORD = WORD_SWIZZLE on both channels.
 *
 *  Case B - DATA32B transmits DATA[31:24] first:
 *    TX raw   : DATA = 0x12345600 -> 0x12 out first -> 12 34 56 00           OK
 *    => WBOEN = 0 (no swizzle) on both channels; a swizzle would break it.
 *
 * The datasheet says byte 0 first => Case A => swizzle ON. BYTORD = 01
 * (WORD_SWIZZLE) is the 3->0 / 2->1 / 1->2 / 0->3 reversal, named
 * DMA_BDCTRLB_BYTORD_BYTORD_WORD_SWIZZLE in the DFP and defined the same
 * way in the DMAC chapter. Case B is kept in the comment only as the
 * record of why the other value is wrong; there is no reason to select it.
 *
 * A scope check at first light is a sanity check, not an open question.
 */
#ifndef AUDIO_DMA_BYTE_SWIZZLE
#define AUDIO_DMA_BYTE_SWIZZLE      1u
#endif

/* ------------------------------------------------------------------ */
/* Isolated CMSIS-DSP IIR benchmark harness                            */
/* ------------------------------------------------------------------ */
/*
 * The DSP bench is measurement-only code and defaults to OFF. It is enabled
 * only by an explicit guarded -D profile; the two permanent MPLAB
 * configurations remain live DRC images and must not acquire benchmark-only
 * behaviour merely because they use -O3 and ARM_MATH_LOOPUNROLL.
 *
 * The isolated benchmark is deliberately NOT connected to the audio transport. It feeds
 * the kernel from its own synthetic buffers so that the cycle count belongs
 * to the kernel and to nothing else. That also means the number it produces
 * is a FLOOR, not a budget: the TDM-slot-to-channel gather, the scatter back
 * to the transmit buffer and any observation code are all outside it. Live
 * transport cost is evaluated separately with the complete audio image.
 */
#ifndef APP_ENABLE_DSP_BENCH
#define APP_ENABLE_DSP_BENCH        0
#endif

/*
 * Bench geometry.
 *
 * Channels and frames are tied to the transport geometry rather than
 * written out again, because the live transport feeds one from the other and a
 * silent disagreement between the two would be a real defect. The
 * _Static_asserts below hold that tie.
 *
 * Stages are independent: the cascade length is the thing being swept.
 */
#define DSP_BENCH_MAX_CHANNELS      AUDIO_SLOTS_PER_FRAME       /* 8  */
#define DSP_BENCH_MAX_FRAMES        AUDIO_FRAMES_PER_BLOCK      /* 32 */

/*
 * Cascade length ceiling.
 *
 * The maximum is 84 sections x 4 channels = 336 biquads. Short benches cannot
 * show what happens at that length: code and data locality,
 * the state working set and the per-call setup all start to matter, and this
 * device has a 4 KB combined cache.
 *
 * The ceiling is a promise about buffer sizes, and it is only kept if the
 * coefficient bank is at least this long as well. It is not: the Butterworth
 * bank is 6 sections. Any run longer than its bank would read past the end of
 * a const array - which is why dsp_iir_configure() now takes the bank's own
 * length and refuses to exceed it, and why the long sweep uses
 * dsp_coeffs_ap84[] (DSP_COEFFS_AP_STAGES == 84).
 */
#define DSP_BENCH_MAX_STAGES        84u

/* The long-cascade comparison point. */
#define DSP_BENCH_CHALLENGE_STAGES  84u

/*
 * Points the stage sweep visits. 84 individual measurements would say
 * nothing 10 of them do not, and the interesting shape is at the ends: the
 * fixed per-call cost shows in the small points, locality effects in the
 * large ones.
 */
#define DSP_BENCH_SWEEP_STAGE_POINTS \
    { 1u, 2u, 4u, 6u, 8u, 16u, 22u, 32u, 48u, 64u, 84u }

/*
 * Tier 2 - the DRC-equivalent DSP block.
 *
 * TDM8 is a wire format, not a DSP channel count. Both sides of the
 * comparison take two active input slots, expand them to four DSP channels
 * and run the cascade on those four. This makes the input and output work
 * explicit alongside the DF2T call.
 */
#define DSP_TIER2_IN_SLOTS          AUDIO_SLOTS_PER_FRAME       /* 8 */
#define DSP_TIER2_IN_CHANNELS       2u
#define DSP_TIER2_DSP_CHANNELS      4u
#define DSP_TIER2_OUT_SLOTS         4u

/*
 * Gains and the 24-bit mask model the codec conversion, channel expansion and
 * output conversion work performed per sample. The values exist so
 * that the multiplies are real work and cannot be folded away, and they are
 * the values used to compute the known-answer vectors.
 * Changing one here without regenerating the vectors breaks the KAT, which is
 * the intended coupling.
 */
#define DSP_TIER2_PRE_GAIN          0.875f
#define DSP_TIER2_POST_GAIN         0.9375f
#define DSP_TIER2_EXPAND_GAIN_L1    1.0f
#define DSP_TIER2_EXPAND_GAIN_R1    1.0f
#define DSP_TIER2_EXPAND_GAIN_L2    0.5f
#define DSP_TIER2_EXPAND_GAIN_R2    0.5f
#define DSP_TIER2_SLOT_MASK         0xFFFFFF00
#define DSP_TIER2_Q31_SCALE         (1.0f / 2147483648.0f)

/*
 * Mandatory re-check threshold, in thousandths of a cycle per sample per
 * section.
 *
 * The scalar CMSIS DF2T hot loop needs at least 4 vfma + 1 vmul + 1 load +
 * 1 store per sample per section - an apparent instruction lower bound, not a
 * cycle floor, since it excludes loop control, FPU dependency latency and
 * issue conditions. A measurement below 8.000 is therefore fast enough to be
 * suspicious, and the result should be repeated with its hot-loop disassembly
 * checked before it is published. The harness
 * prints that requirement itself rather than trusting anyone to remember it.
 */
#define DSP_BENCH_RECHECK_MILLI     8000u

/*
 * DWT auxiliary counter diagnostic. Diagnostic only: it reports FOLDCNT,
 * CPICNT, LSUCNT and EXCCNT around a kernel run so the gap between the static
 * instruction count and the measured cycle count can be attributed rather than
 * guessed at. It never produces a benchmark metric, and it does not touch the
 * timed region of dsp_bench_run(). It is available only when the explicit
 * isolated-benchmark profile enables that harness.
 */
#ifndef APP_ENABLE_DWT_DIAG
#define APP_ENABLE_DWT_DIAG         1
#endif

/*
 * Blocks per geometry in the DWT diagnostic. Small on purpose: the auxiliary
 * counters' widths are not documented in the Cortex-M33 TRM (it defers the
 * register descriptions to the Armv8-M Architecture Reference Manual, page 78),
 * so the run is kept short and each chunk is drained separately. The
 * max-per-chunk figures the diagnostic prints are what would reveal a counter
 * narrower than one block.
 */
#define DSP_BENCH_DWT_DIAG_CHUNKS   8u

/*
 * Frozen cycle prediction, in thousandths, for the build actually being
 * compiled. Both numbers are read out of this project's own compiled object
 * code by a static def/use analysis of the steady-state unrolled body. They
 * are measurements of the code, not estimates.
 *
 *   ISSUE_FLOOR = instructions issued per sample per section.
 *   DEP_OPS     = FP arithmetic ops per sample per section that lie on the
 *                 loop-carried dependency chain of the DF2T recurrence.
 *
 * The prediction is C = max(ISSUE_FLOOR, DEP_OPS * L), where L is the
 * dependent-result latency of VFMA.F32/VMUL.F32 in cycles. L is the one free
 * parameter: it is not quoted here because the Cortex-M33 instruction-timing
 * table has not been obtained from Arm, so the harness reports the L that the
 * measurement implies instead of pretending to know it in advance.
 *
 * ISSUE_FLOOR is a hard impossibility on a single-issue core: fewer cycles
 * than instructions cannot happen. That makes it a stronger statement than
 * DSP_BENCH_RECHECK_MILLI above, which is only a suspicion threshold.
 */
#if defined(ARM_MATH_LOOPUNROLL)
#define DSP_BENCH_ISSUE_FLOOR_MILLI 7250u   /* 116 instructions / 16 samples */
#define DSP_BENCH_DEP_OPS_MILLI     2500u   /* 40 chain links / 16 samples   */
#else
#define DSP_BENCH_ISSUE_FLOOR_MILLI 12000u  /* 12 instructions / 1 sample    */
#define DSP_BENCH_DEP_OPS_MILLI     4000u   /* 4 chain links / 1 sample      */
#endif

/* Default operating point, and the geometry the known-answer vectors were
 * generated for: 8 channels x 32 frames x 6 second-order sections. Six
 * sections is a 12th-order filter. */
#define DSP_BENCH_DEFAULT_CHANNELS  8u
#define DSP_BENCH_DEFAULT_FRAMES    32u
#define DSP_BENCH_DEFAULT_STAGES    6u

/* Repeats per reported measurement. The bench reports min and mean over
 * these; min is the uninterrupted cost, mean includes whatever else the
 * core was doing. Both are printed because they answer different
 * questions, and quoting one as if it were the other is how a budget ends
 * up wrong. */
#define DSP_BENCH_ITERATIONS        64u

/*
 * Optimisation level tag, set by the build configuration so the banner can
 * state what was actually built rather than what someone believed was
 * built. 0 = not stated.
 */
#ifndef DSP_BENCH_OPT_TAG
#define DSP_BENCH_OPT_TAG           0
#endif

#if (DSP_BENCH_OPT_TAG != 0) && !defined(__OPTIMIZE__)
#error "DSP_BENCH_OPT_TAG claims an optimised build but the compiler is at -O0"
#endif

_Static_assert(DSP_BENCH_DEFAULT_CHANNELS <= DSP_BENCH_MAX_CHANNELS,
               "default bench channel count must fit the static buffers");
_Static_assert(DSP_BENCH_DEFAULT_FRAMES <= DSP_BENCH_MAX_FRAMES,
               "default bench frame count must fit the static buffers");
_Static_assert(DSP_BENCH_DEFAULT_STAGES <= DSP_BENCH_MAX_STAGES,
               "default bench stage count must fit the static buffers");
_Static_assert(DSP_BENCH_MAX_CHANNELS == AUDIO_SLOTS_PER_FRAME,
                "bench channel count must track the TDM slot count");
_Static_assert(DSP_BENCH_MAX_FRAMES == AUDIO_FRAMES_PER_BLOCK,
                "bench block length must track the DMA block length");
_Static_assert(DSP_BENCH_CHALLENGE_STAGES <= DSP_BENCH_MAX_STAGES,
               "the challenge cascade length must fit the static buffers");
_Static_assert(DSP_TIER2_DSP_CHANNELS <= DSP_BENCH_MAX_CHANNELS,
               "Tier 2 DSP channels must fit the bench buffers");
_Static_assert(DSP_TIER2_IN_CHANNELS < DSP_TIER2_IN_SLOTS,
               "the active input channels must be a subset of the TDM frame");
_Static_assert(DSP_TIER2_OUT_SLOTS == DSP_TIER2_DSP_CHANNELS,
               "every DSP channel must have a transmit slot");

#endif /* APP_CONFIG_H */
