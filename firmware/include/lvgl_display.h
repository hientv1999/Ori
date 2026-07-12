#pragma once

// LVGL display driver registration for the 800x480 RGB565 panel.
//
// Call AFTER lcd_panel::init() and lv_init(). Allocates a full-frame draw
// buffer in PSRAM and registers a display whose flush callback copies each
// finished dirty rectangle into the framebuffer owned by lcd_panel.cpp.

namespace lvgl_display {

void init();

} // namespace lvgl_display
