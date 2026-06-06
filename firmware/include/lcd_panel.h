#pragma once
#include <stdint.h>

// 800x480 RGB565 parallel panel bring-up for the Waveshare ESP32-S3
// 4.3" Touch LCD. Owned by the esp32-lvgl agent (M2 L1).
//
// Implementation uses Arduino_GFX_Library's Arduino_ESP32RGBPanel +
// Arduino_RGB_Display classes so we stay on the Arduino framework — no
// raw esp_lcd_* / ESP-IDF calls.
//
// The library allocates the framebuffer in PSRAM (auto_flush=false). LVGL
// renders into a PSRAM partial buffer and copies finished rectangles into the
// framebuffer via flush_area(); the LCD_CAM peripheral DMA-scans it continuously.

namespace lcd_panel {

void     init();         // brings up the panel; framebuffer in PSRAM
void*    framebuffer();  // raw pointer to RGB565 framebuffer (width*height*2 bytes)
uint16_t width();        // 800
uint16_t height();       // 480

void     flush_area(int16_t x1, int16_t y1, int16_t x2, int16_t y2,
                    const uint16_t* pixels);

} // namespace lcd_panel
