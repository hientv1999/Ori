#pragma once

// LVGL display driver registration for the 800x480 RGB565 panel.
//
// Call AFTER lcd_panel::init() and lv_init(). Allocates a small partial
// draw buffer in internal SRAM and registers an lv_disp_drv_t whose flush
// callback streams the dirty rectangle into the PSRAM framebuffer owned
// by lcd_panel.cpp.

namespace lvgl_display {

void init();

} // namespace lvgl_display
