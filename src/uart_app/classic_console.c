/* Sonora Classic module 'c': retain the shared biquad bypass command. */
#include "app_config.h"

#if APP_ENABLE_AUDIO_DRC

#include <stddef.h>

#include "apps/sonora_app_console.h"
#include "classic_console.h"

#include "drivers/console.h"
#include "dsp/sonora_drc_path.h"

void classic_console_onmsg(app_console_msg_t *msg)
{
    if (msg == NULL) return;
    if (msg->module != 'c')
    {
        msg->data_len = 0u;
        msg->status = APP_CONSOLE_ERR_NOT_FOUND;
        return;
    }

    switch (msg->name)
    {
    case 'f':
    {
        const uint16_t in_len = msg->data_len;
        const uint8_t bypass = (in_len > 0u) ? msg->data[0] : 0xFFu;
        msg->data_len = 0u;
        if (msg->kind != '*')
        {
            msg->status = APP_CONSOLE_ERR_UNSUPPORTED;
        }
        else if (in_len != 1u)
        {
            console_writeln(" \"*cf VV\" bad args VV=bypass (0=normal 1=bypass)");
            msg->status = APP_CONSOLE_ERR_BAD_DATA;
        }
        else
        {
            if (bypass != 0u)
            {
                app_biquad_cascade_4ch_request_bypass();
            }
            else
            {
                app_biquad_cascade_4ch_request_normal();
            }
            msg->status = APP_CONSOLE_OK;
        }
        break;
    }
    default:
        msg->data_len = 0u;
        msg->status = APP_CONSOLE_ERR_NOT_FOUND;
        break;
    }
}

void sonora_app_console_onmsg(app_console_msg_t *msg)
{
    classic_console_onmsg(msg);
}

#endif /* APP_ENABLE_AUDIO_DRC */
