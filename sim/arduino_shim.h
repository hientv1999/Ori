#pragma once

// Minimal Arduino API stubs for the LVGL desktop simulator.
//
// The simulator only links the LVGL-pure source files (screens, widgets,
// theme, mock_data). It deliberately does NOT link the hardware modules
// (backlight, touch_gt911, nvs_store, io_expander_ch422g, lcd_panel,
// lvgl_display, lvgl_input, main.cpp). The few Arduino-specific symbols
// that leak into the LVGL-pure code (only `millis()` is required because
// lv_conf.h's LV_TICK_CUSTOM_SYS_TIME_EXPR uses it) are stubbed here.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Wall-clock milliseconds since program start. Backs LVGL's LV_TICK_CUSTOM
// path so we don't need lv_tick_inc().
uint32_t millis(void);

#ifdef __cplusplus
}
#endif

// LVGL's lv_conf.h sets LV_TICK_CUSTOM_INCLUDE = "Arduino.h". We point the
// preprocessor at this shim instead via -include and -D overrides in the
// Makefile, so this header gets dragged in transitively.
