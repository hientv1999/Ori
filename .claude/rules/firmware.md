---
paths:
  - "firmware/**"
---

# Ori — Firmware

The `firmware/` folder contains the ESP32-S3 firmware for Ori.

## Build System

- **IDE / build tool**: PlatformIO
- **Framework**: Arduino
- **Target board**: Waveshare ESP32-S3 Touch LCD 4.3" (ESP32-S3)

## What the Firmware Must Implement

Refer to the other rule files in `.claude/rules/` for the full behavioral specification. Key areas:

- **Display**: render the 800 × 480 UI described in `screen-layout.md` and `state-machine.md`
- **Touch**: GT911 driver; single-touch UI interactions + two-finger backlight swipe gesture (`gestures.md`)
- **BLE — PC link**: connect to the Orion companion app; receive and cache profile, calendar, PTO, and local time (`connectivity.md`)
- **BLE — Phone link**: ANCS client for notification awareness — icons in the status bar; tap opens a full-screen detail overlay (title + body); read-only, no replying (`connectivity.md`)
- **Controls-mode characteristics**: four new chars on the Ori Sync Service (v1.2) — `Keyboard Command` (notify Ori → Orion; the internal name keeps "Keyboard" for historical reasons but it carries Controls-mode commands), `Host Volume State` (Orion → Ori), `Media Metadata` (Orion → Ori), `Media Album Art` (Orion → Ori, chunked JPEG). See `ble-protocol.md` §3 and §12. **No standard BLE HID Over GATT (HOGP) profile** — all Controls-mode commands ride the custom Ori Sync Service.
- **Presence Status characteristic** (`ble-protocol.md` §3 char 16, v1.3) — 1-byte enum (Available / Busy / Away / Offline) pushed by Orion when the user's Teams presence changes. Receive handler calls `widget_profile_card::set_presence()` which updates the profile-photo border colour via `theme::COLOR_PRESENCE_*`. **Device-side fallback rule**: when the BLE-PC link is down, force the border colour to `COLOR_PRESENCE_OFFLINE` regardless of the last cached value Orion pushed — the device must not claim a presence it can't verify. Wire this into the connection state-change handler so the transition happens immediately on link drop/restore. See `screen-layout.md` for the colour mapping.
- **Controls-mode UI** (internal code name: `kbd-mode`): secondary left-panel mode with a 216×216 album-art image as the dominant element (tap = play/pause, horizontal swipe = prev/next, **vertical swipe = volume** with a momentary HUD overlay), centred title + artist text below, and three user-assignable icon-only shortcut buttons at the bottom. No persistent volume slider and no dedicated mute button — volume is purely a gesture on the art; mute is just one of the user-assignable shortcuts. The album-art empty state (nothing playing) shows the **Ori brand mark** (see `memory.md` § Brand Assets) on a dark gradient. Album art decoded with LVGL's `lv_img` + the bundled JPEG decoder (LV_USE_SJPG or LV_USE_TJPGD — pick at M5). Selected mode persists in NVS. See `keyboard-mode.md`.
- **Status-bar mode-toggle visibility**: render the toggle only when Orion is connected over BLE. When the PC link drops, remove the toggle from the status bar; if the user was in Controls mode, auto-revert to calendar mode before hiding. When Orion reconnects, the toggle reappears (still in calendar mode — entering Controls is the user's explicit choice).
- **State machine**: priority-ordered left panel logic in **calendar mode** (`state-machine.md`)
- **Meeting list**: sorting, overlap display, live removal of past meetings (`meeting-list.md`)
- **NVS persistence**: profile, meetings, PTO, backlight state, pairing bonds — survives power cycles
- **Backlight**: binary on/off via CH422G EXIO2 (digital-only — no PWM possible on this board); saved to NVS (`gestures.md`)
- **Setup flow**: first-boot sequence, factory reset (`setup-flow.md`)
- **5-minute alert timer**: countdown modal before each meeting (`state-machine.md`)
- **Firmware update receiver**: USB CDC framed OTA protocol only (`ota.md`). No MSC by design — Ori must not appear as a removable drive. Feeds Arduino's `Update` library writing the inactive OTA slot, with SHA-256 verification and bootloader-rollback on unhealthy first boot. Bricked-unit recovery uses the internal UART port (service-only, enclosure must be opened). **Firmware updates do NOT run over BLE** — that was removed in `ble-protocol.md` v1.1. The on-device "Updating firmware… N%" screen is transport-agnostic and already wired up in `src/screens/screen_ota_updating.cpp`.
