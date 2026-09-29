/*
 * main.c - PIC32CK audio application and the ordering of codec and transport
 *          startup.
 *
 * Boot order:
 *   1. board_init()      clocks, cache policy, PORT, LED
 *   2. console_init()    SERCOM5 USART 230400 8N1
 *   3. banner
 *   4. LED check
 *   5. optional WM8904 probe over SERCOM0 I2C
 *   6. integrated audio startup, codec-only, or transport-only operation
 *   7. idle loop: 1 Hz LED heartbeat + non-blocking console commands
 *
 * The app_config.h fallback gates are 0 for source-level portability. Named
 * MPLAB configurations select the intended hardware path explicitly; the
 * named CMSIS configuration is the 36-stage comparison image.
 */
#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "audio/audio_buffers.h"
#include "audio/audio_transport.h"
#include "board/board.h"
#include "drivers/console.h"
#include "drivers/i2c_host.h"
#include "drivers/wm8904.h"
#include "drivers/wm8904_def.h"
#include "drivers/wm8904_port_pic32ck.h"

enum { APP_WM8904_INSTANCE = 1u };

#if APP_ENABLE_DSP_BENCH
#include "dsp/dsp_bench.h"
#endif
#if APP_ENABLE_AUDIO_DRC
#include "dsp/sonora_drc_path.h"
#include "uart_app/app_console.h"
#include "uart_app/app_debug.h"
#include "uart_app/apps/sonora_app_console.h"
#include "uart_app/audio_transport_console.h"
#endif

/* The DFP ships its own pack version as compile-time macros. */
#include <component-version.h>

#ifndef __XC32_VERSION__
#define __XC32_VERSION__ 0
#endif

/* MPLAB X defines XPRJ_<conf> for every configuration. The name goes in the
 * banner so the serial log says which HEX is on the board; an image built from
 * the command line with MP_EXTRA_CC_PRE still reports the configuration its
 * makefile came from, which is why the gate lines below are printed as well. */
#if defined(XPRJ_PIC32CK2051SG01064_CMSIS_DRC)
#define APP_BUILD_CONF_NAME "PIC32CK2051SG01064_CMSIS_DRC"
#elif defined(XPRJ_PIC32CK2051SG01064_CMSIS_M33OPT_DRC)
#define APP_BUILD_CONF_NAME "PIC32CK2051SG01064_CMSIS_M33OPT_DRC"
#else
#define APP_BUILD_CONF_NAME "(unknown)"
#endif

static void print_banner(void)
{
    const uint32_t cpu = board_cpu_clock_hz();

    console_write("\n");
    console_writeln("=============================================");
    console_writeln(" PIC32CK Audio Bench");
    console_writeln("=============================================");
    console_printf(" device      : PIC32CK2051SG01064\n");
    console_printf(" board       : EV33V87A + AC164162 slot 1\n");
    console_printf(" build       : %s %s\n", __DATE__, __TIME__);
    console_printf(" MPLAB conf  : %s\n", APP_BUILD_CONF_NAME);
    console_printf(" WM8904 drv  : Sonora common driver + PIC32CK port\n");
    console_printf(" CPU clock   : %u Hz (%u.%03u MHz)\n",
                   cpu, cpu / 1000000u, (cpu / 1000u) % 1000u);
    console_printf(" clock chain : %s\n", board_clock_profile_name());
    console_printf(" SERCOM GCLK : %u Hz\n", board_sercom_core_clock_hz());
    console_printf(" DFP         : PIC32CK-SG %s.%u\n",
                   COMPONENT_VERSION_STRING, (uint32_t)BUILD_NUMBER);
    console_printf(" XC32        : %u (v%u.%02u)\n",
                   (uint32_t)__XC32_VERSION__,
                   (uint32_t)(__XC32_VERSION__ / 1000),
                   (uint32_t)((__XC32_VERSION__ / 10) % 100));
    console_printf(" D-cache     : off (DMA coherency)\n");
    console_printf(" I-cache     : %s\n", APP_ENABLE_ICACHE ? "on" : "off");
    console_writeln("---------------------------------------------");
    console_printf(" audio target: %u Hz, %u slots x %u bit, BCLK %u Hz\n",
                   (uint32_t)AUDIO_FS_HZ,
                   (uint32_t)AUDIO_SLOTS_PER_FRAME,
                   (uint32_t)AUDIO_SLOT_BITS,
                   (uint32_t)AUDIO_BCLK_HZ);
    console_printf(" DMA block   : %u frames = %u bytes\n",
                   (uint32_t)AUDIO_FRAMES_PER_BLOCK,
                   (uint32_t)AUDIO_BLOCK_BYTES);
    console_printf(" transport   : %s\n",
                   APP_ENABLE_AUDIO_TRANSPORT ? "ENABLED" : "compiled, not started");
    console_printf(" codec start : %s\n",
                   APP_ENABLE_WM8904_AUDIO_STARTUP ? "ENABLED" : "compiled, not started");
    console_printf(" DSP bench   : %s\n",
                    APP_ENABLE_DSP_BENCH ? "ENABLED (isolated kernel only)"
                                        : "not built");
#if APP_ENABLE_AUDIO_DRC
#if defined(DSP_USE_M33_DF2T_OPT) && (DSP_USE_M33_DF2T_OPT != 0)
    console_printf(" audio path  : Sonora DRC, 4 ch x %u DF2T stages (CMSIS API, M33 optimized kernel)\n",
                   (uint32_t)APP_AUDIO_DRC_STAGES);
#else
    console_printf(" audio path  : Sonora DRC, 4 ch x %u DF2T stages (Arm CMSIS-DSP)\n",
                   (uint32_t)APP_AUDIO_DRC_STAGES);
#endif
#endif
#if APP_AUDIO_LOOPTHROUGH
#if APP_ENABLE_AUDIO_DRC
    console_writeln(" TX contents : Sonora DRC RX->TX (auto-unmute after prime)");
#else
    console_writeln(" TX contents : firmware RX->TX loop-through (auto-unmute after prime)");
#endif
#elif APP_AUDIO_TX_TEST_PATTERN
    if (audio_buffers_tx_is_test_pattern())
    {
        console_printf(" TX contents : TEST PATTERN 0x%08x  <-- DEBUG BUILD, not silence\n",
                       audio_buffers_tx_test_word());
    }
    else
    {
        console_printf(" TX contents : silence (all zero)\n");
    }
#else
    console_printf(" TX contents : silence (all zero)\n");
#endif
    console_writeln("=============================================");
}

static void led_check(void)
{
    /* Three deliberate blinks: proves the CPU clock, the delay loop and
     * the LED pin all work, with nothing else attached. */
    console_write("LED check (PD05) ... ");
    for (uint32_t i = 0u; i < 3u; i++)
    {
        board_led_set(true);
        board_delay_ms(120u);
        board_led_set(false);
        board_delay_ms(120u);
    }
    console_writeln("done");
}

#if APP_ENABLE_WM8904_PROBE || APP_ENABLE_WM8904_AUDIO_STARTUP
static bool wm8904_check(void)
{
    const uint16_t id = wm8904_reg_read(APP_WM8904_INSTANCE,
                                        WM8904_SW_RESET_AND_ID);

    console_printf("WM8904 probe: addr 0x%02x, SCL target %u Hz\n",
                   (uint32_t)WM8904_I2C_ADDR, (uint32_t)APP_I2C_SCL_HZ);

    if (id == 0x8904u)
    {
        console_printf("WM8904 ID=0x%04x PASS\n", (uint32_t)id);
        return true;
    }
    console_printf("WM8904 probe failed: read 0x%04x, expected 0x8904\n",
                   (uint32_t)id);
    return false;
}
#endif

#if APP_ENABLE_WM8904_AUDIO_STARTUP
static void wm8904_print_intended_config(void)
{
    /* What the driver is ABOUT to program. Every number is a compile-time
     * constant - none of it is measured, so it must not be read as evidence
     * that the codec is doing any of this. */
    console_writeln("WM8904 config:");
    console_printf("  role=ADC+DAC\n");
    console_printf("  fs=%u\n",       (uint32_t)AUDIO_FS_HZ);
    console_printf("  frame=TDM8\n");
    console_printf("  slot=%u\n",     (uint32_t)AUDIO_SLOT_BITS);
    console_printf("  MCLK=%u\n",     (uint32_t)WM8904_MCLK_HZ);
    console_printf("  SYSCLK=%u\n",   (uint32_t)WM8904_SYSCLK_HZ);
    console_printf("  BCLK=%u\n",     (uint32_t)WM8904_BCLK_HZ);
    console_printf("  FS=%u\n",       (uint32_t)AUDIO_FS_HZ);
    console_printf("  FLL=off\n");
#if RESOLVED_TRANSPORT_DATA_DELAY_BITS == 1u
    console_printf("  format=DSP mode A (1-bit data delay)\n");
#else
    console_printf("  format=DSP mode B (no data delay)\n");
#endif
}

#if !APP_ENABLE_AUDIO_TRANSPORT
/* Codec-only startup. When transport is also enabled, the ordered integrated
 * startup sequence below replaces this. */
static void wm8904_audio_startup(void)
{
    wm8904_print_intended_config();

    if (!wm8904_set_rate_hz(APP_WM8904_INSTANCE, AUDIO_FS_HZ) ||
        !wm8904_init(APP_WM8904_INSTANCE, true))
    {
        console_writeln("WM8904 init=FAILED (see first wm8904 E-line)");
        return;
    }
    console_writeln("WM8904 init=PASS (BCLK/FS now driven by the codec)");
}
#endif /* !APP_ENABLE_AUDIO_TRANSPORT */
#endif /* APP_ENABLE_WM8904_AUDIO_STARTUP */

/* `*ts` is a terminal pre-flash stop. Any subsequent transport start makes
 * its previous confirmation stale. */
#if APP_ENABLE_AUDIO_DRC
static bool preflash_stop_latched;
static bool preflash_stop_confirmed;
#endif

#if APP_ENABLE_AUDIO_TRANSPORT
static const char *sync_name(audio_sync_state_t s)
{
    switch (s)
    {
    case AUDIO_SYNC_LOCKED:     return "LOCKED";
    case AUDIO_SYNC_MISALIGNED: return "MISALIGNED";
    case AUDIO_SYNC_STALLED:    return "STALLED";
    default:                    return "idle";
    }
}
#endif

/* ------------------------------------------------------------------ */
/* Ordered startup of two independent devices                           */
/* ------------------------------------------------------------------ */
/*
 * The codec is brought up the normal way - configured AND driving BCLK/FS -
 * and the transport is started afterwards, into clocks that are already
 * running. Getting the first frame right is the transport's job (it enables
 * the SERCOM last and refuses a misaligned start); the codec is not held
 * quiet for it.
 *
 * The common driver's wm8904_init(inst, true) configures the codec and enables
 * its master clock outputs in one hardware-proven Sonora sequence.
 *
 * Neither driver calls the other. audio_transport.* contains no WM8904
 * symbol, and wm8904.* contains no SERCOM4 or DMA symbol.
 */
#if APP_ENABLE_AUDIO_TRANSPORT && APP_ENABLE_WM8904_AUDIO_STARTUP

static bool codec_start_master(void)
{
    console_write("  codec configure + BCLK/FS out ........ ");
    if (!wm8904_set_rate_hz(APP_WM8904_INSTANCE, AUDIO_FS_HZ) ||
        !wm8904_init(APP_WM8904_INSTANCE, true))
    {
        console_writeln("FAILED");
        return false;
    }
    console_writeln("PASS (BCLK/FS running)");
    return true;
}

/* The codec-master FS pulse is one BCLK high. Enable the framed client just
 * after that pulse ends, leaving almost a complete 48 kHz frame for TX DMA to
 * prime the FIFO before the next active edge. A fixed delay cannot guarantee
 * this phase. PC00 remains readable because its SERCOM PMUX has INEN set. */
static bool wait_for_audio_frame_gap(void)
{
    uint32_t spins = APP_CPU_CLOCK_HZ / 100u;

    while (!board_pin_read(BOARD_AUDIO_FS_PORT, BOARD_AUDIO_FS_PIN) &&
           (spins != 0u))
    {
        spins--;
    }
    if (spins == 0u)
    {
        return false;
    }

    spins = APP_CPU_CLOCK_HZ / 100u;
    while (board_pin_read(BOARD_AUDIO_FS_PORT, BOARD_AUDIO_FS_PIN) &&
           (spins != 0u))
    {
        spins--;
    }
    return spins != 0u;
}

/* Transport only. The codec clock is not touched and may already be running. */
static bool transport_start_checked(void)
{
#if APP_ENABLE_AUDIO_DRC
    static bool drc_ready;

    if (!drc_ready)
    {
        console_printf("  Sonora DRC path init (4 ch x %u) ...... ",
                       (unsigned)APP_AUDIO_DRC_STAGES);
        if (!sonora_drc_path_init())
        {
            console_writeln("FAILED");
            return false;
        }
        drc_ready = true;
        console_writeln("PASS");
    }
#endif

    console_write("  transport init + start (SERCOM last) . ");
    (void)audio_transport_init();

    if (!wait_for_audio_frame_gap())
    {
        console_writeln("REFUSED (FSYNC not observed)");
        return false;
    }

    if (!audio_transport_start())
    {
        const audio_stats_t *const c = audio_transport_stats();
        console_printf("REFUSED (start: LENERR %u TUR %u BUFOVF %u RXstall %u TXstall %u)\n",
                       c->startup_lenerr, c->startup_tur, c->startup_bufovf,
                       c->startup_rx_stall, c->startup_tx_stall);
        return false;
    }

    audio_transport_state_t st;
    audio_transport_get_state(&st);
    console_printf("PASS (%s, first half RX %d TX %d)\n",
                   sync_name(st.sync), (int)st.rx_first_half, (int)st.tx_first_half);
#if APP_ENABLE_AUDIO_DRC
    console_printf("  live timing (DWT CYCCNT) ............. %s\n",
                   sonora_drc_path_timing_init() ? "PASS" : "UNAVAILABLE");
    preflash_stop_latched = false;
    preflash_stop_confirmed = false;
#endif
    return true;
}

/* Boot: codec first (clocks running), then the transport - Test B's order. */
static bool audio_first_light_start(void)
{
    console_writeln("Audio startup (clock first):");
    if (!codec_start_master())
    {
        return false;
    }
    board_delay_ms(10u);    /* clocks demonstrably running before the start */

    /* A clean TUR/LENERR snapshot is necessary but not sufficient: a cold
     * start can leave TX enabled yet without a completion.  The transport
     * rejects that condition after its progress proof.  Retry against the
     * still-running codec clock with deliberately non-frame-period offsets,
     * so a caller never has to discover that a manual p/o fixes first light. */
    for (uint32_t attempt = 0u; attempt < 3u; attempt++)
    {
        if (transport_start_checked())
        {
            return true;
        }
        if (attempt < 2u)
        {
            console_printf("  transport retry %u/3 after refused start\n", attempt + 2u);
            board_delay_us(317u + (attempt * 131u));
        }
    }
    return false;
}

static void audio_transport_only_stop(void)
{
    console_writeln("stopping transport (codec clock keeps running)");
    audio_transport_stop();
}

#if APP_AUDIO_LOOPTHROUGH
static bool loopthrough_hpo_unmuted;
static bool loopthrough_unmute_pending;

static const char *loopthrough_state_name(audio_loopthrough_state_t state)
{
    switch (state)
    {
    case AUDIO_LOOPTHROUGH_PRIMING:  return "PRIMING";
    case AUDIO_LOOPTHROUGH_RUNNING:  return "RUNNING";
    case AUDIO_LOOPTHROUGH_DRAINING: return "DRAINING";
    case AUDIO_LOOPTHROUGH_FAULT:    return "FAULT";
    default:                         return "OFF";
    }
}

static bool loopthrough_set_analog_mute(bool mute)
{
    if (!wm8904_set_analog_output_mute_verified(APP_WM8904_INSTANCE, mute))
    {
        console_printf("HPOUT analogue %s FAILED (readback)\n",
                       mute ? "mute" : "unmute");
        return false;
    }

    loopthrough_hpo_unmuted = !mute;
    return true;
}

static bool loopthrough_transport_healthy(void)
{
    const audio_stats_t *const c = audio_transport_stats();

    return (c->sercom_tur_count == 0u) &&
           (c->sercom_bufovf_count == 0u) &&
           (c->sercom_lenerr_count == 0u) &&
           (c->dma_rx_rde_count == 0u) &&
           (c->dma_rx_wre_count == 0u) &&
           (c->dma_tx_rde_count == 0u) &&
           (c->dma_tx_wre_count == 0u) &&
           (c->dma_rx_ta_count == 0u) &&
           (c->dma_tx_ta_count == 0u);
}

static void loopthrough_request_start(void)
{
    audio_transport_state_t     transport;
    audio_loopthrough_status_t  loop;

    audio_transport_get_state(&transport);
    audio_transport_loopthrough_get_status(&loop);

    if (!transport.running || (transport.sync != AUDIO_SYNC_LOCKED))
    {
        console_writeln("loop-through refused: transport is not LOCKED");
        return;
    }
    if (!loopthrough_transport_healthy())
    {
        console_writeln("loop-through refused: transport has an error counter");
        return;
    }
    if ((loop.state != AUDIO_LOOPTHROUGH_OFF) || loopthrough_unmute_pending ||
        loopthrough_hpo_unmuted)
    {
        console_printf("loop-through already %s\n", loopthrough_state_name(loop.state));
        return;
    }

    /* The codec starts with analogue HPOUT mute asserted.  Reassert it before
     * every trial so a restart can never reveal stale TX buffer contents. */
    if (!loopthrough_set_analog_mute(true))
    {
        return;
    }
    if (!audio_transport_loopthrough_start())
    {
        console_writeln("loop-through refused by transport");
        return;
    }

    loopthrough_unmute_pending = true;
    console_writeln("loop-through PRIMING: waiting for two ISR RX->TX pairs (HPOUT muted)");
}

static void loopthrough_request_stop(void)
{
    loopthrough_unmute_pending = false;
    (void)loopthrough_set_analog_mute(true);
    audio_transport_loopthrough_stop();
    console_writeln("loop-through DRAINING: HPOUT muted; two TX halves will be zeroed");
}

/* Runs only in the low-rate foreground.  It never touches audio buffers: its
 * single job is the I2C-verified analogue mute gate after the ISR has primed
 * TX with valid data, or after the ISR has declared a fault. */
static void loopthrough_service(void)
{
    audio_loopthrough_status_t loop;

    audio_transport_loopthrough_get_status(&loop);

    if (loopthrough_unmute_pending)
    {
        if (loop.state == AUDIO_LOOPTHROUGH_RUNNING)
        {
            loopthrough_unmute_pending = false;
            console_write("loop-through RUNNING: HPOUT analogue unmute ... ");
            if (loopthrough_set_analog_mute(false))
            {
                console_writeln("PASS");
            }
            else
            {
                (void)loopthrough_set_analog_mute(true);
                audio_transport_loopthrough_stop();
            }
        }
        else if (loop.state == AUDIO_LOOPTHROUGH_FAULT)
        {
            loopthrough_unmute_pending = false;
            console_writeln("loop-through FAULT while priming: HPOUT remains muted");
            (void)loopthrough_set_analog_mute(true);
            audio_transport_loopthrough_stop();
        }
    }

    if (loopthrough_hpo_unmuted && (loop.state == AUDIO_LOOPTHROUGH_FAULT))
    {
        /* The ISR has already begun zero-filling safe TX halves.  This is the
         * foreground hardware safety gate: assert both analogue mute bits and
         * then ask the ISR to finish the two-half silence drain. */
        console_writeln("loop-through FAULT: muting HPOUT and draining TX");
        (void)loopthrough_set_analog_mute(true);
        audio_transport_loopthrough_stop();
    }
}
#endif /* APP_AUDIO_LOOPTHROUGH */

#endif /* transport && codec startup */

#if APP_ENABLE_AUDIO_DRC
bool pic32ck_console_restart_audio(void)
{
#if APP_ENABLE_AUDIO_TRANSPORT && APP_ENABLE_WM8904_AUDIO_STARTUP
    preflash_stop_latched = false;
    preflash_stop_confirmed = false;
#if APP_AUDIO_LOOPTHROUGH
    loopthrough_request_stop();
    board_delay_ms(2u);
#endif
    audio_transport_only_stop();
    if (!transport_start_checked())
    {
        return false;
    }
#if APP_AUDIO_LOOPTHROUGH
    loopthrough_request_start();
#endif
    return true;
#else
    return false;
#endif
}

bool pic32ck_console_stop_audio_for_flash(void)
{
#if APP_ENABLE_AUDIO_TRANSPORT && APP_ENABLE_WM8904_AUDIO_STARTUP
    bool mute_ok;

    preflash_stop_latched = true;
#if APP_AUDIO_LOOPTHROUGH
    loopthrough_unmute_pending = false;
    mute_ok = loopthrough_set_analog_mute(true);
    audio_transport_loopthrough_stop();
    board_delay_ms(2u);
#else
    mute_ok = wm8904_set_analog_output_mute_verified(APP_WM8904_INSTANCE, true);
#endif

    audio_transport_stop();

    audio_transport_state_t state;
    audio_transport_get_state(&state);
    const bool stopped = !state.running && !state.rx_dma_enabled &&
                         !state.tx_dma_enabled;
    preflash_stop_confirmed = mute_ok && stopped;

    if (preflash_stop_confirmed)
    {
        /* This phrase is the common flash-tool gate. Keep it byte-identical
         * to Sonora. */
        console_writeln(" audio transport: stopped by *ts (analog mute verified, TDM/DMA halted)");
    }
    else
    {
        console_printf(" audio transport: *ts stopped TDM/DMA=%u, but analog mute NOT verified; "
                       "do NOT flash or reset this board yet\n",
                       stopped ? 1u : 0u);
    }
    return preflash_stop_confirmed;
#else
    preflash_stop_latched = true;
    preflash_stop_confirmed = false;
    console_writeln(" audio transport: *ts unavailable; do NOT flash or reset this board yet");
    return false;
#endif
}

void pic32ck_console_report_audio_stop_for_flash(void)
{
    if (preflash_stop_confirmed)
    {
        console_writeln(" ?ts: stopped by *ts (analog mute verified, TDM/DMA halted)");
    }
    else if (preflash_stop_latched)
    {
        console_writeln(" ?ts: *ts did NOT establish verified analog mute; do NOT flash or reset");
    }
    else
    {
        console_writeln(" ?ts: no pre-flash stop has been confirmed since boot or restart");
    }
}
#endif /* APP_ENABLE_AUDIO_DRC */

/* ------------------------------------------------------------------ */
/* Console diagnostics - explicit request only, never a boot dump      */
/* ------------------------------------------------------------------ */

#if APP_ENABLE_AUDIO_TRANSPORT
static uint32_t telemetry_period_ms  = APP_DBG_PERIOD_MS;
static uint32_t telemetry_elapsed_ms = APP_DBG_PERIOD_MS;

#if APP_ENABLE_AUDIO_DRC
static uint32_t cycles_to_us_x10(uint32_t cycles)
{
    return (uint32_t)((((uint64_t)cycles * 10000000ull) +
                       ((uint64_t)APP_CPU_CLOCK_HZ / 2ull)) /
                      (uint64_t)APP_CPU_CLOCK_HZ);
}

static uint32_t load_pct_x10(uint32_t used_cycles, uint32_t window_cycles)
{
    return (window_cycles != 0u)
               ? (uint32_t)(((uint64_t)used_cycles * 1000ull) / window_cycles)
               : 0u;
}
#endif

void pic32ck_console_set_telemetry_period_ms(uint32_t period_ms)
{
    telemetry_period_ms = period_ms;
    /* Sonora reports immediately when telemetry is enabled after having been
     * off.  Prime the elapsed value to retain that useful console behaviour. */
    telemetry_elapsed_ms = period_ms;
}

static void print_periodic_telemetry(void)
{
    audio_transport_state_t st;
    const audio_stats_t *const counters = audio_transport_stats();

#if APP_ENABLE_AUDIO_DRC
    sonora_drc_path_stats_t drc;
    sonora_drc_telemetry_t telemetry;
    sonora_drc_path_get_stats(&drc);
    sonora_drc_path_take_telemetry(&telemetry);
#endif

    audio_transport_get_state(&st);
    const bool active = st.running && (st.sync == AUDIO_SYNC_LOCKED) &&
                        st.rx_dma_enabled && st.tx_dma_enabled;

    /* Sonora prints STREAM only on a state transition (and at the first
     * report), not every period.  Keep its field order and spelling exactly. */
    {
        static bool memo_valid;
        static uint32_t memo_epoch;
        static bool memo_active;
        static audio_sync_state_t memo_sync;
        const uint32_t epoch = counters->sync_attempts;
        const bool failed = (st.sync == AUDIO_SYNC_MISALIGNED) ||
                            (st.sync == AUDIO_SYNC_STALLED);

        if (!memo_valid || (memo_epoch != epoch) ||
            (memo_active != active) || (memo_sync != st.sync))
        {
            const char *const transition = (epoch == 0u) ? "none" :
                                           ((epoch == 1u) ? "initial-start" :
                                                           "manual-restart");
            console_printf("STREAM epoch=%u qualified=%u transition=%s"
                           " safe_mute=%u failed=%u error=%s mute_held=%u\n",
                           epoch, active ? 1u : 0u, transition,
                           failed ? 1u : 0u, failed ? 1u : 0u,
                           failed ? "start-failed" : "none",
                           failed ? 1u : 0u);
            memo_valid = true;
            memo_epoch = epoch;
            memo_active = active;
            memo_sync = st.sync;
        }
    }

#if APP_ENABLE_AUDIO_DRC
    {
        const uint32_t resp_us10 = cycles_to_us_x10(telemetry.callback_peak_cycles);
        const uint32_t deadline_us10 = cycles_to_us_x10(APP_AUDIO_BLOCK_CYCLES);
        const int32_t margin_us10 = (int32_t)deadline_us10 - (int32_t)resp_us10;

        /* Sonora-compatible TDMx: x is the physical SPI peripheral number.
         * This target uses SERCOM4 in SPI mode. */
        console_printf("TDM4:resp=%u.%uus margin=",
                       resp_us10 / 10u, resp_us10 % 10u);
        if (margin_us10 < 0)
        {
            const uint32_t magnitude = (uint32_t)(-margin_us10);
            console_printf("-%u.%uus", magnitude / 10u, magnitude % 10u);
        }
        else
        {
            console_printf("%u.%uus", (uint32_t)margin_us10 / 10u,
                           (uint32_t)margin_us10 % 10u);
        }
        console_printf(" (run,act,blk,miss)=(%u,%u,%u,%u)\n",
                       st.running ? 1u : 0u, active ? 1u : 0u,
                       st.rx_blocks, telemetry.callback_deadline_misses);
    }

    if (telemetry.load_windows == 0u)
    {
        console_writeln("DSPload:win=10.000ms n=0 (report interval shorter than the window)");
    }
    else
    {
        static bool window_announced;
        const uint32_t mean_x10 = load_pct_x10(telemetry.load_mean_cycles,
                                               telemetry.load_window_cycles);
        const uint32_t max_x10 = load_pct_x10(telemetry.load_max_cycles,
                                              telemetry.load_window_cycles);
        if (!window_announced)
        {
            window_announced = true;
            console_writeln("DSPloadcfg:win=10.000ms (self = A+B; demand = self+stolen; stolen/n printed only when they matter)");
        }
        console_printf("DSPload:A=%u.%u%% B=0.0%% max=%u.%u%% bad=0/0/0/0\n",
                       mean_x10 / 10u, mean_x10 % 10u,
                       max_x10 / 10u, max_x10 % 10u);
    }

    {
        const uint32_t iir_us10 = cycles_to_us_x10(drc.iir_cycles_last);
        console_printf("DSP [4ch][stage=%u total=%u]CMSIS-IIR:%u.%uus\n",
                       (uint32_t)APP_AUDIO_DRC_STAGES,
                       4u * (uint32_t)APP_AUDIO_DRC_STAGES,
                       iir_us10 / 10u, iir_us10 % 10u);
    }
#endif
    console_write("\n");
}

static void telemetry_tick(uint32_t elapsed_ms)
{
    if (telemetry_period_ms == 0u)
    {
        return;
    }

    if (telemetry_elapsed_ms >= telemetry_period_ms)
    {
        telemetry_elapsed_ms = 0u;
        print_periodic_telemetry();
        return;
    }

    telemetry_elapsed_ms += elapsed_ms;
}

#endif /* APP_ENABLE_AUDIO_TRANSPORT */

static void print_help(void)
{
    console_writeln("keys: s=status  x=RX peek  v=RX 32-frame capture  d=codec regs  h=help");
#if APP_ENABLE_AUDIO_TRANSPORT && APP_ENABLE_WM8904_AUDIO_STARTUP
    console_writeln("      p=stop transport  o=start transport (codec clock untouched)");
#endif
#if APP_ENABLE_AUDIO_TRANSPORT
    console_writeln("      structured: ?gv  ?gh  *tr  *ts/?ts  *tq0000/0001/0002XXXX");
#endif
#if APP_AUDIO_LOOPTHROUGH
    console_writeln("      loop-through auto-starts and unmutes after prime; l=re-start it");
    console_writeln("      m=mute HPOUT + drain loop-through TX to silence");
#endif
#if APP_ENABLE_AUDIO_DRC
    console_writeln("      *cf00=IIR normal  *cf01=IIR bypass");
#endif
#if APP_ENABLE_DSP_BENCH
    console_writeln("      t=DSP known-answer test   b=bench   i=build info");
    console_writeln("      f=M33 FPU latency/throughput probe");
    console_writeln("      w=stage sweep             n=channel sweep");
    console_writeln("      c=84-section challenge (4 ch, N_max)");
    console_writeln("      2=Tier 2 block (6 sections)  3=Tier 2 block (84)");
#endif
}

#if APP_ENABLE_AUDIO_TRANSPORT
static void print_dma_ta_snapshot(const char *name, const audio_dma_ta_snapshot_t *d)
{
    if (!d->valid)
    {
        return;
    }

    console_printf("    %s TA: CTRLA %08x CTRLB %08x EVCTRL %08x INTEN %08x INTF %08x\n",
                   name, d->chctrla, d->chctrlb, d->chevctrl, d->chinten, d->chintf);
    console_printf("      SSA %08x DSA %08x NXT %08x XSIZ %08x LLCFG %08x\n",
                   d->chssa, d->chdsa, d->chnxt, d->chxsiz, d->chllcfgstat);
    console_printf("      STATBC %08x STATCC %08x STAT %08x\n",
                   d->chstatbc, d->chstatcc, d->chstat);
}

static void print_transport_status(void)
{
    audio_transport_state_t     st;
    const audio_stats_t *const  c = audio_transport_stats();
    audio_dma_diagnostics_t     diag;

    audio_transport_get_state(&st);
    audio_transport_get_dma_diagnostics(&diag);

    console_printf("audio running=%s  sync=%s  first half RX %d TX %d\n",
                   st.running ? "yes" : "no", sync_name(st.sync),
                   (int)st.rx_first_half, (int)st.tx_first_half);
    console_printf("  start-up: attempts %u  refused %u  LENERR %u  TUR %u  BUFOVF %u  RXstall %u  TXstall %u\n",
                   c->sync_attempts, c->sync_fail,
                   c->startup_lenerr, c->startup_tur, c->startup_bufovf,
                   c->startup_rx_stall, c->startup_tx_stall);
    console_printf("  RX blocks       = %u\n", st.rx_blocks);
    console_printf("  TX blocks       = %u\n", st.tx_blocks);
    console_printf("  RX ping/pong    = filling %u, last done %u\n",
                   st.rx_filling_half, st.rx_last_done_half);
    console_printf("  TX ping/pong    = reading %u, last done %u\n",
                   st.tx_reading_half, st.tx_last_done_half);
    console_printf("  RX block pending= %s\n", st.rx_block_pending ? "yes" : "no");
    console_writeln("  SERCOM:");
    console_printf("    TUR    = %u\n", c->sercom_tur_count);
    console_printf("    BUFOVF = %u\n", c->sercom_bufovf_count);
    console_printf("    LENERR = %u\n", c->sercom_lenerr_count);
    console_printf("  DMA RX: channel %s\n", st.rx_dma_enabled ? "enabled" : "DISABLED");
    console_printf("    RDE    = %u\n", c->dma_rx_rde_count);
    console_printf("    WRE    = %u\n", c->dma_rx_wre_count);
    console_printf("    TA     = %u\n", c->dma_rx_ta_count);
    console_printf("  DMA TX: channel %s\n", st.tx_dma_enabled ? "enabled" : "DISABLED");
    console_printf("    RDE    = %u\n", c->dma_tx_rde_count);
    console_printf("    WRE    = %u\n", c->dma_tx_wre_count);
    console_printf("    TA     = %u\n", c->dma_tx_ta_count);
    if (diag.rx.valid || diag.tx.valid)
    {
        console_printf("  DMA TA snapshot: INTSTAT3 %08x INTSTAT2 %08x INTSTAT1 %08x\n",
                       diag.dma_intstat3, diag.dma_intstat2, diag.dma_intstat1);
        console_printf("    SERCOM: STATUS %04x INTFLAG %02x FIFOSPACE %04x\n",
                       (uint32_t)diag.sercom_status, (uint32_t)diag.sercom_intflag,
                       (uint32_t)diag.sercom_fifospace);
        print_dma_ta_snapshot("RX", &diag.rx);
        print_dma_ta_snapshot("TX", &diag.tx);
    }
    console_printf("  expected steady rate = %u blocks/s per direction\n",
                   (uint32_t)(AUDIO_FS_HZ / AUDIO_FRAMES_PER_BLOCK));
#if APP_AUDIO_LOOPTHROUGH
    {
        audio_loopthrough_status_t loop;

        audio_transport_loopthrough_get_status(&loop);
        console_printf("  loop-through: %s  copied %u  silenced %u  pair faults %u\n",
                       loopthrough_state_name(loop.state), loop.copied_blocks,
                       loop.silenced_blocks, loop.pair_faults);
        console_printf("  HPOUT analogue: %s%s\n",
                       loopthrough_hpo_unmuted ? "UNMUTED" : "muted",
                       loopthrough_unmute_pending ? " (unmute pending)" : "");
    }
#endif
#if APP_ENABLE_AUDIO_DRC
    {
        sonora_drc_path_stats_t drc;
        sonora_drc_path_get_stats(&drc);
        console_printf("  Sonora DRC: 4 ch x %u stages  blocks %u  deadline misses %u\n",
                       (uint32_t)APP_AUDIO_DRC_STAGES, drc.processed_blocks,
                       drc.deadline_misses);
        if (drc.timing_available && (drc.processed_blocks != 0u))
        {
            console_printf("    full path cycles: last %u  min %u  max %u / %u available\n",
                           drc.cycles_last, drc.cycles_min, drc.cycles_max,
                           (uint32_t)APP_AUDIO_BLOCK_CYCLES);
        }
        else
        {
            console_writeln("    full path cycles: unavailable (DWT required)");
        }
    }
#endif
}

static void peek_rx_block(void)
{
    int32_t        w[8];
    const uint32_t n = audio_transport_peek_rx(w, 8u);

    if (n == 0u)
    {
        console_writeln("RX peek: no completed block yet");
        return;
    }

    console_printf("RX peek: first %u words (frame 0, slots 0..%u)\n", n, n - 1u);
    for (uint32_t i = 0u; i < n; i++)
    {
        console_printf("  [%u] = 0x%08x\n", i, (uint32_t)w[i]);
    }
}

/*
 * Explicit RX-wire diagnostic. This copies one completed ping/pong half before
 * printing anything, so the UART output cannot make the 32-frame observation
 * span several DMA blocks. Slot 0/1 are then shown sample-by-sample; the
 * min/max span for every slot makes the codec's active TDM slots evident.
 */
static void capture_rx_block(void)
{
    static int32_t capture[AUDIO_WORDS_PER_BLOCK];
    int32_t lo[AUDIO_SLOTS_PER_FRAME];
    int32_t hi[AUDIO_SLOTS_PER_FRAME];
    const uint32_t n = audio_transport_peek_rx(capture, AUDIO_WORDS_PER_BLOCK);

    if (n != AUDIO_WORDS_PER_BLOCK)
    {
        console_writeln("RX capture: no complete 32-frame block yet");
        return;
    }

    for (uint32_t slot = 0u; slot < AUDIO_SLOTS_PER_FRAME; slot++)
    {
        lo[slot] = capture[slot];
        hi[slot] = capture[slot];
    }

    for (uint32_t frame = 1u; frame < AUDIO_FRAMES_PER_BLOCK; frame++)
    {
        for (uint32_t slot = 0u; slot < AUDIO_SLOTS_PER_FRAME; slot++)
        {
            const int32_t sample = capture[frame * AUDIO_SLOTS_PER_FRAME + slot];
            if (sample < lo[slot]) { lo[slot] = sample; }
            if (sample > hi[slot]) { hi[slot] = sample; }
        }
    }

    console_writeln("RX capture: one completed block (32 frames x 8 slots)");
    for (uint32_t slot = 0u; slot < AUDIO_SLOTS_PER_FRAME; slot++)
    {
        const uint32_t span = (uint32_t)hi[slot] - (uint32_t)lo[slot];
        console_printf("  slot %u: min 0x%08x  max 0x%08x  span 0x%08x\n",
                       slot, (uint32_t)lo[slot], (uint32_t)hi[slot], span);
    }

    console_writeln("  frame  slot0       slot1");
    for (uint32_t frame = 0u; frame < AUDIO_FRAMES_PER_BLOCK; frame++)
    {
        const uint32_t base = frame * AUDIO_SLOTS_PER_FRAME;
        console_printf("  %02u     0x%08x  0x%08x\n", frame,
                       (uint32_t)capture[base], (uint32_t)capture[base + 1u]);
    }
}
#endif /* APP_ENABLE_AUDIO_TRANSPORT */

/* PIC32CK application hotkeys behind Sonora's app-blind UART contract.
 * Command framing, CR/LF handling, and CSV ownership stay in app_debug.c. */
#if APP_ENABLE_AUDIO_DRC
sonora_hotkey_result_t sonora_app_handle_hotkey(char c)
{
    switch (c)
    {
    case 'h':
    case '?':
        print_help();
        break;

#if APP_ENABLE_AUDIO_TRANSPORT
    case 's':
        print_transport_status();
        break;

    case 'x':
        peek_rx_block();
        break;

    case 'v':
        capture_rx_block();
        break;
#endif

#if APP_ENABLE_WM8904_PROBE || APP_ENABLE_WM8904_AUDIO_STARTUP
    case 'd':
        wm8904_dump_reg(APP_WM8904_INSTANCE);
        break;
#endif

#if APP_ENABLE_AUDIO_TRANSPORT && APP_ENABLE_WM8904_AUDIO_STARTUP
    case 'p':
#if APP_AUDIO_LOOPTHROUGH
        /* A deliberate stop is always muted first.  Give the ISR at least two
         * block periods to replace both TX halves with zero while clocks still
         * run; if the stream has already faulted this delay is harmless. */
        loopthrough_request_stop();
        board_delay_ms(2u);
#endif
        audio_transport_only_stop();
        break;

    case 'o':
        /* Test C: restart into a codec clock that never stopped. */
#if APP_AUDIO_LOOPTHROUGH
        loopthrough_request_stop();
#endif
        (void)transport_start_checked();
        break;

#if APP_AUDIO_LOOPTHROUGH
    case 'l':
        loopthrough_request_start();
        break;

    case 'm':
        loopthrough_request_stop();
        break;
#endif

#endif

#if APP_ENABLE_DSP_BENCH
    case 'i':
        dsp_bench_print_build_info();
        break;

    case 't':
        (void)dsp_bench_self_test();
        break;

    case 'b':
    {
        dsp_bench_result_t r;

        if (dsp_bench_run(DSP_BENCH_DEFAULT_CHANNELS,
                          DSP_BENCH_DEFAULT_FRAMES,
                          DSP_BENCH_DEFAULT_STAGES,
                          DSP_BENCH_ITERATIONS, &r))
        {
            dsp_bench_print_result(&r);
        }
        else
        {
            console_writeln("bench: refused - geometry or counter unavailable");
        }
        break;
    }

    case 'f':
        dsp_bench_run_fpu_probe();
        break;

    case 'w':
        dsp_bench_sweep_stages();
        break;

    case 'n':
        dsp_bench_sweep_channels();
        break;

    case 'c':
        dsp_bench_run_challenge();
        break;

    case '2':
        dsp_bench_run_tier2(DSP_BENCH_DEFAULT_STAGES);
        break;

    case '3':
        dsp_bench_run_tier2(DSP_BENCH_CHALLENGE_STAGES);
        break;

#if APP_ENABLE_DWT_DIAG
    case 'y':
        dsp_bench_run_dwt_diag();
        break;
#endif
#endif

    default:
        return SONORA_HOTKEY_IGNORED;
    }

    return SONORA_HOTKEY_HANDLED;
}
#endif

int main(void)
{
    board_init();
    console_init();
#if APP_ENABLE_AUDIO_DRC
    app_console_init();
#endif

    print_banner();
    led_check();

#if APP_ENABLE_WM8904_PROBE || APP_ENABLE_WM8904_AUDIO_STARTUP
    i2c_host_init();
    const bool codec_present = wm8904_check();
#else
    console_writeln("WM8904 probe: disabled (APP_ENABLE_WM8904_PROBE = 0)");
#endif

#if APP_ENABLE_AUDIO_TRANSPORT && APP_ENABLE_WM8904_AUDIO_STARTUP
    /* Start the codec as clock master, then start the
     * transport, which syncs itself to them. */
    if (codec_present)
    {
        wm8904_print_intended_config();
        if (audio_first_light_start())
        {
#if APP_AUDIO_LOOPTHROUGH
            console_writeln("loop-through auto-start: prime then unmute HPOUT");
            loopthrough_request_start();
#endif
        }
    }
    else
    {
        console_writeln("Audio startup skipped: codec did not answer");
    }

#elif APP_ENABLE_WM8904_AUDIO_STARTUP
    /* Codec-only path: no transport. */
    if (codec_present)
    {
        wm8904_audio_startup();
    }
    else
    {
        console_writeln("WM8904 audio startup skipped: codec did not answer");
    }

#elif APP_ENABLE_AUDIO_TRANSPORT
    /*
     * Transport without codec startup. This is legal and it compiles, but with
     * nothing driving BCLK/FSYNC the SERCOM client will sit silent forever and
     * every block counter will stay at 0 - which is correct behaviour, not a
     * fault. Said out loud so nobody debugs a missing clock as a DMA problem.
     */
#if APP_ENABLE_WM8904_PROBE
    /* The probe ran, but nothing here acts on the answer: this build never
     * touches the codec's clock registers. */
    (void)codec_present;
#endif
    console_write("audio transport init (no codec clock source) ... ");
    if (audio_transport_init() && audio_transport_start())
    {
        console_writeln("armed; will stay idle until something clocks BCLK/FS");
    }
    else
    {
        console_writeln("FAILED");
    }

#elif APP_ENABLE_WM8904_PROBE
    (void)codec_present;
    console_writeln("audio: disabled (APP_ENABLE_WM8904_AUDIO_STARTUP = 0, "
                    "APP_ENABLE_AUDIO_TRANSPORT = 0)");
#endif

#if APP_ENABLE_DSP_BENCH
    /*
     * The isolated benchmark runs on the bare board: it touches no codec, no SERCOM4 and
     * no DMA, and it is compiled only in the `bench` configuration.
     *
     * Order matters. The counter is selected first so the build info can
     * state which one it got; then correctness; then one timing point. A
     * cycle count from a kernel that computes the wrong answer is worse
     * than no cycle count, so the known-answer test comes first and its
     * verdict is printed before any number.
     *
     * This image auto-runs a measurement, because
     * the one number it exists to produce is the whole reason the build was
     * flashed. Everything else stays behind a key.
     */
    if (dsp_bench_init())
    {
        dsp_bench_print_build_info();

        if (dsp_bench_self_test())
        {
            dsp_bench_result_t r;

            if (dsp_bench_run(DSP_BENCH_DEFAULT_CHANNELS,
                              DSP_BENCH_DEFAULT_FRAMES,
                              DSP_BENCH_DEFAULT_STAGES,
                              DSP_BENCH_ITERATIONS, &r))
            {
                dsp_bench_print_result(&r);
            }
        }
        else
        {
            console_writeln("DSP bench: known-answer test FAILED - no timing "
                            "will be reported. Fix correctness first.");
        }
    }
#endif

    console_writeln("idle loop (LED heartbeat 1 Hz)");
    print_help();

    uint32_t tick = 0u;

    for (;;)
    {
        /*
         * 10 ms cadence so the console stays responsive, with the LED toggled
         * every 50 ticks (1 Hz). The old 500 ms blocking delay would have made
         * a keypress take up to half a second to be noticed and would have let
         * ~750 RX blocks pass between error sweeps.
         */
        board_delay_ms(10u);

        if (++tick >= 50u)
        {
            tick = 0u;
            board_led_toggle();
        }

#if APP_ENABLE_AUDIO_DRC
        app_uart_process();
#endif

#if APP_ENABLE_AUDIO_TRANSPORT
        audio_transport_poll_errors();

#if APP_AUDIO_LOOPTHROUGH
        loopthrough_service();
#endif

        telemetry_tick(10u);

        if (audio_transport_rx_block_ready())
        {
            /*
             * The foreground does NO processing in a transport-only build. The loop-through build
             * is the explicit exception: its copy already happened in the DMA
             * ISR, because a 10 ms foreground cadence cannot service a
             * 666.7 us block.  Keep this ownership release for the existing
             * diagnostic API; it does not take part in the ISR data path.
             */
            (void)audio_transport_get_rx_block();
            (void)audio_transport_get_tx_block();
            audio_transport_release_block();
        }
#endif
    }

    /* not reached */
}
