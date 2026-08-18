# Ori — Project Memory

Stable facts with no better home. Only update if a fundamental decision changes.

---

## Names & URLs

| Item | Value |
|---|---|
| Product name | **Ori** |
| PC companion app name | **Orion** — own sibling repo, `../Orion/`, split out 2026-07-26. Full Orion implementation facts (tech stack, pairing UX, window model, background lifecycle, distribution channel, weather source) now live in `../Orion/.claude/memory.md`; only the rows below still matter from Ori's own side. |
| Parent/company brand | **Orinari** — three-tier structure decided 2026-07-21: Orinari is the company ("Apple"), Ori is the hardware ("iPhone"), Orion is the companion software ("iOS"). Domain: `orinari.net` (`ori.app`/`ori.net`/`ori.ca` were all unavailable). |
| Orion installation URL | `orinari.net/orion` (referenced from the firmware's own Setup Step 1 screen) |
| BLE device name | `Ori` |
| Phone icon disconnected-state design | **Neutral colour in both states (never red); a diagonal slash cut across the glyph is the sole "disconnected" signal.** Rationale: color-only status is a legibility gap for colorblind users, and a shape change reads correctly regardless of red/grey perception — same convention as "muted mic"/"no wifi" icons. Implemented identically across four surfaces: `firmware/src/widgets/widget_status_bar.cpp` (LVGL `lv_line` diagonal) and `Ori_UI_Prototype.html`'s `#i-phone-broken` (SVG mask-cut) here in Ori; `Orion_UI_Prototype.html/css` and `src/index.html`/`styles.css` (overlaid SVG `<line class="phone-slash">`, toggled via `.phone-disconnected`) in the sibling `../Orion/` repo. |

---

## Library Versions

| Library | Pinned version |
|---|---|
| LVGL | **9.5.0** — `lvgl/lvgl@9.5.0` in `firmware/platformio.ini` |
| NimBLE-Arduino | **2.5.0** — `h2zero/NimBLE-Arduino@2.5.0` |

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
| OTA progress ring (`screen_ota_updating`) | 200 ms | 5 | Driven by `ota_receiver::poll()` at one update per integer percent; on a ~1.5 MB BLE transfer that lands roughly every 0.4–1 s |

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
reconnecting at once (never during setup — peers bond one at a time there), this
chain got deep enough to overflow first the NimBLE host task (fixed: stack
4096→16384 in `platformio.ini`), then the Arduino loop task (fixed:
`getArduinoLoopTaskStackSize()` override → 16384 in `main.cpp`). If a backtrace
touches `ble_store_config_persist_cccds`, suspect this.

---

## Wordmark

Lowercase `ori` text; "o" and "i" in primary text colour, "r" in accent gold `#E0B86A` (`theme::COLOR_ACCENT`). Flanking gradient lines on setup screens. Used as the album-art empty-state placeholder (centred on dark gradient) and the Orion app icon.
