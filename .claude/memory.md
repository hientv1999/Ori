# Ori — Project Memory

Stable facts with no better home. Only update if a fundamental decision changes.

---

## Names & URLs

| Item | Value |
|---|---|
| Product name | **Ori** |
| PC companion app name | **Orion** |
| Orion installation URL | `ori.app/orion` |
| Orion tech stack | **Tauri v2 (Rust backend + OS webview), one codebase for both Windows and macOS** — supersedes the 2026-06-29 WinUI 3 + SwiftUI split, decided 2026-07-08. The real UI is `PC_app/orion/src/` (`index.html`/`styles.css`/`app.js`, ported near-verbatim from `Orion_UI_Prototype.html/css/js`), hosted in WebView2 (Win) / WKWebView (mac) via Tauri. Backend crates: `btleplug` (BLE central, native — not a plugin layer, so it satisfies the original "native BLE access" reasoning that ruled out Flutter), `serialport` (USB CDC OTA), plus calendar/OAuth/media-bridge integrations. See `pc-app.md` and the plan at the time of the pivot for the full rationale. |
| Orion pairing UX | **Custom in-app passkey entry on Windows** — Ori displays its 6-digit passkey; the user types it into Orion's own digit-box modal (`setup-flow.md`), which drives WinRT's `DeviceInformationCustomPairing` (`PairingRequested` → `ProvidePin`). **macOS is unsolved, not "OS-native by design"** — CoreBluetooth exposes no app-level pairing UI hook at all, so custom entry is impossible there; deferred until the macOS build (M6b) starts. Options then: accept the system Bluetooth sheet (inconsistent but simple), or move passkey verification to the app layer (bond "Just Works" + verify over a GATT characteristic) for parity — the latter reopens the locked M1/M5 BLE contract and trades link-layer MITM for an app-layer equivalent, needs explicit sign-off. |
| Orion supported platforms | Windows and macOS from the same Tauri/Rust codebase — Windows building now, macOS build target not yet started |
| Orion UI model | **Compact, movable, non-resizable panel — no separate "main window," but NOT a click-away-dismiss flyout.** The panel auto-opens on launch, stays visible until the user explicitly minimizes it (in-app minimize button, top-right of a custom titlebar visible on every screen — or the tray icon again), and shows a normal taskbar entry while open (`skipTaskbar:false`), alt-tab-able like a regular small window. Frameless (`decorations:false`, custom-drawn rounded corners), tray-anchored (tray icon toggles show/hide, restores last-dragged position), but does not auto-hide on focus loss and is not `alwaysOnTop`. The custom titlebar (`.app-titlebar`) doubles as the drag handle — every screen (pairing wizard, settings, profile editor, calendar picker, connection status) is a view inside this one panel. |
| Orion rounded corners (Windows) | **CSS `border-radius` + a transparent Tauri window is NOT sufficient on Windows 10** — measured zero transparency at the geometric corner (WebView2's alpha doesn't reliably reach the native window surface; Windows 10 has no DWM auto-rounding, an 11-only feature). Fixed via `SetWindowRgn`/`CreateRoundRectRgn` (Win32, `src-tauri/src/lib.rs`'s `apply_rounded_corners()`, `windows` crate) — clips the HWND shape at the OS level, independent of WebView2 compositing. `#[cfg(target_os = "windows")]`; macOS's WKWebView is expected to handle CSS transparency correctly (not yet verified). |
| Orion window drag (Windows) | The plain `data-tauri-drag-region` attribute was unreliable in testing. Fixed with an explicit `app.js` handler: `mousedown` on `.app-titlebar` (skipping `.h-ico` buttons) calls `window.__TAURI__.window.getCurrentWindow().startDragging()` — needs `core:window:allow-start-dragging` in `capabilities/default.json`. |
| Orion background requirement | **Must run continuously in the background — BLE connection, sync loop, and media/volume bridge all stay live — regardless of whether the panel is open, closed, or never opened.** Closing the panel must never pause any of Orion's work. Implies: start at login (configurable), survive screen lock/sleep where the OS allows, avoid any packaging model that lets the OS suspend the process when backgrounded (prefer an unpackaged/Desktop-Bridge-style Win32 process over a lifecycle-managed UWP container) — see `orion-sync.md`'s "Background service lifecycle." |
| Orion distribution channel | **Direct download + notarization only, on both platforms — never the Mac App Store or Microsoft Store.** This is why macOS Controls mode can use the private `MediaRemote` framework for now-playing detection (`pc-app.md`, `ble-protocol.md` §12): App Store Review Guideline 2.5.1 bans private-API apps, but notarization only scans for malicious code. If this ever changes to Mac App Store distribution, the `MediaRemote`-dependent features must be dropped or reworked first. |
| BLE device name format | `Ori-XX-XX` (per-device suffix, e.g. `Ori-XT-9F`) |
| Orion weather data source | **Open-Meteo (open-meteo.com, no API key) for condition + temperature; Windows Location API (`Geolocator`, WinRT `Devices_Geolocation`) for lat/long.** Deferred to Phase D (`ble-protocol.md` §6.4's `"w"`/`"d"`/`"u"` Device Settings fields already have a working write path — `pc-app.md` — just no data source yet). IP-based geolocation was rejected: city-level accuracy (~1–30 km) breaks badly on VPN/mobile, well outside the ~5 km target. |
| Phone icon disconnected-state design | **Neutral colour in both states (never red); a diagonal slash cut across the glyph is the sole "disconnected" signal.** Rationale: color-only status is a legibility gap for colorblind users, and a shape change reads correctly regardless of red/grey perception — same convention as "muted mic"/"no wifi" icons. Implemented identically across all four surfaces: `firmware/src/widgets/widget_status_bar.cpp` (LVGL `lv_line` diagonal), `Ori_UI_Prototype.html`'s `#i-phone-broken` (SVG mask-cut), `PC_app/Orion_UI_Prototype.html/css` and `PC_app/orion/src/index.html`/`styles.css` (overlaid SVG `<line class="phone-slash">`, toggled via `.phone-disconnected`). |

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
reconnecting at once (never during setup — peers bond one at a time there), this
chain got deep enough to overflow first the NimBLE host task (fixed: stack
4096→16384 in `platformio.ini`), then the Arduino loop task (fixed:
`getArduinoLoopTaskStackSize()` override → 16384 in `main.cpp`). If a backtrace
touches `ble_store_config_persist_cccds`, suspect this.

---

## Wordmark

Lowercase `ori` text; "o" and "i" in primary text colour, "r" in accent gold `#E0B86A` (`theme::COLOR_ACCENT`). Flanking gradient lines on setup screens. Used as the album-art empty-state placeholder (centred on dark gradient) and the Orion app icon.
