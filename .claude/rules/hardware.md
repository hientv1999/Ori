# Ori — Hardware

Hardware specs are in `memory.md`. Behavioral rules derived from hardware:

- **Never add any battery UI, and never assume a battery exists.** Ori is USB-C wall-powered. Some units may include an optional LiPo as a brief-blackout backup, but firmware/UI must behave as if there is none: no indicator, no charging glyph, no low-battery popup, no SoC readout. The backup exists purely to ride out a momentary USB drop without rebooting; software never reads or surfaces it.
- **No power supervision at firmware level** — no low-power warnings, no shutdown logic. Pulling the USB-C cable is the off switch. NVS writes are immediate on `SyncControl{op:"END"}` so a yank-the-cable mid-write is safe.
- **USB-C carries power only — it is not a data path.** Firmware updates go over BLE (`ota.md`); there is no USB CDC update path and no USB-MSC (Ori must not appear as a removable drive). Do not add a customer-facing USB data feature: the enclosure does not expose a reachable data port, so anything built on one is unreachable in the field.
- **No port is customer-accessible.** The UART debug header (CH343/CP2102) is internal-only — factory provisioning and bricked-unit recovery, never customer-facing. Since BLE is now the only update transport, this header is also the *only* way back from a build that boots healthily but has broken BLE; see `ota.md`'s recovery note for why that makes "BLE comes up" a release-blocking smoke test.
- The GT911 supports up to 5 simultaneous touch points. No current feature uses more than one touch point.

## GPIO Pin Map — Waveshare ESP32-S3 Touch LCD 4.3"

**Single source of truth: [firmware/include/pins.h](../../firmware/include/pins.h).** Every GPIO assignment lives there; never hardcode pins elsewhere.

| Bus / signal | Pin |
|---|---|
| Touch I²C SDA / SCL / INT | GPIO 8 / 9 / 4 |
| CH422G expander I²C addr | `0x24` (shares the touch I²C bus) |
| GT911 reset (`TP_RST`) | CH422G **EXIO1** |
| Backlight enable (`LCD_BL`) | CH422G **EXIO2** (digital only — always ON; no PWM) |
| LCD reset (`LCD_RST`) | CH422G **EXIO3** |
| LCD `PCLK` / `HSYNC` / `VSYNC` / `DE` | GPIO 7 / 46 / 3 / 5 |
| LCD `R3..R7` (5 bits, **LSB→MSB**) | GPIO 1, 2, 42, 41, 40 |
| LCD `G2..G7` (6 bits, **LSB→MSB**) | GPIO 39, 0, 45, 48, 47, 21 |
| LCD `B3..B7` (5 bits, **LSB→MSB**) | GPIO 14, 38, 18, 17, 10 |
| Pixel clock | 12 MHz (`ORI_LCD_PCLK_HZ` in `pins.h`; most stable on this panel — 14/16 MHz tested and reverted) |
| Bounce buffer | **removed** — replaced by `esp_cache_msync` after each framebuffer write (see below) |

## PSRAM + LCD_CAM cache coherency rule

**After every CPU write to the PSRAM framebuffer, call `esp_cache_msync(region, bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M)`.** LCD_CAM DMA reads physical PSRAM directly, bypassing the CPU data cache. Without the flush, LCD_CAM reads stale pixels — the symptom is old widget positions persisting on screen (e.g. spinner arc traces). The bounce buffer previously masked this by routing LCD_CAM reads through SRAM; with it removed the explicit sync is mandatory.

- Cache line size on this hardware: **32 bytes (0x20)**
- Both start address and byte count must be 32-byte aligned — round down the start, round up the size
- Reference implementation: `lcd_panel::flush_area()` in `firmware/src/lcd_panel.cpp`
- NVS flash writes also disable ICache/DCache briefly; keeping the LVGL draw buffer in PSRAM (not static SRAM) avoids a "cache disabled but cached memory region accessed" crash during NVS operations

**RGB channel pin order:** `Arduino_ESP32RGBPanel` expects **LSB→MSB** (R3/G2/B3 are the *lowest* bits of each channel, not the highest). Getting one channel reversed collapses its mid-tones — on the green channel this shows as mid-greys looking magenta. If you see single-channel color artifacts on hardware, check pin order in `pins.h` before anything else. Do not add per-channel software color correction — RGB565 quantization makes it unworkably sensitive. Use `COLOR_BG = #000000` and accept the panel's native gamma.
