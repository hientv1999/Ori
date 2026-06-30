# Ori — Project Memory

Stable facts with no better home. Only update if a fundamental decision changes.

---

## Names & URLs

| Item | Value |
|---|---|
| Product name | **Ori** |
| PC companion app name | **Orion** |
| Orion installation URL | `ori.app/orion` |
| Orion tech stack (Windows) | WinUI 3 (Windows App SDK), C#/XAML — chosen over Flutter 2026-06-29 for native BLE access + minimal background-service footprint |
| Orion tech stack (macOS, planned) | SwiftUI (Swift), Core Bluetooth, EventKit, `MenuBarExtra` — chosen 2026-06-29 for the same native-BLE/minimal-footprint reasoning as Windows |
| Orion supported platforms | Windows first (WinUI 3) — building now. macOS planned next as a **separate Swift/SwiftUI codebase**, not a shared one — not yet scheduled/started |
| Orion UI model | **Compact flyout/panel anchored to the tray (Win) / menu bar (macOS) icon — never a full-size, resizable, taskbar-visible window.** No title bar, no min/max/resize chrome, no separate "main window." Every screen (pairing wizard, passkey confirm, settings, profile editor, calendar source picker, connection status) is a view inside this one panel, not a separate window. Decided 2026-06-29. |
| Orion background requirement | **Must run continuously in the background — BLE connection, sync loop, and media/volume bridge all stay live — regardless of whether the tray/menu-bar panel is open, closed, or never opened at all.** The panel is purely a UI surface for the user to glance at or configure things; closing it must never pause or stop any of Orion's actual work. Implies: start at login (configurable), survive screen lock/sleep where the OS allows, and avoid any deployment/packaging model that lets the OS suspend the process when it's not in the foreground (e.g. prefer an unpackaged or Desktop-Bridge-style Win32 process over a lifecycle-managed UWP-style container) — see `orion-sync.md`'s "Background service lifecycle." |
| Orion distribution channel | **Direct download + notarization only, on both platforms — never the Mac App Store or Microsoft Store.** Decided 2026-06-29. This is why macOS Controls mode can use the private `MediaRemote` framework for now-playing detection (`pc-app.md`, `ble-protocol.md` §12): App Store Review Guideline 2.5.1 bans private-API apps, but notarization (Gatekeeper) only scans for malicious code, not private-API usage — so a Developer-ID-signed, notarized direct download is fine. If this ever changes to Mac App Store distribution, the `MediaRemote`-dependent now-playing/seek features must be dropped or reworked first. |
| BLE device name format | `Ori-XX-XX` (per-device suffix, e.g. `Ori-XT-9F`) |

---

## Library Versions

| Library | Pinned version |
|---|---|
| LVGL | **9.5.0** — `lvgl/lvgl@9.5.0` in `firmware/platformio.ini` |
| NimBLE-Arduino | **2.5.0** — `h2zero/NimBLE-Arduino@2.5.0` (add to `lib_deps` at M5) |

---

## Fixed Numeric Constants

| Constant | Value |
|---|---|
| Pre-meeting alert | 5 minutes before start |
| Long-press duration | 3 seconds (factory reset + re-pair phone) |
| Profile photo size | 228 × 228 px JPEG |

---

## Build Tool

`pio` is not on PATH. Invoke PlatformIO with the full path in PowerShell from `firmware/`:

```powershell
& "C:\Users\hient\.platformio\penv\Scripts\pio.exe" run
```

---

## Animation Rates

All animated widgets use `lv_timer` (not `lv_anim`) for independent rate control.

| Widget | Interval | fps | Notes |
|---|---|---|---|
| Setup spinners, re-pair spinner, `widget_progress_ring` indeterminate | 42 ms | 24 | 10°/step, 36 steps ≈ 1512 ms/rev |
| Colon blink (`screen_clock`) | 42 ms | 24 | 36-tick linear fade, opacity 255 ↔ 64 |
| OTA progress ring (`screen_ota_updating`) | 200 ms | 5 | Real PROGRESS frames arrive every 500 ms–1.5 s |

**M5:** Remove the mock OTA tick timer in `screen_ota_updating.cpp`; replace with `widget_progress_ring::set_value(ring, pct)` called directly from the USB CDC PROGRESS frame handler.

---

## Display Buffer Architecture

| Buffer | Location | Size | Notes |
|---|---|---|---|
| LVGL draw buffer | PSRAM (`heap_caps_malloc`) | 750 KB (800 × 480 × 2) | `lvgl_display.cpp` — full-frame |
| Framebuffer | PSRAM (Arduino_GFX `auto_flush=false`) | 750 KB (800 × 480 × 2) | LCD_CAM DMA-scans continuously |
| Bounce buffer | — | removed | replaced by `esp_cache_msync` — see `hardware.md` |

**Draw buffer = full frame (480 lines).** PSRAM is 8 MB; the extra ~650 KB vs the old 60-line buffer is negligible. Benefit: any dirty region LVGL renders always fits in one pass — no render-pass splits regardless of screen activity.

**Draw buffer must stay separate from the framebuffer.** It acts as the protection layer: only fully-rendered rectangles are ever copied into the live framebuffer via `flush_area()`, so LCD_CAM DMA never sees a partially-drawn frame. Do not switch to `LV_DISPLAY_RENDER_MODE_DIRECT` (which would render straight into the framebuffer and eliminate this guarantee).

Draw buffer is in PSRAM (not static SRAM) because NVS flash writes temporarily disable ICache/DCache — a static SRAM draw buffer causes a cache-fault crash when LVGL rendering and NVS writes overlap.

---

## Debugging Guru Meditation Crashes

Resolve panic backtraces against the real build (addresses often point into
framework/library code, not just `firmware/src`):

```bash
"/c/Users/hient/.platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-addr2line.exe" \
  -pfiC -e firmware/.pio/build/ori/firmware.elf <addr> [<addr> ...]
```

**Known crash family:** `notify()` on any GATT char can synchronously run NimBLE's
CCCD-persist-to-NVS chain (`ble_gatts_chr_updated → ble_store_write_cccd →
ble_store_config_persist_cccds`) on whatever task called it. With 2 bonded peers
reconnecting at once (power-cycle reconnect — never happens during setup, since
peers bond one at a time there), this chain got deep enough to overflow first the
NimBLE host task (fixed: stack 4096→16384 in `platformio.ini`), then later the
Arduino loop task (fixed: `getArduinoLoopTaskStackSize()` override → 16384 in
`main.cpp`). If a backtrace touches `ble_store_config_persist_cccds`, suspect this.

---

## Wordmark

Lowercase `ori` text; "o" and "i" in primary text colour, "r" in accent gold `#E0B86A` (`theme::COLOR_ACCENT`). Flanking gradient lines on setup screens. Used as the album-art empty-state placeholder (centred on dark gradient) and the Orion app icon.
