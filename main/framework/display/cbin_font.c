/*
 * cbin_font.c - bounds-checked reader for legacy 32-bit CBIN fonts.
 *
 * The format is emitted by the pinned 78/lv_font_conv c420999f converter.
 * It is deliberately read by offsets instead of casting the file image to
 * LVGL structs: LVGL 8.4 uses a different lv_font_t header and, depending on
 * LV_FONT_FMT_TXT_LARGE, a different glyph descriptor layout.
 */

#include "cbin_font.h"

#include "esp_log.h"

#include <stdbool.h>

static const char *TAG = "CBIN_FONT";

enum {
    CBIN_FONT_HEADER_SIZE = 36, /* 3 callbacks + metrics + 3 offsets */
    CBIN_FONT_DSC_SIZE    = 24, /* 4 pointers + scale/flags + padding */
    CBIN_CMAP_RECORD_SIZE = 20,
    CBIN_GLYPH_RECORD_SIZE = 16,
    CBIN_KERN_CLASS_SIZE  = 16,
};

typedef struct {
    uint32_t bitmap_index;
    uint32_t adv_w;
    uint16_t box_w;
    uint16_t box_h;
    int16_t ofs_x;
    int16_t ofs_y;
} cbin_glyph_dsc_t;

struct cbin_font {
    lv_font_t lv_font;
    uint8_t *data;
    size_t size;

    /* All offsets below are relative to dsc_offset unless documented. */
    uint32_t dsc_offset;
    uint32_t glyph_bitmap_offset;
    uint32_t glyph_dsc_offset;
    uint32_t cmap_offset;
    uint32_t kern_offset;
    uint16_t kern_scale;
    uint16_t cmap_num;
    uint8_t bpp;
    uint8_t kern_classes;
    uint8_t bitmap_format;
};

static uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static int16_t read_i16(const uint8_t *p)
{
    return (int16_t)read_u16(p);
}

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int32_t read_i32(const uint8_t *p)
{
    return (int32_t)read_u32(p);
}

static bool span_ok(const cbin_font_t *font, size_t offset, size_t length)
{
    return offset <= font->size && length <= font->size - offset;
}

static bool rel_span_ok(const cbin_font_t *font, uint32_t relative,
                        size_t length)
{
    if (relative > font->size - font->dsc_offset) {
        return false;
    }
    return span_ok(font, (size_t)font->dsc_offset + relative, length);
}

static const uint8_t *dsc_ptr(const cbin_font_t *font, uint32_t relative)
{
    if (!rel_span_ok(font, relative, 1)) {
        return NULL;
    }
    return font->data + font->dsc_offset + relative;
}

static bool read_cmap(const cbin_font_t *font, uint16_t index,
                      uint32_t *range_start, uint16_t *range_length,
                      uint16_t *glyph_id_start, const uint8_t **unicode_list,
                      const uint8_t **glyph_id_ofs_list, uint16_t *list_length,
                      uint8_t *type)
{
    if (index >= font->cmap_num || font->cmap_num > 256) {
        return false;
    }

    size_t record_offset = (size_t)font->dsc_offset + font->cmap_offset +
                           (size_t)index * CBIN_CMAP_RECORD_SIZE;
    if (!span_ok(font, record_offset, CBIN_CMAP_RECORD_SIZE)) {
        return false;
    }

    const uint8_t *record = font->data + record_offset;
    *range_start = read_u32(record + 0);
    *range_length = read_u16(record + 4);
    *glyph_id_start = read_u16(record + 6);
    uint32_t unicode_relative = read_u32(record + 8);
    uint32_t glyph_id_relative = read_u32(record + 12);
    *list_length = read_u16(record + 16);
    *type = record[18];

    *unicode_list = NULL;
    *glyph_id_ofs_list = NULL;

    if (*type != LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY &&
        *type != LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL &&
        *type != LV_FONT_FMT_TXT_CMAP_SPARSE_TINY &&
        *type != LV_FONT_FMT_TXT_CMAP_SPARSE_FULL) {
        return false;
    }

    bool sparse = (*type == LV_FONT_FMT_TXT_CMAP_SPARSE_TINY ||
                   *type == LV_FONT_FMT_TXT_CMAP_SPARSE_FULL);
    bool full = (*type == LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL ||
                 *type == LV_FONT_FMT_TXT_CMAP_SPARSE_FULL);

    if (sparse) {
        size_t bytes = (size_t)*list_length * sizeof(uint16_t);
        if (!rel_span_ok(font, font->cmap_offset + unicode_relative, bytes)) {
            return false;
        }
        *unicode_list = dsc_ptr(font, font->cmap_offset + unicode_relative);
    }
    if (full) {
        size_t item_size = sparse ? sizeof(uint16_t) : sizeof(uint8_t);
        size_t bytes = (size_t)*list_length * item_size;
        if (!rel_span_ok(font, font->cmap_offset + glyph_id_relative, bytes)) {
            return false;
        }
        *glyph_id_ofs_list = dsc_ptr(font,
                                     font->cmap_offset + glyph_id_relative);
    }

    return true;
}

static bool find_glyph(const cbin_font_t *font, uint32_t letter,
                       cbin_glyph_dsc_t *glyph, uint16_t *glyph_id)
{
    if (letter == '\0') {
        return false;
    }
    if (letter == '\t') {
        letter = ' ';
    }

    for (uint16_t i = 0; i < font->cmap_num; ++i) {
        uint32_t range_start;
        uint16_t range_length;
        uint16_t glyph_start;
        const uint8_t *unicode_list;
        const uint8_t *glyph_id_ofs_list;
        uint16_t list_length;
        uint8_t type;
        if (!read_cmap(font, i, &range_start, &range_length, &glyph_start,
                       &unicode_list, &glyph_id_ofs_list, &list_length,
                       &type)) {
            return false;
        }

        if (letter < range_start ||
            (uint64_t)letter >= (uint64_t)range_start + range_length) {
            continue;
        }

        uint32_t relative = letter - range_start;
        uint32_t gid = 0;
        if (type == LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY) {
            gid = glyph_start + relative;
        }
        else if (type == LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL) {
            gid = glyph_start + glyph_id_ofs_list[relative];
        }
        else {
            /* Sparse lists are sorted relative code points. */
            int32_t low = 0;
            int32_t high = (int32_t)list_length - 1;
            int32_t found = -1;
            while (low <= high) {
                int32_t middle = low + (high - low) / 2;
                uint16_t value = read_u16(unicode_list + middle * 2);
                if (value < relative) {
                    low = middle + 1;
                }
                else if (value > relative) {
                    high = middle - 1;
                }
                else {
                    found = middle;
                    break;
                }
            }
            if (found < 0) {
                return false;
            }
            gid = glyph_start + (uint32_t)found;
            if (type == LV_FONT_FMT_TXT_CMAP_SPARSE_FULL) {
                gid = glyph_start + read_u16(glyph_id_ofs_list + found * 2);
            }
        }

        size_t glyph_offset = (size_t)font->dsc_offset + font->glyph_dsc_offset +
                              (size_t)gid * CBIN_GLYPH_RECORD_SIZE;
        if (!span_ok(font, glyph_offset, CBIN_GLYPH_RECORD_SIZE)) {
            return false;
        }

        const uint8_t *record = font->data + glyph_offset;
        glyph->bitmap_index = read_u32(record + 0);
        glyph->adv_w = read_u32(record + 4);
        glyph->box_w = read_u16(record + 8);
        glyph->box_h = read_u16(record + 10);
        glyph->ofs_x = read_i16(record + 12);
        glyph->ofs_y = read_i16(record + 14);
        if (glyph_id) {
            *glyph_id = (uint16_t)gid;
        }
        return true;
    }

    return false;
}

static int8_t get_kern_value(const cbin_font_t *font, uint16_t left,
                             uint16_t right)
{
    if (!font->kern_offset || font->kern_classes != 1) {
        return 0;
    }

    const uint8_t *kern = dsc_ptr(font, font->kern_offset);
    if (!kern || !rel_span_ok(font, font->kern_offset, CBIN_KERN_CLASS_SIZE)) {
        return 0;
    }

    uint32_t values_relative = read_u32(kern + 0);
    uint32_t left_relative = read_u32(kern + 4);
    uint32_t right_relative = read_u32(kern + 8);
    uint8_t left_count = kern[12];
    uint8_t right_count = kern[13];
    if (left_count == 0 || right_count == 0) {
        return 0;
    }

    if (!rel_span_ok(font, font->kern_offset + values_relative,
                     (size_t)left_count * right_count) ||
        !rel_span_ok(font, font->kern_offset + left_relative, (size_t)left + 1) ||
        !rel_span_ok(font, font->kern_offset + right_relative, (size_t)right + 1)) {
        return 0;
    }

    const uint8_t *left_map = dsc_ptr(font, font->kern_offset + left_relative);
    const uint8_t *right_map = dsc_ptr(font, font->kern_offset + right_relative);
    uint8_t left_class = left_map[left];
    uint8_t right_class = right_map[right];
    if (left_class == 0 || right_class == 0 || left_class > left_count ||
        right_class > right_count) {
        return 0;
    }

    const uint8_t *values = dsc_ptr(font, font->kern_offset + values_relative);
    return (int8_t)values[(size_t)(left_class - 1) * right_count +
                          (right_class - 1)];
}

static bool cbin_get_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *out,
                               uint32_t letter, uint32_t letter_next)
{
    const cbin_font_t *cbin = (const cbin_font_t *)font->dsc;
    bool is_tab = letter == '\t';
    cbin_glyph_dsc_t glyph;
    uint16_t glyph_id;
    if (!cbin || !find_glyph(cbin, letter, &glyph, &glyph_id)) {
        return false;
    }

    cbin_glyph_dsc_t next_glyph;
    uint16_t next_id = 0;
    int8_t kern = 0;
    if (letter_next && find_glyph(cbin, letter_next, &next_glyph, &next_id)) {
        kern = get_kern_value(cbin, glyph_id, next_id);
    }

    int32_t advance = (int32_t)glyph.adv_w;
    advance += ((int32_t)kern * cbin->kern_scale) >> 4;
    advance = (advance + 8) >> 4;
    if (is_tab) {
        advance *= 2;
    }

    out->adv_w = (uint16_t)advance;
    out->box_w = (lv_coord_t)glyph.box_w;
    out->box_h = (lv_coord_t)glyph.box_h;
    out->ofs_x = (lv_coord_t)glyph.ofs_x;
    out->ofs_y = (lv_coord_t)glyph.ofs_y;
    out->bpp = cbin->bpp;
    out->is_placeholder = false;
    return true;
}

static const uint8_t *cbin_get_glyph_bitmap(const lv_font_t *font,
                                            uint32_t letter)
{
    const cbin_font_t *cbin = (const cbin_font_t *)font->dsc;
    cbin_glyph_dsc_t glyph;
    if (!cbin || !find_glyph(cbin, letter, &glyph, NULL) ||
        glyph.box_w == 0 || glyph.box_h == 0) {
        return NULL;
    }

    /* The shipped common profiles are plain CBIN bitmaps. */
    if (cbin->bitmap_format != 0) {
        return NULL;
    }

    size_t bitmap_offset = (size_t)cbin->dsc_offset +
                           cbin->glyph_bitmap_offset + glyph.bitmap_index;
    uint64_t bits = (uint64_t)glyph.box_w * glyph.box_h * cbin->bpp;
    size_t bitmap_bytes = (size_t)((bits + 7u) / 8u);
    if (!span_ok(cbin, bitmap_offset, bitmap_bytes)) {
        return NULL;
    }
    return cbin->data + bitmap_offset;
}

static bool validate_font(cbin_font_t *font)
{
    if (font->size < CBIN_FONT_HEADER_SIZE ||
        font->dsc_offset > font->size - CBIN_FONT_DSC_SIZE ||
        font->cmap_num == 0 || font->cmap_num > 256 || font->bpp == 0 ||
        font->bpp > 8 || font->bitmap_format != 0) {
        return false;
    }

    if (!rel_span_ok(font, font->glyph_bitmap_offset, 1) ||
        !rel_span_ok(font, font->glyph_dsc_offset, CBIN_GLYPH_RECORD_SIZE) ||
        !rel_span_ok(font, font->cmap_offset,
                     (size_t)font->cmap_num * CBIN_CMAP_RECORD_SIZE)) {
        return false;
    }

    for (uint16_t i = 0; i < font->cmap_num; ++i) {
        uint32_t range_start;
        uint16_t range_length;
        uint16_t glyph_start;
        const uint8_t *unicode_list;
        const uint8_t *glyph_id_ofs_list;
        uint16_t list_length;
        uint8_t type;
        if (!read_cmap(font, i, &range_start, &range_length, &glyph_start,
                       &unicode_list, &glyph_id_ofs_list, &list_length,
                       &type)) {
            return false;
        }
        (void)range_start;
        (void)range_length;
        (void)glyph_start;
        (void)unicode_list;
        (void)glyph_id_ofs_list;
        (void)list_length;
        (void)type;
    }

    if (font->kern_offset && font->kern_classes == 1 &&
        !rel_span_ok(font, font->kern_offset, CBIN_KERN_CLASS_SIZE)) {
        return false;
    }
    return true;
}

cbin_font_t *cbin_font_create(uint8_t *bin_addr, size_t bin_size)
{
    if (!bin_addr || bin_size < CBIN_FONT_HEADER_SIZE) {
        return NULL;
    }

    cbin_font_t *font = lv_mem_alloc(sizeof(*font));
    if (!font) {
        return NULL;
    }
    *font = (cbin_font_t){
        .data = bin_addr,
        .size = bin_size,
    };

    font->dsc_offset = read_u32(bin_addr + 24);
    if (font->dsc_offset > bin_size - CBIN_FONT_DSC_SIZE) {
        ESP_LOGE(TAG, "invalid descriptor offset: %u (size %u)",
                 (unsigned)font->dsc_offset, (unsigned)bin_size);
        lv_mem_free(font);
        return NULL;
    }

    const uint8_t *dsc = bin_addr + font->dsc_offset;
    font->glyph_bitmap_offset = read_u32(dsc + 0);
    font->glyph_dsc_offset = read_u32(dsc + 4);
    font->cmap_offset = read_u32(dsc + 8);
    font->kern_offset = read_u32(dsc + 12);
    font->kern_scale = read_u16(dsc + 16);
    uint16_t packed = read_u16(dsc + 18);
    font->cmap_num = packed & 0x01FFu;
    font->bpp = (packed >> 9) & 0x0Fu;
    font->kern_classes = (packed >> 13) & 0x01u;
    font->bitmap_format = (packed >> 14) & 0x03u;

    if (!validate_font(font)) {
        ESP_LOGE(TAG, "invalid/unsupported CBIN layout (size=%u, cmap=%u, bpp=%u, format=%u)",
                 (unsigned)bin_size, (unsigned)font->cmap_num,
                 (unsigned)font->bpp, (unsigned)font->bitmap_format);
        lv_mem_free(font);
        return NULL;
    }

    font->lv_font.get_glyph_dsc = cbin_get_glyph_dsc;
    font->lv_font.get_glyph_bitmap = cbin_get_glyph_bitmap;
    font->lv_font.line_height = (lv_coord_t)read_i32(bin_addr + 12);
    font->lv_font.base_line = (lv_coord_t)read_i32(bin_addr + 16);
    font->lv_font.subpx = bin_addr[20] & 0x03u;
    font->lv_font.underline_position = (int8_t)bin_addr[21];
    font->lv_font.underline_thickness = (int8_t)bin_addr[22];
    font->lv_font.dsc = font;
    font->lv_font.fallback = NULL;
#if LV_USE_USER_DATA
    font->lv_font.user_data = NULL;
#endif

    ESP_LOGI(TAG, "CBIN ready: %u KB, %u cmaps, %u bpp, line=%d",
             (unsigned)(bin_size / 1024), (unsigned)font->cmap_num,
             (unsigned)font->bpp, (int)font->lv_font.line_height);
    return font;
}

const lv_font_t *cbin_font_get(const cbin_font_t *font)
{
    return font ? &font->lv_font : NULL;
}

void cbin_font_delete(cbin_font_t *font)
{
    if (!font) {
        return;
    }
    lv_mem_free(font->data);
    lv_mem_free(font);
}
