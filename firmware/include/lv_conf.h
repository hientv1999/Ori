/**
 * @file lv_conf.h
 * LVGL 9.x configuration for Ori (Waveshare ESP32-S3 4.3" Touch LCD).
 *
 * Picked up via -DLV_CONF_INCLUDE_SIMPLE and -DLV_CONF_PATH in platformio.ini.
 *
 * Tuning notes:
 *  - Panel is 800x480 RGB565.
 *  - Framebuffer lives in PSRAM (lcd_panel.cpp). LVGL partial-renders into a
 *    60-line PSRAM draw buffer and flushes completed rects via lcd_panel::flush_area().
 *  - Tick: lv_tick_set_cb(millis) called in main.cpp after lv_init().
 *  - Custom log: routed to Serial.
 */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/*====================
   COLOR SETTINGS
 *====================*/
/* RGB565 — matches the Waveshare panel's native pixel format. */
#define LV_COLOR_DEPTH        16

/*=========================
   STDLIB WRAPPER SETTINGS
 *=========================*/
/* Firmware: route LVGL's heap to 8 MB PSRAM via custom allocator.
 * Desktop sim: use LVGL's built-in pool. */
#ifdef ESP_PLATFORM
#define LV_USE_STDLIB_MALLOC    LV_STDLIB_CUSTOM
#include "lv_mem_custom.h"          /* declares lv_psram_malloc/realloc/free */
#define lv_malloc_core          lv_psram_malloc
#define lv_realloc_core         lv_psram_realloc
#define lv_free_core            lv_psram_free
#else
#define LV_USE_STDLIB_MALLOC    LV_STDLIB_BUILTIN
#ifndef LV_MEM_SIZE
#define LV_MEM_SIZE             (2097152U)   /* 2 MB — desktop sim override */
#endif
#endif

#define LV_USE_STDLIB_STRING    LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_BUILTIN

/*====================
   HAL SETTINGS
 *====================*/
/* ~60 Hz refresh. Covers display refresh, indev read, and animation step. */
#define LV_DEF_REFR_PERIOD    16    /* ms */
#define LV_DPI_DEF            130

/*=================
 * OPERATING SYSTEM
 *=================*/
/* Single-threaded Arduino — no RTOS. */
#define LV_USE_OS             LV_OS_NONE

/*========================
 * RENDERING CONFIGURATION
 *========================*/
#define LV_DRAW_BUF_STRIDE_ALIGN    1
#define LV_DRAW_BUF_ALIGN           4

/* Layers: budget for simple-layer chunks (rounded corners, shadow, opacity < 255). */
#define LV_DRAW_LAYER_SIMPLE_BUF_SIZE    (24U * 1024U)

/* Software renderer — the only renderer used on this ESP32-S3. */
#define LV_USE_DRAW_SW      1
#if LV_USE_DRAW_SW == 1
    /* Single draw unit (no OS / no multi-thread). */
    #define LV_DRAW_SW_DRAW_UNIT_CNT    1
    #define LV_USE_DRAW_SW_ASM          LV_DRAW_SW_ASM_NONE
    #define LV_USE_DRAW_ARM2D_SYNC      0
    #define LV_USE_NATIVE_HELIUM_ASM    0
    /* Complex renderer: rounded corners, shadow, arcs. Required by Ori UI. */
    #define LV_DRAW_SW_COMPLEX          1
    #if LV_DRAW_SW_COMPLEX == 1
        #define LV_DRAW_SW_SHADOW_CACHE_SIZE    0
        #define LV_DRAW_SW_CIRCLE_CACHE_SIZE    4
    #endif
    #define LV_USE_DRAW_SW_ASM          LV_DRAW_SW_ASM_NONE
#endif

/* All GPU / accelerator back-ends off. */
#define LV_USE_DRAW_VGLITE    0
#define LV_USE_DRAW_PXP       0
#define LV_USE_DRAW_DAVE2D    0
#define LV_USE_DRAW_SDL       0
#define LV_USE_DRAW_VG_LITE   0

/*=======================
 * FEATURE CONFIGURATION
 *======================*/

/*-------------
 * Logging
 *-----------*/
#define LV_USE_LOG              1
#if LV_USE_LOG
    #define LV_LOG_LEVEL        LV_LOG_LEVEL_WARN
    #define LV_LOG_PRINTF       1
    #define LV_LOG_USE_TIMESTAMP    0
    #define LV_LOG_USE_FILE_LINE    0
#endif

/*-------------
 * Asserts
 *-----------*/
#define LV_USE_ASSERT_NULL          1
#define LV_USE_ASSERT_MALLOC        1
#define LV_USE_ASSERT_STYLE         0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ           0
#define LV_ASSERT_HANDLER_INCLUDE   <stdint.h>
#define LV_ASSERT_HANDLER           while(1);

/*-------------
 * Debug overlays — off in production
 *-----------*/
#define LV_USE_REFR_DEBUG           0
#define LV_USE_LAYER_DEBUG          0
#define LV_USE_PARALLEL_DRAW_DEBUG  0

/*-------------
 * Perf / mem monitors
 *-----------*/
/* LV_USE_SYSMON is the LVGL 9 parent gate for perf / mem monitors. */
#ifdef ESP_PLATFORM
#define LV_USE_SYSMON           1
#else
#define LV_USE_SYSMON           0
#endif
#if LV_USE_SYSMON
    #define LV_SYSMON_GET_IDLE      lv_timer_get_idle
    #define LV_USE_PERF_MONITOR     1
    #if LV_USE_PERF_MONITOR
        #define LV_USE_PERF_MONITOR_POS         LV_ALIGN_BOTTOM_RIGHT
        #define LV_USE_PERF_MONITOR_LOG_MODE    0
    #endif
    #define LV_USE_MEM_MONITOR      0
#endif

/*-------------
 * Misc
 *-----------*/
#define LV_ENABLE_GLOBAL_CUSTOM     0
#define LV_CACHE_DEF_SIZE           0
#define LV_IMAGE_HEADER_CACHE_DEF_CNT   0
#define LV_GRADIENT_MAX_STOPS       2
#define LV_COLOR_MIX_ROUND_OFS      0
#define LV_OBJ_STYLE_CACHE          0
#define LV_USE_OBJ_ID               0
#define LV_USE_OBJ_ID_BUILTIN       0
#define LV_USE_OBJ_PROPERTY         0
#define LV_USE_VG_LITE_THORVG       0

/*=====================
 *  COMPILER SETTINGS
 *====================*/
#define LV_BIG_ENDIAN_SYSTEM        0
#define LV_ATTRIBUTE_TICK_INC
#define LV_ATTRIBUTE_TIMER_HANDLER
#define LV_ATTRIBUTE_FLUSH_READY
#define LV_ATTRIBUTE_MEM_ALIGN_SIZE 4
#define LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_LARGE_CONST  __attribute__((section(".rodata")))
#define LV_ATTRIBUTE_LARGE_RAM_ARRAY
/* Putting LVGL's hot paths in IRAM saves ~10-15% render time on ESP32. */
#define LV_ATTRIBUTE_FAST_MEM       IRAM_ATTR
#define LV_EXPORT_CONST_INT(int_value) struct _silence_gcc_warning
#define LV_ATTRIBUTE_EXTERN_DATA
#define LV_USE_FLOAT                0

/*==================
 *   FONT USAGE
 *===================*/
/* All bundled Montserrat fonts are OFF — Ori ships Hanken Grotesk Medium fonts
 * (ori_font_hanken_*) from firmware/src/fonts/ that cover ASCII + U+B0 (°),
 * U+B7 (·), U+2013 (–), U+2014 (—), U+2022 (•), U+2026 (…). */
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
#define LV_FONT_MONTSERRAT_28_COMPRESSED 0
#define LV_FONT_DEJAVU_16_PERSIAN_HEBREW 0
#define LV_FONT_SIMSUN_16_CJK            0
#define LV_FONT_UNSCII_8                 0
#define LV_FONT_UNSCII_16                0

#define LV_FONT_CUSTOM_DECLARE \
    LV_FONT_DECLARE(ori_font_hanken_20) \
    LV_FONT_DECLARE(ori_font_hanken_24) \
    LV_FONT_DECLARE(ori_font_hanken_26) \
    LV_FONT_DECLARE(ori_font_hanken_28) \
    LV_FONT_DECLARE(ori_font_hanken_30) \
    LV_FONT_DECLARE(ori_font_hanken_42) \
    LV_FONT_DECLARE(ori_font_hanken_48)

#define LV_FONT_DEFAULT         &ori_font_hanken_20
#define LV_FONT_FMT_TXT_LARGE   0
#define LV_USE_FONT_COMPRESSED  0
#define LV_USE_FONT_PLACEHOLDER 1

/*=================
 *  TEXT SETTINGS
 *=================*/
#define LV_TXT_ENC                      LV_TXT_ENC_UTF8
#define LV_TXT_BREAK_CHARS              " ,.;:-_"
#define LV_TXT_LINE_BREAK_LONG_LEN      0
#define LV_TXT_LINE_BREAK_LONG_PRE_MIN_LEN  3
#define LV_TXT_LINE_BREAK_LONG_POST_MIN_LEN 3
#define LV_USE_BIDI                     0
#define LV_USE_ARABIC_PERSIAN_CHARS     0

/*==================
 *  WIDGET USAGE
 *==================*/
/* LVGL 9 renames: BTN→BUTTON, BTNMATRIX→BUTTONMATRIX, IMG→IMAGE, IMGBTN→IMAGEBUTTON */
#define LV_WIDGETS_HAS_DEFAULT_VALUE    1
#define LV_USE_ARC          1
#define LV_USE_BAR          1
#define LV_USE_BUTTON       1
#define LV_USE_BUTTONMATRIX 1
#define LV_USE_CANVAS       1
#define LV_USE_CHECKBOX     1
#define LV_USE_DROPDOWN     1
#define LV_USE_IMAGE        1
#define LV_USE_IMAGEBUTTON  1
#define LV_USE_LABEL        1
#if LV_USE_LABEL
    #define LV_LABEL_TEXT_SELECTION     0
    #define LV_LABEL_LONG_TXT_HINT      1
    #define LV_LABEL_WAIT_CHAR_COUNT    3
#endif
#define LV_USE_LED          1
#define LV_USE_LINE         1
#define LV_USE_ROLLER       1
#define LV_USE_SCALE        1
#define LV_USE_SLIDER       1
#define LV_USE_SPAN         1
#if LV_USE_SPAN
    #define LV_SPAN_SNIPPET_STACK_SIZE  64
#endif
#define LV_USE_SPINNER      1
#define LV_USE_SWITCH       1
#define LV_USE_TEXTAREA     1
#if LV_USE_TEXTAREA
    #define LV_TEXTAREA_DEF_PWD_SHOW_TIME 1500
#endif
#define LV_USE_TABLE        1
#define LV_USE_ANIMIMG      1
#define LV_USE_CALENDAR     0
#define LV_USE_CHART        0
#define LV_USE_KEYBOARD     0
#define LV_USE_LIST         1
#define LV_USE_MENU         0
#define LV_USE_MSGBOX       1
#define LV_USE_SPINBOX      0
#define LV_USE_TABVIEW      0
#define LV_USE_TILEVIEW     0
#define LV_USE_WIN          0

/*==================
 *  THEMES
 *==================*/
#define LV_USE_THEME_DEFAULT 1
#if LV_USE_THEME_DEFAULT
    #define LV_THEME_DEFAULT_DARK               0
    #define LV_THEME_DEFAULT_GROW               1
    #define LV_THEME_DEFAULT_TRANSITION_TIME    80
#endif
/* LVGL 9 renamed THEME_BASIC → THEME_SIMPLE */
#define LV_USE_THEME_SIMPLE     1
#define LV_USE_THEME_MONO       0

/*==================
 * LAYOUTS
 *==================*/
#define LV_USE_FLEX     1
#define LV_USE_GRID     1

/*==================
 * 3RD PARTY LIBRARIES
 *==================*/
#define LV_USE_FS_STDIO     0
#define LV_USE_FS_POSIX     0
#define LV_USE_FS_WIN32     0
#define LV_USE_FS_FATFS     0
#define LV_USE_FS_MEMFS     0
#define LV_USE_LODEPNG      0
#define LV_USE_LIBPNG       0
#define LV_USE_BMP          0
/* LVGL 9 renamed SJPG → TJPGD. Enabled at M5 for album-art JPEG decoding. */
#define LV_USE_TJPGD        1
#define LV_USE_LIBJPEG_TURBO 0
#define LV_USE_GIF          0
#define LV_BIN_DECODER_RAM_LOAD 0
#define LV_USE_RLE          0
#define LV_USE_QRCODE       0
#define LV_USE_BARCODE      0
#define LV_USE_FREETYPE     0
#define LV_USE_TINY_TTF     0
#define LV_USE_RLOTTIE      0
#define LV_USE_VECTOR_GRAPHIC   0
#define LV_USE_THORVG_INTERNAL  0
#define LV_USE_THORVG_EXTERNAL  0
#define LV_USE_LZ4_INTERNAL     0
#define LV_USE_LZ4_EXTERNAL     0
#define LV_USE_FFMPEG           0

/*==================
 * OTHERS
 *==================*/
#define LV_USE_SNAPSHOT     0
#define LV_USE_PROFILER     0
#define LV_USE_MONKEY       0
#define LV_USE_GRIDNAV      0
#define LV_USE_FRAGMENT     0
#define LV_USE_IMGFONT      0
#define LV_USE_OBSERVER     1
#define LV_USE_IME_PINYIN   0
#define LV_USE_FILE_EXPLORER 0

/*==================
 * DEVICES (all off — using custom Arduino_GFX driver)
 *==================*/
#define LV_USE_SDL          0
#define LV_USE_X11          0
#define LV_USE_LINUX_FBDEV  0
#define LV_USE_NUTTX        0
#define LV_USE_LINUX_DRM    0
#define LV_USE_TFT_ESPI     0
#define LV_USE_EVDEV        0
#define LV_USE_LIBINPUT     0
#define LV_USE_ST7735       0
#define LV_USE_ST7789       0
#define LV_USE_ST7796       0
#define LV_USE_ILI9341      0
#define LV_USE_WINDOWS      0

/*==================
 * EXAMPLES / DEMOS — off (Ori has its own UI)
 *==================*/
#define LV_BUILD_EXAMPLES               0
#define LV_USE_DEMO_WIDGETS             0
#define LV_USE_DEMO_KEYPAD_AND_ENCODER  0
#define LV_USE_DEMO_BENCHMARK           0
#define LV_USE_DEMO_RENDER              0
#define LV_USE_DEMO_STRESS              0
#define LV_USE_DEMO_MUSIC               0
#define LV_USE_DEMO_FLEX_LAYOUT         0
#define LV_USE_DEMO_MULTILANG           0
#define LV_USE_DEMO_TRANSFORM           0
#define LV_USE_DEMO_SCROLL              0
#define LV_USE_DEMO_VECTOR_GRAPHIC      0

#endif /* LV_CONF_H */
