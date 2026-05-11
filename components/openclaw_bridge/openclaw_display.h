/*
 * openclaw_display.h - C-callable display interface
 */
#ifndef _OPENCLAW_DISPLAY_H_
#define _OPENCLAW_DISPLAY_H_

#ifdef __cplusplus
extern "C" {
#endif

void openclaw_display_set_state(const char* state);
void openclaw_display_show_text(const char* text);
void openclaw_display_set_emotion(const char* emotion);
void openclaw_display_notify(const char* msg, int duration_ms);

#ifdef __cplusplus
}
#endif

#endif
