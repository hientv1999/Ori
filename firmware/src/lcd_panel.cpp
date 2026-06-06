#include "lcd_panel.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_cache.h>

#include "io_expander_ch422g.h"
#include "pins.h"

namespace {

constexpr uint16_t LCD_W = 800;
constexpr uint16_t LCD_H = 480;

// Arduino_GFX maintains a full-frame RGB565 framebuffer in PSRAM when
// auto_flush=false. LVGL renders into a PSRAM partial buffer, then flush_area()
// copies each finished rect into this framebuffer. LCD_CAM DMA reads physical
// PSRAM directly (bypasses CPU cache), so after every copy we call
// esp_cache_msync() to force write-back of the updated region from the CPU
// data cache to physical PSRAM.
//
// Framebuffer size: 800 * 480 * 2 = 768,000 bytes (~750 KB).
Arduino_ESP32RGBPanel* bus    = nullptr;
Arduino_RGB_Display*   panel  = nullptr;

} // namespace

namespace lcd_panel {

void init() {
    // --- Panel reset pulse via CH422G EXIO3 --------------------------------
    // LCD_RST is wired to CH422G EXIO3, not a direct GPIO, so Arduino_GFX's
    // Arduino_RGB_Display::begin() has no way to assert it itself. We pulse
    // it here before bringing the RGB bus up to guarantee a clean panel
    // controller state on every boot (cold boot or watchdog reboot). Relies
    // on touch::init() having already brought Wire and the CH422G up.
    ch422g::write_output(ORI_CH422G_EXIO_LCD_RST, false);
    delay(20);
    ch422g::write_output(ORI_CH422G_EXIO_LCD_RST, true);
    delay(20);

    // --- RGB16 parallel bus ------------------------------------------------
    // Constructor signature (Arduino_GFX_Library ~1.4.x):
    //   Arduino_ESP32RGBPanel(DE, VSYNC, HSYNC, PCLK,
    //                         R0..R4, G0..G5, B0..B4,
    //                         hsync_polarity, hsync_front, hsync_pulse, hsync_back,
    //                         vsync_polarity, vsync_front, vsync_pulse, vsync_back,
    //                         pclk_active_neg, prefer_speed, useBigEndian)
    //
    // The R0/G0/B0 names in the library map to our R3/G2/B3 (the low bit of
    // each colour channel on a 16-bit RGB565 bus). Wiring layout matches
    // the Waveshare reference schematic.
    bus = new Arduino_ESP32RGBPanel(
        ORI_LCD_DE_PIN, ORI_LCD_VSYNC_PIN, ORI_LCD_HSYNC_PIN, ORI_LCD_PCLK_PIN,
        // R: low->high bit
        ORI_LCD_R3_PIN, ORI_LCD_R4_PIN, ORI_LCD_R5_PIN, ORI_LCD_R6_PIN, ORI_LCD_R7_PIN,
        // G: low->high bit
        ORI_LCD_G2_PIN, ORI_LCD_G3_PIN, ORI_LCD_G4_PIN, ORI_LCD_G5_PIN, ORI_LCD_G6_PIN, ORI_LCD_G7_PIN,
        // B: low->high bit
        ORI_LCD_B3_PIN, ORI_LCD_B4_PIN, ORI_LCD_B5_PIN, ORI_LCD_B6_PIN, ORI_LCD_B7_PIN,
        ORI_LCD_HSYNC_POLARITY, ORI_LCD_HSYNC_FRONT, ORI_LCD_HSYNC_PULSE, ORI_LCD_HSYNC_BACK,
        ORI_LCD_VSYNC_POLARITY, ORI_LCD_VSYNC_FRONT, ORI_LCD_VSYNC_PULSE, ORI_LCD_VSYNC_BACK,
        ORI_LCD_PCLK_ACTIVE_NEG, ORI_LCD_PCLK_HZ,
        /* useBigEndian */    false,
        /* de_idle_high */    0,
        /* pclk_idle_high */  0,
        /* bounce_buffer_size_px */ 0
    );

    // --- Logical display ---------------------------------------------------
    // Arduino_RGB_Display(width, height, bus, rotation, auto_flush)
    //   auto_flush=false => library keeps an internal PSRAM framebuffer
    //   that we can write into directly and that the LCD_CAM peripheral
    //   continuously scans out via DMA. This is exactly what we want for
    //   LVGL with partial flush.
    panel = new Arduino_RGB_Display(LCD_W, LCD_H, bus, 0 /* rotation */, false /* auto_flush */);

    if (!panel->begin()) {
        Serial.println("[lcd] panel begin() FAILED");
        return;
    }

    panel->fillScreen(0x0000);  // RGB565 black (Arduino_GFX 1.6 dropped the BLACK macro)

    Serial.printf("[lcd] init %ux%u rgb565 fb=%p (psram)\n",
                  (unsigned)LCD_W, (unsigned)LCD_H, panel->getFramebuffer());
}

void* framebuffer() {
    return panel ? (void*)panel->getFramebuffer() : nullptr;
}

uint16_t width()  { return LCD_W; }
uint16_t height() { return LCD_H; }

void flush_area(int16_t x1, int16_t y1, int16_t x2, int16_t y2,
                const uint16_t* pixels) {
    if (!panel) return;
    // draw16bitRGBBitmap() signature is non-const for historical reasons; it
    // does not mutate the source buffer.
    panel->draw16bitRGBBitmap(x1, y1, const_cast<uint16_t*>(pixels),
                              x2 - x1 + 1, y2 - y1 + 1);

    // Force CPU data-cache write-back to physical PSRAM for the updated region.
    // LCD_CAM DMA reads physical PSRAM directly (bypasses CPU cache); without
    // this sync it may read stale pixels, leaving old arc/spinner traces.
    // esp_cache_msync requires both address and size aligned to the cache-line
    // boundary (0x20 = 32 bytes), so we round down the start and round up the
    // size to cover the full affected region.
    {
        constexpr uintptr_t LINE = 0x20;
        uint16_t*  fb       = static_cast<uint16_t*>(panel->getFramebuffer());
        uintptr_t  raw      = reinterpret_cast<uintptr_t>(fb)
                              + (size_t)y1 * LCD_W * sizeof(uint16_t);
        uintptr_t  aligned  = raw & ~(LINE - 1);
        size_t     bytes    = (size_t)(y2 - y1 + 1) * LCD_W * sizeof(uint16_t)
                              + (raw - aligned);
        bytes = (bytes + LINE - 1) & ~(LINE - 1);
        esp_cache_msync(reinterpret_cast<void*>(aligned), bytes,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }
}

} // namespace lcd_panel
