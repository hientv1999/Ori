#pragma once

// Single source of truth for every GPIO assignment on Ori.
//
// Target board: Waveshare ESP32-S3 Touch LCD 4.3"
// (ESP32-S3-WROOM-1, 16 MB flash, 8 MB OPI PSRAM, 800x480 RGB565 panel,
//  GT911 capacitive touch, CH422G I/O expander on I2C bus 0x24).
//
// Every pin may be overridden at build time via a -D flag in platformio.ini.

// --------------------------------------------------------------------------
// Waveshare ESP32-S3-Touch-LCD-4.3" pin map
// --------------------------------------------------------------------------
// Three board-critical signals are NOT direct ESP32 GPIOs — they live on a
// CH422G 8-bit I²C I/O expander that shares the touch I²C bus with the
// GT911. The CH422G is digital-only (no PWM):
//
//   EXIO1 -> TP_RST   (GT911 reset)
//   EXIO2 -> LCD_BL   (backlight enable; also tied to the panel DISP signal,
//                      so the panel does not run with this low)
//   EXIO3 -> LCD_RST  (RGB panel reset)
//
// The GPIOs you might intuitively expect to find these on (GPIO 2, GPIO 38)
// are consumed by the RGB16 parallel panel as R4 and B4. There is no free
// ESP32 GPIO wired to the backlight LED driver — the only path is through
// CH422G EXIO2 (digital-only). Backlight is always ON; no runtime control.
// If a future hardware revision introduces a real PWM line, only backlight.cpp
// needs to change.

// ----- CH422G I/O expander (shared with GT911 on the touch I²C bus) -------
#ifndef ORI_CH422G_I2C_ADDR
#define ORI_CH422G_I2C_ADDR 0x24
#endif

// EXIOn assignments. Constant names, not macros, so the driver is type-safe.
// Defined as macros to keep -D override flexibility consistent with the rest
// of this header.
#ifndef ORI_CH422G_EXIO_TP_RST
#define ORI_CH422G_EXIO_TP_RST  1
#endif
#ifndef ORI_CH422G_EXIO_LCD_BL
#define ORI_CH422G_EXIO_LCD_BL  2
#endif
#ifndef ORI_CH422G_EXIO_LCD_RST
#define ORI_CH422G_EXIO_LCD_RST 3
#endif

// ----- GT911 capacitive touch ---------------------------------------------
// SDA/SCL/INT are direct ESP32 GPIOs. TP_RST is on CH422G EXIO1 — see above.
#ifndef ORI_TOUCH_SDA_PIN
#define ORI_TOUCH_SDA_PIN 8
#endif
#ifndef ORI_TOUCH_SCL_PIN
#define ORI_TOUCH_SCL_PIN 9
#endif
#ifndef ORI_TOUCH_INT_PIN
#define ORI_TOUCH_INT_PIN 4
#endif
#ifndef ORI_TOUCH_I2C_ADDR
#define ORI_TOUCH_I2C_ADDR 0x5D
#endif

// ----- LCD panel (RGB16 parallel via ESP32-S3 LCD_CAM peripheral) ---------
// Waveshare ESP32-S3-Touch-LCD-4.3" reference design. Each macro can be
// overridden at build time. These feed Arduino_GFX_Library's
// Arduino_ESP32RGBPanel constructor in lcd_panel.cpp.
//
// Sync + clock
#ifndef ORI_LCD_PCLK_PIN
#define ORI_LCD_PCLK_PIN  7
#endif
#ifndef ORI_LCD_HSYNC_PIN
#define ORI_LCD_HSYNC_PIN 46
#endif
#ifndef ORI_LCD_VSYNC_PIN
#define ORI_LCD_VSYNC_PIN 3
#endif
#ifndef ORI_LCD_DE_PIN
#define ORI_LCD_DE_PIN    5
#endif

// Data lines — RGB565 packed into the high bits of the 16-bit bus.
// R3..R7 (5 bits)
#ifndef ORI_LCD_R3_PIN
#define ORI_LCD_R3_PIN  1
#endif
#ifndef ORI_LCD_R4_PIN
#define ORI_LCD_R4_PIN  2
#endif
#ifndef ORI_LCD_R5_PIN
#define ORI_LCD_R5_PIN  42
#endif
#ifndef ORI_LCD_R6_PIN
#define ORI_LCD_R6_PIN  41
#endif
#ifndef ORI_LCD_R7_PIN
#define ORI_LCD_R7_PIN  40
#endif

// G2..G7 (6 bits) — panel pin G2 = LSB, G7 = MSB. Arduino_GFX's constructor
// expects the green-channel arguments in LSB→MSB order, which means the GPIO
// assigned to ORI_LCD_G2_PIN must be the GPIO physically wired to the panel's
// G2 (lowest green bit), not the highest.
//
// Verified against the Waveshare ESP32-S3-Touch-LCD-4.3 reference schematic
// after observing on hardware that every mid-tone green collapsed to ~0 while
// pure green and pure black both rendered correctly — the textbook signature
// of a fully reversed bit order on a single colour channel. (Magenta-looking
// mid-greys, pure-blue cyan, pure-red yellow.)
#ifndef ORI_LCD_G2_PIN
#define ORI_LCD_G2_PIN  39
#endif
#ifndef ORI_LCD_G3_PIN
#define ORI_LCD_G3_PIN  0
#endif
#ifndef ORI_LCD_G4_PIN
#define ORI_LCD_G4_PIN  45
#endif
#ifndef ORI_LCD_G5_PIN
#define ORI_LCD_G5_PIN  48
#endif
#ifndef ORI_LCD_G6_PIN
#define ORI_LCD_G6_PIN  47
#endif
#ifndef ORI_LCD_G7_PIN
#define ORI_LCD_G7_PIN  21
#endif

// B3..B7 (5 bits)
#ifndef ORI_LCD_B3_PIN
#define ORI_LCD_B3_PIN  14
#endif
#ifndef ORI_LCD_B4_PIN
#define ORI_LCD_B4_PIN  38
#endif
#ifndef ORI_LCD_B5_PIN
#define ORI_LCD_B5_PIN  18
#endif
#ifndef ORI_LCD_B6_PIN
#define ORI_LCD_B6_PIN  17
#endif
#ifndef ORI_LCD_B7_PIN
#define ORI_LCD_B7_PIN  10
#endif

// Panel timing — Waveshare 4.3" 800x480 reference values.
// HSYNC/VSYNC active-low; DE active-high; PCLK active-edge falling.
#ifndef ORI_LCD_PCLK_HZ
// Pixel clock. 16 MHz is the Waveshare reference value. Going below ~12 MHz
// caused a white screen on this board (likely below the panel's minimum
// sampling clock).
#define ORI_LCD_PCLK_HZ          12000000   // 12 MHz pixel clock (most stable on this panel)
#endif

#ifndef ORI_LCD_HSYNC_POLARITY
#define ORI_LCD_HSYNC_POLARITY   0          // 0 = active low
#endif
#ifndef ORI_LCD_HSYNC_FRONT
#define ORI_LCD_HSYNC_FRONT      8
#endif
#ifndef ORI_LCD_HSYNC_PULSE
#define ORI_LCD_HSYNC_PULSE      4
#endif
#ifndef ORI_LCD_HSYNC_BACK
#define ORI_LCD_HSYNC_BACK       8
#endif
#ifndef ORI_LCD_VSYNC_POLARITY
#define ORI_LCD_VSYNC_POLARITY   0          // 0 = active low
#endif
#ifndef ORI_LCD_VSYNC_FRONT
#define ORI_LCD_VSYNC_FRONT      8
#endif
#ifndef ORI_LCD_VSYNC_PULSE
#define ORI_LCD_VSYNC_PULSE      4
#endif
#ifndef ORI_LCD_VSYNC_BACK
#define ORI_LCD_VSYNC_BACK       8
#endif
#ifndef ORI_LCD_PCLK_ACTIVE_NEG
#define ORI_LCD_PCLK_ACTIVE_NEG  1          // 1 = data latched on falling edge
#endif
