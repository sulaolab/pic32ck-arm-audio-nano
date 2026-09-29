/* Sonora common transport namespace, with PIC32CK platform hooks. */
#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"

#if APP_ENABLE_AUDIO_DRC

#include <stddef.h>

#include "audio_transport_console.h"
#include "drivers/console.h"

void audio_transport_console_onmsg(app_console_msg_t *msg)
{
    if (msg == NULL) return;

    switch (msg->name)
    {
    case 's':
        if (msg->data_len != 0u)
        {
            console_writeln(" \"*ts/?ts\" takes no value");
            msg->data_len = 0u;
            msg->status = APP_CONSOLE_ERR_BAD_PARM_LEN;
            break;
        }
        if (msg->kind == '?')
        {
            pic32ck_console_report_audio_stop_for_flash();
            msg->data_len = 0u;
            msg->status = APP_CONSOLE_OK;
            break;
        }
        if (msg->kind != '*')
        {
            msg->data_len = 0u;
            msg->status = APP_CONSOLE_ERR_UNSUPPORTED;
            break;
        }
        msg->data_len = 0u;
        msg->status = pic32ck_console_stop_audio_for_flash()
                          ? APP_CONSOLE_OK
                          : APP_CONSOLE_ERR_OPERATION_FAILED;
        break;

    case 'r':
        if ((msg->kind != '*') || (msg->data_len != 0u))
        {
            msg->data_len = 0u;
            msg->status = (msg->kind != '*') ? APP_CONSOLE_ERR_UNSUPPORTED
                                              : APP_CONSOLE_ERR_BAD_PARM_LEN;
            break;
        }
        console_writeln(" \"*tr\" force audio stop/restart (same rate)");
        msg->data_len = 0u;
        msg->status = pic32ck_console_restart_audio()
                          ? APP_CONSOLE_OK
                          : APP_CONSOLE_ERR_OPERATION_FAILED;
        break;

    case 'q':
        if (msg->kind != '*')
        {
            msg->data_len = 0u;
            msg->status = APP_CONSOLE_ERR_UNSUPPORTED;
            break;
        }
        {
            const uint16_t mode = (msg->data_len >= 2u)
                                      ? (uint16_t)(((uint16_t)msg->data[0] << 8) |
                                                   msg->data[1])
                                      : ((msg->data_len == 1u) ? msg->data[0] : 0u);
            if (mode == 2u)
            {
                const uint16_t ms = (msg->data_len >= 4u)
                                        ? (uint16_t)(((uint16_t)msg->data[2] << 8) |
                                                     msg->data[3])
                                        : 0u;
                pic32ck_console_set_telemetry_period_ms(ms);
                console_printf(" \"*tq\" telemetry %s period=%ums\n",
                               (ms == 0u) ? "OFF" : "ON", (uint32_t)ms);
            }
            else
            {
                const bool on = mode != 0u;
                pic32ck_console_set_telemetry_period_ms(
                    on ? (uint32_t)APP_DBG_PERIOD_MS : 0u);
                console_printf(" \"*tq\" telemetry %s\n",
                               on ? "ON" : "OFF");
            }
            msg->data_len = 0u;
            msg->status = APP_CONSOLE_OK;
        }
        break;

    default:
        msg->data_len = 0u;
        msg->status = APP_CONSOLE_ERR_NOT_FOUND;
        break;
    }
}

#endif /* APP_ENABLE_AUDIO_DRC */
