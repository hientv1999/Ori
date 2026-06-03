# Ori — Hardware

Hardware specs are in `memory.md`. Behavioral rules derived from hardware:

- **Never add any battery UI, and never assume a battery exists.** Ori is USB-C wall-powered. Some units may include an optional LiPo as a brief-blackout backup, but the firmware and UI must behave as if there is none: no indicator, no charging glyph, no low-battery popup, no SoC readout, no "now on battery" badge. The backup exists purely to ride out a momentary USB drop without rebooting; software does not read it, surface it, or alter behavior because of it.
- **No power supervision at firmware level** — no low-power warnings, no shutdown logic, no graceful-shutdown sequence. Pulling the USB-C cable is the off switch. NVS writes are immediate on `SyncControl{op:"END"}` so a yank-the-cable mid-write is safe.
- **USB-C is the firmware-update transport.** Because the cable is always physically attached, firmware updates go over USB CDC only — no USB-MSC fallback (Ori must not appear as a removable drive). See `ota.md`.
- **Expose only the native USB-C port through the enclosure.** The UART debug port (CH343/CP2102) is internal-only — accessible for factory provisioning and bricked-unit recovery but never customer-facing.
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
| Pixel clock | 16 MHz (Waveshare reference; <12 MHz → white screen) |
| Bounce buffer | 800 × 20 px (32 KB) — **hardware artifact fix**, not a perf setting; removes G-channel speckle and left-edge pixel noise caused by PSRAM bandwidth |

**RGB channel pin order:** `Arduino_ESP32RGBPanel` expects **LSB→MSB** (R3/G2/B3 are the *lowest* bits of each channel, not the highest). Getting one channel reversed collapses its mid-tones — on the green channel this shows as mid-greys looking magenta. If you see single-channel color artifacts on hardware, check pin order in `pins.h` before anything else. Do not add per-channel software color correction — RGB565 quantization makes it unworkably sensitive. Use `COLOR_BG = #000000` and accept the panel's native gamma.
