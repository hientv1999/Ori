/**
 * @file lv_conf.h
 * LVGL 8.x configuration for Ori (Waveshare ESP32-S3 4.3" Touch LCD).
 *
 * Picked up via -DLV_CONF_INCLUDE_SIMPLE and -DLV_CONF_PATH in platformio.ini.
 *
 * Tuning notes:
 *  - Panel is 800x480 RGB565.
 *  - Framebuffer lives in PSRAM (lcd_panel.cpp). LVGL composes into a small
 *    partial buffer (~10 KB) in internal SRAM and we flush via DMA-capable
 *    Arduino_GFX draw16bitRGBBitmap().
 *  - Custom tick: lv_tick_inc() is called from loop() in main.cpp.
 *  - Custom log: routed to Serial for bring-up.
 *  - Animations / filesystem / image decoders trimmed for M2; they can be
 *    re-enabled in M3 when real UI lands.
 */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/*====================
   COLOR SETTINGS
 *====================*/
#define LV_COLOR_DEPTH        16
#define LV_COLOR_16_SWAP      0   /* RGB16-parallel panel — no byte swap */
#define LV_COLOR_SCREEN_TRANSP 0

/*=========================
   MEMORY SETTINGS
 *=========================*/
#ifdef ESP_PLATFORM
/* Firmware build — route LVGL's heap to the 8 MB PSRAM so the 64 KB SRAM
 * limit never starves the UI object tree. Draw buffers stay in SRAM (they
 * are statically declared in lcd_panel.cpp, not via lv_mem), so DMA is
 * unaffected. */
#define LV_MEM_CUSTOM         1
#define LV_MEM_CUSTOM_INCLUDE "lv_mem_custom.h"
#define LV_MEM_CUSTOM_ALLOC   lv_psram_malloc
#define LV_MEM_CUSTOM_REALLOC lv_psram_realloc
#define LV_MEM_CUSTOM_FREE    lv_psram_free
#else
/* Desktop simulator — LVGL's internal fixed-size pool.
 * build.ps1 overrides LV_MEM_SIZE via -DLV_MEM_SIZE=2097152 (2 MB). */
#define LV_MEM_CUSTOM         0
#ifndef LV_MEM_SIZE
#define LV_MEM_SIZE           (64U * 1024U)
#endif
#endif
#define LV_MEM_ADR            0
#define LV_MEM_BUF_MAX_NUM    16

/*====================
   HAL SETTINGS
 *====================*/
#define LV_DISP_DEF_REFR_PERIOD   16    /* ms — ~60 Hz target */
#define LV_INDEV_DEF_READ_PERIOD  16    /* ms */

/* Tickless: we call lv_tick_inc(elapsed_ms) from main.cpp loop(). */
#define LV_TICK_CUSTOM            1
#define LV_TICK_CUSTOM_INCLUDE    "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())

#define LV_DPI_DEF                130

/*=======================
 * FEATURE CONFIGURATION
 *======================*/

/* Drawing */
#define LV_DRAW_COMPLEX           1
#define LV_SHADOW_CACHE_SIZE      0
#define LV_CIRCLE_CACHE_SIZE      4
#define LV_LAYER_SIMPLE_BUF_SIZE  (24U * 1024U)
#define LV_IMG_CACHE_DEF_SIZE     0
#define LV_GRADIENT_MAX_STOPS     2
#define LV_GRAD_CACHE_DEF_SIZE    0
#define LV_DISP_ROT_MAX_BUF       (10U * 1024U)

/* GPU — disabled, software render only. */
#define LV_USE_GPU_ARM2D          0
#define LV_USE_GPU_STM32_DMA2D    0
#define LV_USE_GPU_SWM341_DMA2D   0
#define LV_USE_GPU_NXP_PXP        0
#define LV_USE_GPU_NXP_VG_LITE    0
#define LV_USE_GPU_SDL            0

/* Logging — route to Serial. */
#define LV_USE_LOG                1
#if LV_USE_LOG
  #define LV_LOG_LEVEL            LV_LOG_LEVEL_WARN
  #define LV_LOG_PRINTF           1
#endif

/* Asserts — keep on during bring-up. */
#define LV_USE_ASSERT_NULL        1
#define LV_USE_ASSERT_MALLOC      1
#define LV_USE_ASSERT_STYLE       0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ         0
#define LV_ASSERT_HANDLER_INCLUDE <stdint.h>
#define LV_ASSERT_HANDLER         while(1);

/* Perf / mem monitors — enabled on hardware, off in the desktop sim so
 * screenshots stay clean. Disable both for production builds. */
#ifdef ESP_PLATFORM
#define LV_USE_PERF_MONITOR       1
#define LV_USE_MEM_MONITOR        0
#else
#define LV_USE_PERF_MONITOR       0
#define LV_USE_MEM_MONITOR        0
#endif
#define LV_USE_REFR_DEBUG         0

/* Misc */
#define LV_SPRINTF_CUSTOM         0
#define LV_SPRINTF_USE_FLOAT      0
#define LV_USE_USER_DATA          1
#define LV_ENABLE_GC              0

/* Compiler */
#define LV_BIG_ENDIAN_SYSTEM      0
#define LV_ATTRIBUTE_TICK_INC
#define LV_ATTRIBUTE_TIMER_HANDLER
#define LV_ATTRIBUTE_FLUSH_READY
#define LV_ATTRIBUTE_MEM_ALIGN_SIZE 4
#define LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_LARGE_CONST
#define LV_ATTRIBUTE_LARGE_RAM_ARRAY
#define LV_ATTRIBUTE_FAST_MEM
#define LV_ATTRIBUTE_DMA
#define LV_EXPORT_CONST_INT(int_value) struct _silence_gcc_warning

/*==================
 *   FONT USAGE
 *===================*/
/* Sizes used by the Ori prototype: 14, 16, 20, 24, 28, 30, 36, 48.
 * M3 enables the larger sizes for headings, profile name, passkey and
 * the after-hours digital clock.
 *
 * Clock face choice: the prototype draws the time at ~170 px. LVGL ships
 * Montserrat up to size 48 as a bundled font; bigger sizes require a
 * custom asset. For M3 we use Montserrat 48 with a 2x transform via
 * lv_obj_set_style_transform_zoom() on the LVGL label, giving us an
 * effective ~96 px glyph height with one font asset and no custom
 * embed step. Visually a touch softer than a native 96 px font; an
 * embedded Inter-Light asset can replace this in M8 polish. */
/* All bundled Montserrat fonts are OFF — we ship our own extended versions
 * (ori_font_montserrat_*) from firmware/src/fonts/ that include U+B7,
 * U+2014, U+2026, etc. Saves Flash by not embedding two copies. */
#define LV_FONT_MONTSERRAT_8  0
#define LV_FONT_MONTSERRAT_10 0
#define LV_FONT_MONTSERRAT_12 0
#define LV_FONT_MONTSERRAT_14 0
#define LV_FONT_MONTSERRAT_16 0
#define LV_FONT_MONTSERRAT_18 0
#define LV_FONT_MONTSERRAT_20 0
#define LV_FONT_MONTSERRAT_22 0
#define LV_FONT_MONTSERRAT_24 0
#define LV_FONT_MONTSERRAT_26 0
#define LV_FONT_MONTSERRAT_28 0
#define LV_FONT_MONTSERRAT_30 0
#define LV_FONT_MONTSERRAT_32 0
#define LV_FONT_MONTSERRAT_34 0
#define LV_FONT_MONTSERRAT_36 0
#define LV_FONT_MONTSERRAT_38 0
#define LV_FONT_MONTSERRAT_40 0
#define LV_FONT_MONTSERRAT_42 0
#define LV_FONT_MONTSERRAT_44 0
#define LV_FONT_MONTSERRAT_46 0
#define LV_FONT_MONTSERRAT_48 0
#define LV_FONT_MONTSERRAT_12_SUBPX 0
#define LV_FONT_MONTSERRAT_28_COMPRESSED 0
#define LV_FONT_DEJAVU_16_PERSIAN_HEBREW 0
#define LV_FONT_SIMSUN_16_CJK            0
#define LV_FONT_UNSCII_8                 0
#define LV_FONT_UNSCII_16                0
/* Ori-branded Montserrat fonts (firmware/src/fonts/) extend the ASCII range
 * with U+B7 (middle dot), U+2014 (em dash), U+2026 (horizontal ellipsis),
 * U+B0 (degree), U+2022 (bullet) — characters used by the prototype that
 * LVGL's bundled ASCII-only Montserrat lacks. Declared here so the symbols
 * are in scope for LV_FONT_DEFAULT and any other lv_conf.h reference. */
#define LV_FONT_CUSTOM_DECLARE \
    LV_FONT_DECLARE(ori_font_montserrat_16) \
    LV_FONT_DECLARE(ori_font_montserrat_20) \
    LV_FONT_DECLARE(ori_font_montserrat_22) \
    LV_FONT_DECLARE(ori_font_montserrat_24) \
    LV_FONT_DECLARE(ori_font_montserrat_28) \
    LV_FONT_DECLARE(ori_font_montserrat_30) \
    LV_FONT_DECLARE(ori_font_montserrat_36) \
    LV_FONT_DECLARE(ori_font_montserrat_42) \
    LV_FONT_DECLARE(ori_font_montserrat_48)

#define LV_FONT_DEFAULT &ori_font_montserrat_20
#define LV_FONT_FMT_TXT_LARGE  0
#define LV_USE_FONT_COMPRESSED 0
#define LV_USE_FONT_SUBPX      0

/*=================
 *  TEXT SETTINGS
 *=================*/
#define LV_TXT_ENC                  LV_TXT_ENC_UTF8
#define LV_TXT_BREAK_CHARS          " ,.;:-_"
#define LV_TXT_LINE_BREAK_LONG_LEN  0
#define LV_TXT_LINE_BREAK_LONG_PRE_MIN_LEN  3
#define LV_TXT_LINE_BREAK_LONG_POST_MIN_LEN 3
#define LV_TXT_COLOR_CMD            "#"
#define LV_USE_BIDI                 0
#define LV_USE_ARABIC_PERSIAN_CHARS 0

/*==================
 *  WIDGET USAGE
 *==================*/
/* All standard widgets on — enabled by default in LVGL 8. M3 will use
 * label, button, image, arc, bar, list, scrollable container. */
#define LV_USE_ARC        1
#define LV_USE_BAR        1
#define LV_USE_BTN        1
#define LV_USE_BTNMATRIX  1
#define LV_USE_CANVAS     1
#define LV_USE_CHECKBOX   1
#define LV_USE_DROPDOWN   1
#define LV_USE_IMG        1
#define LV_USE_LABEL      1
#if LV_USE_LABEL
  #define LV_LABEL_TEXT_SELECTION 0
  #define LV_LABEL_LONG_TXT_HINT  1
#endif
#define LV_USE_LINE       1
#define LV_USE_ROLLER     1
#if LV_USE_ROLLER
  #define LV_ROLLER_INF_PAGES 7
#endif
#define LV_USE_SLIDER     1
#define LV_USE_SWITCH     1
#define LV_USE_TEXTAREA   1
#if LV_USE_TEXTAREA
  #define LV_TEXTAREA_DEF_PWD_SHOW_TIME 1500
#endif
#define LV_USE_TABLE      1

/*==================
 *  EXTRA COMPONENTS
 *==================*/
#define LV_USE_ANIMIMG    1
#define LV_USE_CALENDAR   0
#define LV_USE_CHART      0
#define LV_USE_COLORWHEEL 0
#define LV_USE_IMGBTN     1
#define LV_USE_KEYBOARD   0
#define LV_USE_LED        1
#define LV_USE_LIST       1
#define LV_USE_MENU       0
#define LV_USE_METER      1
#define LV_USE_MSGBOX     1
#define LV_USE_SPAN       1
#if LV_USE_SPAN
  #define LV_SPAN_SNIPPET_STACK_SIZE 64
#endif
#define LV_USE_SPINBOX    0
#define LV_USE_SPINNER    1
#define LV_USE_TABVIEW    0
#define LV_USE_TILEVIEW   0
#define LV_USE_WIN        0

/*==================
 *  THEMES
 *==================*/
#define LV_USE_THEME_DEFAULT 1
#if LV_USE_THEME_DEFAULT
  #define LV_THEME_DEFAULT_DARK 0
  #define LV_THEME_DEFAULT_GROW 1
  #define LV_THEME_DEFAULT_TRANSITION_TIME 80
#endif
#define LV_USE_THEME_BASIC 1
#define LV_USE_THEME_MONO  0

/*==================
 * LAYOUTS
 *==================*/
#define LV_USE_FLEX 1
#define LV_USE_GRID 1

/*==================
 * 3RD PARTS LIBRARIES
 *==================*/
#define LV_USE_FS_STDIO     0
#define LV_USE_FS_POSIX     0
#define LV_USE_FS_WIN32     0
#define LV_USE_FS_FATFS     0
#define LV_USE_PNG          0
#define LV_USE_BMP          0
#define LV_USE_SJPG         0
#define LV_USE_GIF          0
#define LV_USE_QRCODE       0
#define LV_USE_FREETYPE     0
#define LV_USE_TINY_TTF     0
#define LV_USE_RLOTTIE      0
#define LV_USE_FFMPEG       0

/*==================
 * OTHERS
 *==================*/
#define LV_USE_SNAPSHOT    0
#define LV_USE_MONKEY      0
#define LV_USE_GRIDNAV     0
#define LV_USE_FRAGMENT    0
#define LV_USE_IMGFONT     0
#define LV_USE_MSG         0
#define LV_USE_IME_PINYIN  0

/*==================
 * EXAMPLES / DEMOS — off (we build our own UI in M3)
 *==================*/
#define LV_BUILD_EXAMPLES 0
#define LV_USE_DEMO_WIDGETS 0
#define LV_USE_DEMO_KEYPAD_AND_ENCODER 0
#define LV_USE_DEMO_BENCHMARK 0
#define LV_USE_DEMO_STRESS    0
#define LV_USE_DEMO_MUSIC     0

#endif /* LV_CONF_H */
