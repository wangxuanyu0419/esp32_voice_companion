#ifndef VAD_H
#define VAD_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    VAD_STATE_SILENCE = 0,
    VAD_STATE_SPEECH,
} vad_state_t;

esp_err_t   vad_init(void);
void        vad_reset(void);
vad_state_t vad_process_frame(const int16_t *pcm_frame, size_t frame_size);
int32_t     vad_get_energy(void);
void        vad_set_sensitivity(int level);
vad_state_t vad_get_state(void);
const char *vad_get_text(void);

typedef void (*vad_callback_t)(vad_state_t state, void *arg);
void vad_register_callback(vad_callback_t cb, void *arg);

#endif /* VAD_H */
