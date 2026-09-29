/*******************************************************************************
*
*******************************************************************************/


/***  Include Files ***********************************************************/

#include "app_specific_config_defs.h"
#include "app_runtime_overrides.h"

#if APP_ENABLE_AUDIO_DRC

#include <xc.h>
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "apps/sonora_app_console.h" // selected-app command ownership + raw hotkeys (app-blind)
#include "general_console.h"            // common module 'g' (?gv version, ?gh hello)
#include "audio_transport_console.h"    // common module 't' (*tr restart)
#if !defined(SONORA_PLATFORM_PIC32CK)
#include "system_console.h"             // common module 's' (?si UDID)
#include "diag_console.h"               // common module 'd' (?dr reg-dump)
#include "traps_console.h"              // common module 'x' (?xl latch, *xa/*xm/*xs force)
#include "touch_console.h"              // touch module 'k' (raw ITC counts)
#include "resident_de/app/resident_de_app_console.h"  // module 'f': resident download engine (?fu, *feaa55)
#include "timer_app.h"   // GetTicks() ms tick (UART2 CSV reject-drain timeout)
#else
#include "sonora_classic_platform_pic32ck.h"
#endif
#include "uart_platform_uart1_usb_serial_port.h"
#include "uart_platform_uart2_usb_serial_device.h"
#include "app_console.h"
#include "app_biquad_coeff_csv.h"
#if ENA_DRC_DF2T_CASCADE
#include "ch_expand_2to4.h"   // module 'n' (?nd/*nd) 2->4 channel expander
#endif
#if defined(ENA_SAMPLE_DELAY)
#include "audio_sample_delay.h"   // module 'n' (?nd/*nd) sample delay
#endif


#include "app_debug.h"


/***  Module Macros  **********************************************************/

//#define APPDBGPRT(...)   // do nothing
#define APPDBGPRT        printf

#define APPDBG_UART_MARKER_CHAR                 ('#')
#define APPDBG_UART_ESC_CHAR                    (0x1Bu)
#define APPDBG_UART_MARKER_LINE_MAX             (128u)


/***  Module Types  ***********************************************************/

typedef enum {
    APPDBG_UART_RX_NORMAL = 0,
    APPDBG_UART_RX_MARKER_LINE,
    APPDBG_UART_RX_BIQUAD_CSV,
    APPDBG_UART_RX_UART2_CSV_REJECT_DRAIN,   /* UART2 tried CSV: swallow the rest */
} appdbg_uart_rx_mode_t;

/***  Module Variables  *******************************************************/

static appdbg_uart_rx_mode_t s_uart_rx_mode = APPDBG_UART_RX_NORMAL;

static struct {
    char     line[APPDBG_UART_MARKER_LINE_MAX];
    uint16_t idx;
} s_marker_rx;

/***  Module Function Prototypes  *********************************************/

static void  local_uart_process_rx_char( uint8_t c );
static void  local_uart_process_normal_char( uint8_t c );
static void  local_uart_marker_feed_char( uint8_t c );

static void  local_flush_uart_data( void );


static void  dbcapp_n_onmsg(app_console_msg_t* pmsg);

static void  dbcapp_nt_app_test( app_console_msg_t* pmsg );
static void  dbcapp_nd_app_dsp_param( app_console_msg_t* pmsg );




/***  Module Functions  *******************************************************/

/*
 * Source-lock for the two-port command input.
 *
 * Both Windows ports (UART1 "USB Serial Port" and UART2 "USB Serial Device")
 * are command-input endpoints feeding the SAME byte-wise parser. A
 * partially-typed command stays locked to the port it started on; the other
 * port's bytes are discarded until the line completes, so the shared line
 * buffer cannot be corrupted by interleaving. The lock releases at each command
 * boundary (parser back to NORMAL and no console line in progress).
 *
 * Simultaneous input is unsupported: users type on one port at a time; bytes
 * from the non-owner may be discarded. Idle polling below drains UART1 first,
 * so if both already have queued data at an idle boundary UART1 acquires the
 * lock first.
 */
typedef enum {
    INPUT_SRC_NONE = 0,
    INPUT_SRC_UART1,
    INPUT_SRC_UART2,
} appdbg_input_src_t;

static appdbg_input_src_t s_input_src = INPUT_SRC_NONE;
static void local_feed_locked( appdbg_input_src_t src, uint8_t c );

#if defined(ENA_BIQUAD_IIR_CASCADE)
/*
 * UART2 CSV reject-drain: after rejecting a CSV BEGIN on UART2 the host may keep
 * streaming the rest of the file. We swallow all further UART2 bytes (so payload
 * / END lines never leak into the command parser) until RX goes quiet. UART2 is
 * polling, so we must NOT wait for the END marker (it can be dropped under load)
 * -- release on an inactivity timeout instead.
 */
#define UART2_CSV_REJECT_DRAIN_TIMEOUT_MS   (500u)
static uint32_t s_uart2_csv_reject_drain_last_ms = 0u;
#endif //defined(ENA_BIQUAD_IIR_CASCADE)

/*
 * Which port is asking, for a handler that has to answer differently per port.
 *
 * The lock below is held ACROSS the dispatch (local_uart_process_rx_char() runs the
 * console command, and only then is the lock released), so a console handler reading
 * this is still seeing the port its own command arrived on. That is what makes this
 * usable from a verb rather than only at the marker-line boundary where the CSV
 * policy below lives.
 *
 * Kept as a predicate rather than exporting appdbg_input_src_t: callers so far only
 * ever ask "is this the PKOB4 mirror?", and a bare enum invites a caller to grow a
 * three-way switch over an enum whose UART1/NONE distinction is an internal detail.
 */
bool app_debug_input_is_uart2( void )
{
    return ( s_input_src == INPUT_SRC_UART2 );
}

static void local_feed_locked( appdbg_input_src_t src, uint8_t c )
{
    if( (s_input_src != INPUT_SRC_NONE) && (s_input_src != src) )
    {
        return;   /* another port owns an in-progress command line */
    }
    s_input_src = src;

    local_uart_process_rx_char( c );

    if( (s_uart_rx_mode == APPDBG_UART_RX_NORMAL) && app_console_is_line_idle() )
    {
        s_input_src = INPUT_SRC_NONE;   /* command boundary -> release lock */
    }
}

void app_uart_process(void)
{
    while( UART1_IsRxReady() )
    {
        local_feed_locked( INPUT_SRC_UART1, UART1_Read() );
    }
#if !APP_ASRC_MEAS_UART2_STREAM
    /* Stream build: UART2 is a dedicated binary DATA port (TX-only) -- no command input from
     * it, so the whole control plane lives on UART1. Normal build drains UART2 commands too. */
    while( UART2_IsRxReady() )
    {
        local_feed_locked( INPUT_SRC_UART2, UART2_Read() );
    }
#endif
#if defined(ENA_BIQUAD_IIR_CASCADE)
    /* End the UART2 CSV reject-drain only once UART2 RX is BOTH empty (no queued
     * byte still to discard) AND quiet for the timeout. This check is AFTER the
     * UART2 drain on purpose: releasing before draining could hand FIFO-queued
     * payload bytes to the command parser on the next loop. Inactivity, not the
     * END marker, is the release condition (polling can drop END under load). */
    if( (s_uart_rx_mode == APPDBG_UART_RX_UART2_CSV_REJECT_DRAIN) &&
        !UART2_IsRxReady() &&
        ((uint32_t)(GetTicks() - s_uart2_csv_reject_drain_last_ms) >= UART2_CSV_REJECT_DRAIN_TIMEOUT_MS) )
    {
        s_uart_rx_mode = APPDBG_UART_RX_NORMAL;
        s_input_src    = INPUT_SRC_NONE;
    }
    app_biquad_coeff_csv_task();

    /*
     * The CSV module can now end a transaction without a byte to carry the
     * transition: its inactivity deadline and its RX-integrity abort both fire
     * from the task, not from local_uart_process_rx_char().  Release RX mode
     * and the source lock here as well, so timeout recovery cannot leave the
     * console owned forever.
     */
    if( (s_uart_rx_mode == APPDBG_UART_RX_BIQUAD_CSV) &&
        !app_biquad_coeff_csv_is_receiving() )
    {
        s_uart_rx_mode = APPDBG_UART_RX_NORMAL;
        s_input_src    = INPUT_SRC_NONE;
    }
#endif //defined(ENA_BIQUAD_IIR_CASCADE)

#if !defined(SONORA_PLATFORM_PIC32CK)
    /* Bring a diagnostically detached U1TX pad back on its own deadline (*du00<SS>). Serviced
     * here because this is the one per-loop function that already owns the console plumbing,
     * and because a detached console has no reply channel to ask for the restore twice. */
    diag_console_route_tick();
#endif
}




void app_onmsg(app_console_msg_t* msg)
{
    if (!msg) return;

    switch (msg->module)
    {
        case 'n': dbcapp_n_onmsg(msg); break;
        case 'g': general_console_onmsg(msg); break; // common general/basic-info (?gv, ?gh)
        case 't': audio_transport_console_onmsg(msg); break; // common transport (*tr restart)
#if !defined(SONORA_PLATFORM_PIC32CK)
        case 's': system_console_onmsg(msg); break;          // common system/board (?si UDID)
        case 'd': diag_console_onmsg(msg); break;             // common diagnostics (?dr reg-dump)
        case 'x': traps_console_onmsg(msg); break;
        case 'k': touch_console_onmsg(msg); break;            // common exceptions/traps (?xl, *xa/*xm/*xs)
        case 'f': resident_de_app_console_onmsg(msg); break;  // resident download engine (?fu, *feaa55)
//        case 'm': app_m_onmsg(msg); break;
//        case 'i': app_i_onmsg(msg); break;

#endif
        // Any module not owned by common code routes to the linker-selected application.
        // (ASRC owns 'a', Classic owns 'c'); the app validates its own module letter.
        default:  sonora_app_console_onmsg(msg);   break;
    }
}






/***  Local Functions  *******************************************************/

static void local_uart_process_rx_char( uint8_t c )
{
    switch( s_uart_rx_mode )
    {
#if defined(ENA_BIQUAD_IIR_CASCADE)
    case APPDBG_UART_RX_BIQUAD_CSV:
        if( !app_biquad_coeff_csv_feed_char( c ) )
        {
            s_uart_rx_mode = APPDBG_UART_RX_NORMAL;
        }
        return;

    case APPDBG_UART_RX_MARKER_LINE:
        local_uart_marker_feed_char( c );
        return;

    case APPDBG_UART_RX_UART2_CSV_REJECT_DRAIN:
        /* Rejected UART2 CSV stream: discard the byte and refresh the
         * inactivity deadline. app_uart_process() returns to NORMAL (and
         * releases the source lock) after UART2_CSV_REJECT_DRAIN_TIMEOUT_MS. */
        s_uart2_csv_reject_drain_last_ms = GetTicks();
        return;
#endif //defined(ENA_BIQUAD_IIR_CASCADE)

    case APPDBG_UART_RX_NORMAL:
    default:
        s_uart_rx_mode = APPDBG_UART_RX_NORMAL;
        break;
    }

    if( c == APPDBG_UART_MARKER_CHAR )
    {
        s_marker_rx.idx = 0u;
        s_marker_rx.line[s_marker_rx.idx] = (char)c;
        s_marker_rx.idx = (uint16_t)(s_marker_rx.idx + 1u);
        s_uart_rx_mode = APPDBG_UART_RX_MARKER_LINE;
        return;
    }

    local_uart_process_normal_char( c );
}

static void local_uart_process_normal_char( uint8_t c )
{
    // Command-mode gate: once a '*'/'?' console command line is started, route EVERY subsequent
    // char to the console line buffer until the line completes -- do NOT let single-key hotkeys
    // (e.g. 'C'=Click-Clack) steal a hex nibble mid-command (was the original design; a refactor
    // that merged hotkeys and the console into one switch had lost it). See app_console_is_idle().
    static bool s_in_console_cmd = false;
    if( ( c == '*' ) || ( c == '?' ) ) { s_in_console_cmd = true; }
    if( s_in_console_cmd )
    {
        (void)app_console_feed_char( c );
        if( app_console_is_idle() ) { s_in_console_cmd = false; }  // line dispatched / emptied
        return;
    }

    // Raw single-key hotkeys are owned by the linker-selected application (app-blind contract).
    // The app consumes the keys it owns; anything it ignores falls through to the console parser.
    // A HANDLED_FLUSH result means the action may have blocked (mute/settle delay, synth blip), so
    // we drop keystrokes that queued during it -- preserving the original per-key flush behavior.
    sonora_hotkey_result_t hk = sonora_app_handle_hotkey( (char)c );
    if( hk != SONORA_HOTKEY_IGNORED )
    {
        if( hk == SONORA_HOTKEY_HANDLED_FLUSH )
        {
            local_flush_uart_data();
        }
        return;
    }

    (void)app_console_feed_char( c );
}



#if defined(ENA_BIQUAD_IIR_CASCADE)
static void local_uart_marker_feed_char( uint8_t c )
{
    if( c == APPDBG_UART_ESC_CHAR )
    {
        s_marker_rx.idx = 0u;
        s_uart_rx_mode = APPDBG_UART_RX_NORMAL;
        printf("\nUART marker monitor abort\n");
        return;
    }

    if( c == '\r' )
    {
        return;
    }

    if( c != '\n' )
    {
        if( s_marker_rx.idx < (APPDBG_UART_MARKER_LINE_MAX - 1u) )
        {
            s_marker_rx.line[s_marker_rx.idx] = (char)c;
            s_marker_rx.idx = (uint16_t)(s_marker_rx.idx + 1u);
        }
        else
        {
            s_marker_rx.idx = 0u;
            s_uart_rx_mode = APPDBG_UART_RX_NORMAL;
            printf("\nUART marker line too long. ignored.\n");
        }
        return;
    }

    s_marker_rx.line[s_marker_rx.idx] = '\0';

    /*
     * Input-source policy: Biquad CSV bulk transfer is a UART1-only transport
     * feature (UART2 RX is polling -- not suited to continuous bulk RX; see the
     * uart2 header). Reject a CSV BEGIN arriving from UART2 BEFORE entering CSV
     * receive mode, so we never start CSV and then overflow/abort mid-stream.
     * The source lock is owned here, so this is the right boundary for it.
     */
    if( (s_input_src == INPUT_SRC_UART2) &&
        app_biquad_coeff_csv_is_begin_marker( s_marker_rx.line ) )
    {
        printf("\n[CONSOLE] Biquad CSV transfer is not supported on \"USB Serial Device\". Use \"USB Serial Port\".\n");
        /*
         * Do NOT enter CSV receive mode. The host may still stream the rest of
         * the CSV file, so enter the reject-drain state: all further UART2 bytes
         * are discarded until RX goes quiet (inactivity timeout), so payload /
         * END lines never leak into the command parser. The source lock stays
         * held (mode != NORMAL) until the drain ends.
         */
        s_uart_rx_mode = APPDBG_UART_RX_UART2_CSV_REJECT_DRAIN;
        s_uart2_csv_reject_drain_last_ms = GetTicks();
        s_marker_rx.idx = 0u;
        return;
    }

    switch( app_biquad_coeff_csv_process_marker_line( s_marker_rx.line ) )
    {
    case APP_BIQUAD_COEFF_CSV_MARKER_STARTED:
        s_uart_rx_mode = APPDBG_UART_RX_BIQUAD_CSV;
        break;

    case APP_BIQUAD_COEFF_CSV_MARKER_CONSUMED:
        s_uart_rx_mode = APPDBG_UART_RX_NORMAL;
        break;

    case APP_BIQUAD_COEFF_CSV_MARKER_NOT_MATCHED:
    default:
        /* '#' line is reserved. Unknown marker lines are intentionally ignored. */
        printf("\nUART marker ignored: %s\n", s_marker_rx.line);
        s_uart_rx_mode = APPDBG_UART_RX_NORMAL;
        break;
    }

    s_marker_rx.idx = 0u;
}
#endif //defined(ENA_BIQUAD_IIR_CASCADE)


static void local_flush_uart_data( void )
{
    uint8_t c = 0u;

    // flush UART data (both command-input ports)
    while( UART1_IsRxReady() )
    {
        c = UART1_Read();
    }
    while( UART2_IsRxReady() )
    {
        c = UART2_Read();
    }
    (void)c;   // make compiler happy
}




static void dbcapp_n_onmsg(app_console_msg_t* pmsg)
{
    switch (pmsg->name)
    {
    case 't':
        dbcapp_nt_app_test(pmsg);
        break;
    case 'd':
        dbcapp_nd_app_dsp_param(pmsg);
        break;
    default:
        pmsg->status   = APP_CONSOLE_ERR_NOT_FOUND;
        pmsg->data_len = 0;
    }
}

#if 0
/* Optional debug-command placeholders are not registered by this dispatcher. */
static void dbcapp_m_onmsg(app_console_msg_t* pmsg)
{
    pmsg->status   = APP_CONSOLE_ERR_NOT_FOUND;
    pmsg->data_len = 0;
}

static void dbcapp_i_onmsg(app_console_msg_t* pmsg)
{
    pmsg->status   = APP_CONSOLE_ERR_NOT_FOUND;
    pmsg->data_len = 0;
}
#endif
















static void dbcapp_nt_app_test( app_console_msg_t* pmsg )
{
    uint8_t* pdata;


    pdata = (uint8_t*)&(pmsg->data[0]);

    if (pmsg->kind == '*')
    {
        switch( pdata[0] )
        {
        case 0x00:
        {
            printf(" \"*\" hello debug console!!\n");
            pmsg->data_len = 0;
            pmsg->status   = APP_CONSOLE_OK;
            break;
        }
        default:
            pmsg->data_len = 0;
            pmsg->status   = APP_CONSOLE_ERR_BAD_DATA;
            break;
        }
    }
    else if (pmsg->kind == '?')
    {
        switch( pdata[0] )
        {
        case 0x0:
            printf(" \"?\" hello debug console!!\n");
            pmsg->data_len = 0;
            pmsg->status   = APP_CONSOLE_OK;
            break;


        // case 0xCD (WM8904 register dump) removed: moved to ?dr (module 'd', diag_console.c).

        default:
            pmsg->data_len = 0;
            pmsg->status   = APP_CONSOLE_ERR_BAD_DATA;
            break;
        }
    }
    else
    {
       pmsg->data_len = 0;
       pmsg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
    }
}


// dbcapp_na_app_execute() removed: 0x0 (bass enhancer LPF cap, set + print) moved to *cb/?cs
// (module 'c', classic_console.c). 0x1-0x5/0x61 were dead no-op stubs (confirmed via git
// history: no real body since the initial commit) with no defined semantics, so they were
// dropped rather than carried forward as placeholders.


static void dbcapp_nd_app_dsp_param( app_console_msg_t* pmsg )
{
    uint8_t* pdata;


    pdata = (uint8_t*)&(pmsg->data[0]);

    if (pmsg->kind == '*')
    {
        switch( pdata[0] )
        {
#if defined(ENA_SAMPLE_DELAY)
        case 0x50:
        {
            if( pmsg->data_len == 4 )
            {
                uint16_t samples = ((pdata[2] << 8) | (pdata[3] << 0));
                pdata[4] = app_audio_sample_delay_set_delay_samples( pdata[1], samples );

                pmsg->data_len += 1;  // return the result of func
                pmsg->status   = APP_CONSOLE_OK;
            }
            else
            {
                pmsg->status   = APP_CONSOLE_ERR_BAD_PARM_LEN;
            }
            break;
        }
#endif //defined(ENA_SAMPLE_DELAY)
#if ENA_DRC_DF2T_CASCADE
        case 0x60:
        {
            if( pmsg->data_len == 6 )
            {
                uint32_t gain_hex = ( ((uint32_t)pdata[2] << 24)
                                    | ((uint32_t)pdata[3] << 16)
                                    | ((uint32_t)pdata[4] << 8)
                                    | ((uint32_t)pdata[5] << 0) );
                float gain = 0.0f;

                app_memcpy( &gain, &gain_hex, sizeof(gain) );
                if( app_ch_expand_2to4_set_gain(pdata[1], gain) )
                {
                    pmsg->status   = APP_CONSOLE_OK;
                }
                else
                {
                    pmsg->status   = APP_CONSOLE_ERR_BAD_DATA;
                }
            }
            else
            {
                pmsg->status   = APP_CONSOLE_ERR_BAD_PARM_LEN;
            }
            break;
        }
#endif //ENA_DRC_DF2T_CASCADE

        default:
            pmsg->status = APP_CONSOLE_ERR_UNSUPPORTED;
            break;
        }
    }
    else if (pmsg->kind == '?')
    {
        switch( pdata[0] )
        {
#if defined(ENA_SAMPLE_DELAY)
        case 0x50:
        {
            uint16_t samples = 0;
            app_audio_sample_delay_get_delay_samples( pdata[1], &samples );

            pdata[2] = ((samples & 0xFF00) >> 8);
            pdata[3] = ((samples & 0x00FF) >> 0);
            pmsg->data_len = 4;
            pmsg->status   = APP_CONSOLE_OK;
            break;
        }
        case 0x51:
        {
            if( pmsg->data_len == 1 )  // 0x51 only of 1Byte
            {
                app_audio_sample_delay_debug_print_status();
                pmsg->status = APP_CONSOLE_OK;
            }
            else
            {
                pmsg->status = APP_CONSOLE_ERR_BAD_PARM_LEN;
            }
            break;
        }
#endif //defined(ENA_SAMPLE_DELAY)
#if ENA_DRC_DF2T_CASCADE
        case 0x60:
        {
            float    gain     = 0.0f;
            uint32_t gain_hex = 0;

            if( app_ch_expand_2to4_get_gain(pdata[1], &gain) )
            {
                app_memcpy( &gain_hex, &gain, sizeof(gain_hex) );

                pdata[2] = (uint8_t)((gain_hex & 0xFF000000u) >> 24);
                pdata[3] = (uint8_t)((gain_hex & 0x00FF0000u) >> 16);
                pdata[4] = (uint8_t)((gain_hex & 0x0000FF00u) >> 8);
                pdata[5] = (uint8_t)((gain_hex & 0x000000FFu) >> 0);

                pmsg->data_len = 6;
                pmsg->status   = APP_CONSOLE_OK;
            }
            else
            {
                pmsg->data_len = 2;
                pmsg->status   = APP_CONSOLE_ERR_BAD_DATA;
            }
            break;
        }
        case 0x61:
        {
            if( pmsg->data_len == 1 )  // 0x61 only of 1Byte
            {
                app_ch_expand_2to4_print_status();
                pmsg->status = APP_CONSOLE_OK;
            }
            else
            {
                pmsg->status = APP_CONSOLE_ERR_BAD_PARM_LEN;
            }
            break;
        }
#endif //ENA_DRC_DF2T_CASCADE

        default:
            pmsg->status = APP_CONSOLE_ERR_UNSUPPORTED;
            break;
        }
    }
    else
    {
       pmsg->data_len = 0;
       pmsg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
    }
}

#endif /* APP_ENABLE_AUDIO_DRC */
