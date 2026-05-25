# Ori — Project Memory

Stable facts about this project. Only update this file if a fundamental decision changes (rename, hardware swap, spec revision). Do not put evolving state here.

---

## Names

| Item | Value |
|---|---|
| Product name | **Ori** |
| PC companion app name | **Orion** |
| Orion installation URL | `ori.app/orion` |
| Orion supported platforms | Windows, macOS |
| BLE device name format | `Ori-XX-XX` (suffix is user-specific, e.g. `Ori-XT-9F`) |

---

## Brand Assets

### Ori brand mark (logo)

The canonical Ori logo is two concentric circles — a **thin outer ring** and a **small solid inner disc** — both rendered in a single flat colour. The geometry is intentionally simple: legible at any size, no gradients, no shading.

**Colour:** accent gold `#E0B86A` throughout. In SVG the symbol uses `currentColor` so the parent element's `color` property controls the shade; in firmware both shapes use `theme::COLOR_ACCENT` directly.

SVG source (canvas `100 × 100`, as defined in `Ori_UI_Prototype.html` `<symbol id="i-ori-logo">`):

```svg
<circle cx="50" cy="50" r="44" fill="none" stroke="currentColor" stroke-width="3"/>
<circle cx="50" cy="50" r="14" fill="currentColor"/>
```

Firmware equivalent (`make_brand_mark()` in `screen_setup.cpp`, size-parametric):
- Outer ring: `lv_obj` with `LV_RADIUS_CIRCLE`, `bg_opa = TRANSP`, `border_color = COLOR_ACCENT`, `border_width = 4`
- Inner disc: child `lv_obj`, `radius = CIRCLE`, `bg_color = COLOR_ACCENT`, diameter = `size / 4`

Used as:
- The brand mark on the device setup-flow Welcome and Step 1 screens (below the ring, the lowercase `ori` wordmark appears with the "r" in accent gold).
- The album-art placeholder in Controls mode when nothing is playing (drawn on top of a dark gradient background — see `keyboard-mode.md`).
- The Orion PC app's logo / app icon (`pc-app.md` — same mark, rendered larger over a dark background).

Companion **wordmark** is the lowercase `ori` text with the middle "r" rendered in accent gold (`#E0B86A`) while the "o" and "i" are in primary text colour. Used on the setup welcome screen below the mark. Not used in Controls mode or the Orion app icon — the mark alone is enough.

### Presence palette (profile-photo border)

The 6 px border around the circular profile photo encodes the user's Microsoft Teams presence, pushed from Orion to Ori via the BLE Presence Status characteristic. **Colours match the actual Teams swatches so users get instant cross-app recognition** — not derived from Ori's softer palette.

| State (BLE byte) | Hex | Token (firmware `theme.h` / HTML CSS var) |
|---|---|---|
| Available (`0x00`) | `#92C353` (Teams green) | `COLOR_PRESENCE_AVAILABLE` / `--presence-available` |
| Busy / DND (`0x01`) | `#C4314B` (Teams red)   | `COLOR_PRESENCE_BUSY` / `--presence-busy` |
| Away / BRB (`0x02`) | `#FFAA44` (Teams amber) | `COLOR_PRESENCE_AWAY` / `--presence-away` |
| Offline (`0x03`)    | `#8A8884` (Teams grey)  | `COLOR_PRESENCE_OFFLINE` / `--presence-offline` |

These swatches are **brighter and more saturated than the rest of Ori's calm palette by design** — the presence ring needs to read instantly from across the room without competing with the calm-toned UI underneath. The contrast is the point: when the user wants to glance at availability they get a Teams-shaped signal in Teams-shaped colour. The device-side fallback colour when Orion is offline is always `COLOR_PRESENCE_OFFLINE`. See `screen-layout.md` § Profile-photo border for the full rule.

---

## Product Identity

- A desk-based status and awareness display — calm, non-intrusive, offline-capable.
- Designed to be **transferable between users** via factory reset.
- Two data sources: Orion (PC) for calendar/profile/PTO, phone ANCS for notification icons (tap icon → full title + body detail overlay; read-only, no replies).

---

## Hardware

| Component | Value |
|---|---|
| Module | Waveshare ESP32-S3 Touch LCD 4.3" |
| Resolution | 800 × 480 px |
| Touch controller | GT911 capacitive, 5-point multitouch |
| Power | USB-C, wall-powered. Cable is permanently connected during normal operation. |
| Battery | **Optional LiPo backup** on some units — a brief-blackout buffer only. Firmware and UI must treat the device as if no battery exists (see Permanent Design Constraints). |
| External port (enclosure) | Native USB-C (ESP32-S3 USB peripheral) — single exposed port; UART debug header kept internally |
| Firmware toolchain | PlatformIO, Arduino framework |

---

## Fixed Numeric Constants

| Constant | Value | Notes |
|---|---|---|
| Work hours | 08:00 – 17:00 local | Drives meeting list vs. clock display |
| Pre-meeting alert | 5 minutes before start | Countdown modal trigger |
| Backlight default | ON | Restored after factory reset |
| Long-press duration | 3 seconds | Factory reset trigger; re-pair phone trigger |
| Re-pair phone long-press | 3 seconds | Same duration as factory reset |
| Backlight swipe presence | ~80 ms | Two-finger contact time before a swipe is evaluated |
| Backlight swipe travel | ≥ 60 px vertical | Minimum deliberate displacement to fire the gesture |
| Backlight NVS debounce | ~2 seconds | Saved 2 s after last toggle |

---

## Permanent Design Constraints

These are non-negotiable and will not change:

- **Never assume a battery.** Ori is USB-C wall-powered. Some units may include an optional LiPo as a brief-blackout backup, but the firmware and UI must behave as if it doesn't exist: no battery indicator, no charging glyph, no SoC readout, no low-power detection, no graceful-shutdown sequence, no "switch to battery mode" UI. The backup's only job is to keep the device from rebooting on a momentary USB drop; software never reads it or surfaces it. Yanking the cable on a unit without a battery is the off switch; on a unit with a battery it is *still* effectively the off switch as far as the firmware is concerned.
- No replying to notifications via ANCS — read-only. Icons appear in the status bar; tapping opens a detail overlay (title + body). No counts shown.
- No empty or broken screens under any connectivity state.
- Device must work fully with no PC connection and no phone connection (cached data display).
- Meeting row title and location are **single-line with ellipsis** — the full text is accessible via a tap-to-expand detail overlay (full-screen scrim showing title, location, start–end time; dismissed via Close button only). See `meeting-list.md` for the full rule.
- Backlight is **binary on/off**, not dimmable. The Waveshare board routes the backlight enable through a digital-only CH422G I/O expander (EXIO2) which is also tied to the panel DISP signal. PWM brightness is not possible without a hardware modification. When the backlight is OFF, the device is still fully running — only the LED is gated.
- **Firmware updates run over USB CDC (the always-present power cable), not BLE.** BLE OTA was in v1.0 of the BLE protocol and was removed in v1.1. USB-MSC drag-and-drop was also considered and rejected — Ori must not appear as a removable drive in File Explorer / Finder. See `ble-protocol.md` changelog and `ota.md` for the rationale and the new wire flow.
- **Controls mode uses Orion-mediated BLE commands, not standard BLE HID Over GATT (HOGP).** In Controls mode the album-art tap (play/pause) / horizontal swipe (prev/next) / vertical swipe (volume, with momentary HUD) gestures and the three user-assignable shortcut buttons all emit custom commands on the Ori Sync Service (`Keyboard Command` characteristic, `ble-protocol.md` §3 char 12 — name retained for historical reasons). Orion subscribes to these notifies and translates each into an OS-level action (media-key injection, OS volume API call, configured shortcut macro). Orion also pushes back state to Ori: host volume level (char 13), now-playing title + artist (char 14), and album art bytes (char 15, chunked JPEG). USB-C carries only power + firmware update. Controls mode is a top-level user-selectable mode reached via the status-bar mode-toggle button (rightmost status-bar element); the toggle is **hidden when Orion is offline** since Controls is useless without the bridge.
