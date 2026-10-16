/*
 * cbin_font.h - loader for the legacy CBIN fonts emitted by
 * 78/lv_font_conv (the format used by xiaozhi-fonts).
 *
 * The CBIN header is not the LVGL 8 lv_font_t layout: it stores 32-bit
 * relative offsets and a third, reserved callback slot.  Keeping the small
 * compatibility layer here lets the project consume the previously
 * validated common CJK assets without changing the global LVGL font ABI.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cbin_font cbin_font_t;

/*
 * On success the loader owns bin_addr; it must remain alive until
 * cbin_font_delete() is called because the LVGL font points into it.
 */
cbin_font_t *cbin_font_create(uint8_t *bin_addr, size_t bin_size);

const lv_font_t *cbin_font_get(const cbin_font_t *font);

/* Releases both the LVGL wrapper and the owned CBIN buffer. */
void cbin_font_delete(cbin_font_t *font);

#ifdef __cplusplus
}
#endif
