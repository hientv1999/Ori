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
// renders into a PSRAM draw buffer, then flush_area() copies finished
// rectangles into the framebuffer; the LCD_CAM peripheral DMA-scans it
// continuously.

namespace lcd_panel {

void     init();         // brings up the panel; framebuffer in PSRAM

// stop — gate + reset the LCD_CAM peripheral, halting the RGB DMA scan-out.
// Used for the firmware-update flash commit only. There is intentionally no
// resume() — every outcome past that point reboots, which re-inits the panel.
void     stop();

// blackout — fill the entire framebuffer with black and flush the CPU cache so
// LCD_CAM DMA picks up the solid-black frame immediately. LCD_CAM keeps running
// (no hardware disruption). Call this before a flash write burst (NVS / LittleFS)
// to prevent rendering glitches while the commit is in progress. The display
// "comes back" automatically when LVGL's next flush_area() writes new pixels.
void     blackout();

uint16_t width();        // 800
uint16_t height();       // 480

// sync_area — flush CPU data-cache to physical PSRAM for the given rectangle
// so LCD_CAM DMA sees the pixels LVGL just rendered.  No pixel copy occurs.
void     sync_area(int16_t x1, int16_t y1, int16_t x2, int16_t y2);

// flush_area — copy pixels from draw_buf into the framebuffer for the given
// rectangle, then sync the CPU cache to physical PSRAM for LCD_CAM DMA.
void     flush_area(int16_t x1, int16_t y1, int16_t x2, int16_t y2,
                    const uint16_t* pixels);

} // namespace lcd_panel
