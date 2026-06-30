#pragma once

#include <lvgl.h>

// Ori — boot splash screen.
//
// Shown for the briefest possible window right after boot: just the Ori
// wordmark, centred on COLOR_BG, at 3x the size of the setup flow's wordmark
// (screen_boot_splash.cpp's local make_brand_mark_xl(), a scaled sibling of
// ui::make_brand_mark() — see that function's comment for why a real 90 px
// font is used instead of a transform-scaled 30 px render). main.cpp::setup()
// loads this screen and forces ONE explicit lv_timer_handler() call to flush
// it to the physical LCD before the rest of boot runs — NVS/LittleFS reads,
// photo_cache's PSRAM JPEG decode, and ble_manager::init() are all blocking
// and happen entirely inside setup(), before loop() ever calls
// lv_timer_handler() on its own. Without an explicit early flush the panel
// would otherwise just sit on a plain black screen (lcd_panel::init() clears
// it) for that whole window.
//
// No interaction, no state-machine integration. state_machine::evaluate()
// (called from screen_manager::init(), later in setup()) loads the real
// first screen through the normal auto_del=true screen-load path
// (state_machine.cpp::load_screen()), which deletes whatever was the active
// screen — this splash — automatically. Nothing here needs explicit cleanup.

namespace screen_boot_splash {

lv_obj_t* create();

} // namespace screen_boot_splash
