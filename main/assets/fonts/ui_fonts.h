/*
 * ui_fonts.h — Centralised font declarations for the ESP32 Voice Companion UI.
 *
 * PuHui: STHeiti-derived font (16/20/24 px, 4bpp) covering ASCII + project CJK
 * FontAwesome: subset of FA5 Solid (20/30 px, 4bpp) for status bar / tile icons
 */
#pragma once
#include "lvgl.h"

/* ── Declarations ─────────────────────────────────────────────────────────── */
LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_24_4);
LV_FONT_DECLARE(font_awesome_20_4);
LV_FONT_DECLARE(font_awesome_30_4);

/* ── Convenience aliases ──────────────────────────────────────────────────── */
#define UI_FONT_TEXT_SM    (&font_puhui_16_4)   /* tile labels, hints    */
#define UI_FONT_TEXT       (&font_puhui_20_4)   /* body, settings rows   */
#define UI_FONT_TEXT_LG    (&font_puhui_24_4)   /* headings, titles      */
#define UI_FONT_ICON       (&font_awesome_20_4) /* status bar icons      */
#define UI_FONT_ICON_LG    (&font_awesome_30_4) /* large icon glyphs     */

/* ── FontAwesome 5 glyph codepoints (UTF-8 encoded string literals) ───────── */
#define FA_WIFI         "\xEF\x87\xAB"   /* 0xF1EB  WiFi             */
#define FA_BATTERY_FULL "\xEF\x89\x80"   /* 0xF240  Battery full     */
#define FA_BATTERY_HALF "\xEF\x89\x82"   /* 0xF242  Battery 1/2      */
#define FA_BATTERY_LOW  "\xEF\x89\x83"   /* 0xF243  Battery low      */
#define FA_VOLUME_UP    "\xEF\x80\xA8"   /* 0xF028  Volume up        */
#define FA_VOLUME_MUTE  "\xEF\x9A\xA9"   /* 0xF6A9  Volume mute      */
#define FA_GEAR         "\xEF\x80\x93"   /* 0xF013  Gear/settings    */
#define FA_COMMENT      "\xEF\x81\xB5"   /* 0xF075  Comment/chat     */
#define FA_PLUS         "\xEF\x81\xA7"   /* 0xF067  Plus             */
#define FA_CHEVRON_LEFT "\xEF\x81\x93"   /* 0xF053  Chevron left     */
#define FA_HOME         "\xEF\x80\x95"   /* 0xF015  Home             */
