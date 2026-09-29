#ifndef AUDIO_TRANSPORT_CONSOLE_H
#define AUDIO_TRANSPORT_CONSOLE_H

#include "app_console.h"

void audio_transport_console_onmsg(app_console_msg_t *msg);

/* Platform hooks implemented beside the PIC32CK transport owner in main.c. */
void pic32ck_console_set_telemetry_period_ms(uint32_t period_ms);
bool pic32ck_console_restart_audio(void);
bool pic32ck_console_stop_audio_for_flash(void);
void pic32ck_console_report_audio_stop_for_flash(void);

#endif /* AUDIO_TRANSPORT_CONSOLE_H */
