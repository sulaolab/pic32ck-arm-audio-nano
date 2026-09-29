#include "app_config.h"

#if APP_ENABLE_AUDIO_DRC

#include <stddef.h>

#include "general_console.h"

#include "drivers/console.h"

#define GEN_STRINGIFY2(x) #x
#define GEN_STRINGIFY(x)  GEN_STRINGIFY2(x)
#ifndef PIC32CK_GIT_COMMIT
#define PIC32CK_GIT_COMMIT (not-stamped)
#endif

void general_console_onmsg(app_console_msg_t *msg)
{
    if (msg == NULL) return;
    if (msg->kind != '?')
    {
        msg->data_len = 0u;
        msg->status = APP_CONSOLE_ERR_UNSUPPORTED;
        return;
    }

    switch (msg->name)
    {
    case 'v':
        console_printf(" PIC32CK console-v2 Audio-Bench %s\n",
                       GEN_STRINGIFY(PIC32CK_GIT_COMMIT));
        msg->data_len = 0u;
        msg->status = APP_CONSOLE_OK;
        break;
    case 'h':
        console_writeln(" PIC32CK console hello");
        msg->data_len = 0u;
        msg->status = APP_CONSOLE_OK;
        break;
    default:
        msg->data_len = 0u;
        msg->status = APP_CONSOLE_ERR_NOT_FOUND;
        break;
    }
}

#endif /* APP_ENABLE_AUDIO_DRC */
