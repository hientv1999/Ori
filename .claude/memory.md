# Ori — Project Memory

Stable facts with no better home. Only update if a fundamental decision changes.

---

## Names & URLs

| Item | Value |
|---|---|
| Product name | **Ori** |
| PC companion app name | **Orion** |
| Orion installation URL | `ori.app/orion` |
| Orion supported platforms | Windows, macOS |
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
| LVGL draw buffer | PSRAM (`heap_caps_malloc`) | 96 KB (800 × 60 × 2) | `lvgl_display.cpp` |
| Framebuffer | PSRAM (Arduino_GFX `auto_flush=false`) | 750 KB (800 × 480 × 2) | LCD_CAM DMA-scans continuously |
| Bounce buffer | — | removed | replaced by `esp_cache_msync` — see `hardware.md` |

Draw buffer is in PSRAM (not static SRAM) because NVS flash writes temporarily disable ICache/DCache — a static SRAM draw buffer causes a cache-fault crash when LVGL rendering and NVS writes overlap.

---

## Wordmark

Lowercase `ori` text; "o" and "i" in primary text colour, "r" in accent gold `#E0B86A` (`theme::COLOR_ACCENT`). Flanking gradient lines on setup screens. Used as the album-art empty-state placeholder (centred on dark gradient) and the Orion app icon.
