# Ori — Hardware

Hardware specs are in `memory.md`. Behavioral rules derived from hardware:

- **Never add any battery UI, and never assume a battery exists.** Ori is USB-C wall-powered. Some units may include an optional LiPo as a brief-blackout backup, but the firmware and UI must behave as if there is none: no indicator, no charging glyph, no low-battery popup, no SoC readout, no "now on battery" badge. The backup exists purely to ride out a momentary USB drop without rebooting; software does not read it, surface it, or alter behavior because of it.
- **No power supervision at firmware level** — no low-power warnings, no shutdown logic, no graceful-shutdown sequence. Even on units that ship with the optional battery, the firmware never branches on battery state. Pulling the USB-C cable is the off switch (the battery, when present, just makes that off-switch slightly forgiving). NVS writes are debounced (~2 s for backlight; immediate on `SyncControl{op:"END"}`) so a yank-the-cable mid-write is safe, not catastrophic, on either hardware variant.
- **USB-C is the firmware-update transport.** Because the cable is always physically attached (it's also the power line), firmware updates go over USB CDC only — no USB-MSC fallback, because Ori must not appear as a removable drive in File Explorer / Finder. See `ota.md`. BLE OTA was also removed from the protocol in v1.1 — see `ble-protocol.md` changelog.
- **Expose only the native USB-C port through the enclosure.** Keep the UART debug port (CH343/CP2102) accessible internally for factory provisioning and bricked-unit recovery, but it must not be customer-facing. UART can't carry the USB CDC update path that ships in `ota.md`; the UART port is the service-only recovery path when the enclosure can be opened.
- The GT911 supports up to 5 simultaneous touch points. The only feature that uses more than one touch point is the two-finger backlight gesture.

## GPIO Pin Map — Waveshare ESP32-S3 Touch LCD 4.3"

**Single source of truth: [firmware/include/pins.h](../../firmware/include/pins.h).** Every GPIO assignment on Ori lives there, and every driver pulls from it via the `ORI_*_PIN` macros. If a pin assignment is wrong on hardware, change `pins.h` — never hardcode pins anywhere else.

### Verified pin assignments (Waveshare ESP32-S3-Touch-LCD-4.3", confirmed on hardware)

| Bus / signal | Pin |
|---|---|
| Touch I²C SDA / SCL / INT | GPIO 8 / 9 / 4 |
| CH422G expander I²C addr | `0x24` (shares the touch I²C bus) |
| GT911 reset (`TP_RST`) | CH422G **EXIO1** |
| Backlight enable (`LCD_BL`) | CH422G **EXIO2** (digital only — no PWM possible) |
| LCD reset (`LCD_RST`) | CH422G **EXIO3** |
| LCD `PCLK` / `HSYNC` / `VSYNC` / `DE` | GPIO 7 / 46 / 3 / 5 |
| LCD `R3..R7` (5 bits, LSB→MSB) | GPIO 1, 2, 42, 41, 40 |
| LCD `G2..G7` (6 bits, LSB→MSB) | GPIO 39, 0, 45, 48, 47, 21 |
| LCD `B3..B7` (5 bits, LSB→MSB) | GPIO 14, 38, 18, 17, 10 |
| Pixel clock | 16 MHz (Waveshare reference; <12 MHz → white screen) |
| Bounce buffer | 800 × 20 px (32 KB) — required to keep PSRAM bandwidth ahead of the LCD DMA |

### Important pin-order rule — easy to get wrong

`Arduino_ESP32RGBPanel`'s constructor expects each color-channel's GPIOs in **LSB→MSB order**. That means the GPIO listed under `ORI_LCD_G2_PIN` must be the GPIO physically wired to the panel's G2 line (the lowest green bit), not the highest. Same for R3 and B3.

Symptom of getting this reversed on a single channel: that channel's mid-tones collapse to ~0 while pure-channel and pure-black both render correctly. On the green channel specifically, this shows up as mid-greys looking magenta, "cyan" looking pure blue, and "yellow" looking pure red. If you see this on hardware, do not start tuning a software color calibration — verify pin order in `pins.h` first.

### Things that are NOT calibration

These were real hardware fixes and must stay in place:

- **Green channel pin order in `pins.h`** (above) — the canonical fix for the green-collapse artifact.
- **Bounce buffer (`ORI_LCD_BOUNCE_BUF_PX`)** wired into the `Arduino_ESP32RGBPanel` constructor in `firmware/src/lcd_panel.cpp`. Fixes PSRAM-bandwidth artifacts (G-channel speckle, left-edge pixel noise).
- **Platform upgrade** to pioarduino (Arduino-ESP32 3.x) + Arduino_GFX ^1.6.0 in `firmware/platformio.ini`. Required to unlock the `bounce_buffer_size_px` constructor argument.

Software per-channel color calibration was tried and rejected (RGB565 quantization made the knob unworkably sensitive). The accepted approach is to use `COLOR_BG = #000000` and live with the panel's native gamma — no per-channel multiplication in `theme::color()`.
