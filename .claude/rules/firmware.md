---
paths:
  - "firmware/**"
---

# Ori — Firmware

## Build System

- **IDE / build tool**: PlatformIO
- **Framework**: Arduino
- **Target board**: Waveshare ESP32-S3 Touch LCD 4.3" (ESP32-S3)

## What the Firmware Must Implement

- **Display**: 800 × 480 UI — `screen-layout.md`, `state-machine.md`
- **Touch**: GT911 driver; single-touch UI interactions — `gestures.md`
- **BLE — PC link**: Orion sync (profile, calendar, PTO, time, backlight, factory reset) — `connectivity.md`, `ble-protocol.md`
- **BLE — Phone link**: ANCS client — notification icons in status bar; tap opens detail overlay (title + body); read-only — `connectivity.md`. Icon registry (`ancs_icons.h/cpp`): compiled-in token→colour+glyph table for 23 apps. **Adding a new app icon requires a firmware update.**
- **Controls-mode BLE chars** (chars 12–15, no HOGP): `Keyboard Command` (notify P→C; internal name kept for historical reasons), `Host Volume State`, `Media Metadata`, `Media Album Art` (chunked JPEG) — `ble-protocol.md` §3, §12
- **Presence Status** (char 16): 1-byte enum pushed by Orion on Teams change. Handler → `widget_profile_card::set_presence()` → `theme::COLOR_PRESENCE_*`. **Fallback rule**: on BLE-PC link drop, immediately force `COLOR_PRESENCE_OFFLINE` in the connection state-change handler — never show stale presence. Colour map: `screen-layout.md`
- **Controls-mode UI** (internal: `kbd-mode`): 216 × 216 album-art image (tap = play/pause, h-swipe = prev/next, v-swipe = volume + HUD), title + artist, three shortcut buttons. Empty state: Ori wordmark on dark gradient. JPEG decoder: `LV_USE_SJPG` or `LV_USE_TJPGD` (decide at M5). Mode persists in NVS. Shortcut button icons are compiled-in flash assets; **adding new icon types requires a firmware update** — `media-mode.md`
- **Mode-toggle visibility**: show only when Orion is BLE-connected. On PC link drop: remove toggle + auto-revert to calendar. On reconnect: restore toggle (calendar mode — entering Controls is always the user's explicit choice)
- **State machine**: priority-ordered left panel — `state-machine.md`
- **Meeting list**: sorting, overlap display, live expiry — `meeting-list.md`
- **NVS persistence**: profile, meetings, PTO, backlight state, pairing bonds — survives power cycles
- **Backlight**: always ON — set by `backlight::init()` at boot via CH422G EXIO2; no runtime control
- **Setup flow**: first-boot sequence, factory reset — `setup-flow.md`
- **5-minute alert timer**: countdown modal before each meeting — `state-machine.md`
- **Firmware update receiver**: USB CDC framed OTA only — **not BLE**. Feeds Arduino `Update` library into the inactive OTA slot; SHA-256 verification; bootloader rollback on unhealthy first boot. Bricked-unit recovery: internal UART (service path only, enclosure required). Screen already implemented: `src/screens/screen_ota_updating.cpp` — `ota.md`
