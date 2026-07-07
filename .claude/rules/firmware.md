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
- **BLE — PC link**: Orion sync (profile, calendar, Time Off, time, backlight, factory reset) — `connectivity.md`, `ble-protocol.md`
- **BLE — Phone link**: ANCS client — notification icons in status bar; tap opens detail overlay (title + body); read-only — `connectivity.md`. Icon registry (`ancs_icons.h/cpp`): compiled-in token→colour+image table for 49 apps. **Adding a new app icon requires a firmware update.** Supported tokens: `gmail`, `messenger`, `instagram`, `facebook`, `whatsapp`, `slack`, `twitter`, `teams`, `sms`, `phone`, `discord`, `telegram`, `youtube`, `youtube_music`, `tiktok`, `spotify`, `wechat`, `line`, `zoom`, `outlook`, `snapchat`, `google_meet`, `facetime`, `linkedin`, `reddit`, `threads`, `twitch`, `uber`, `apple_music`, `amazon`, `viber`, `claude`, `chatgpt`, `google_map`, `google_photos`, `health`, `apple_calendar`, `apple_findmy`, `apple_mail`, `apple_maps`, `apple_reminders`, `apple_wallet`, `github`, `google_authenticator`, `microsoft_authenticator`, `notion`, `venmo`, `skype`, `paypal`. Unknown tokens fall back to a brand-colour circle. On connect Ori also reads the iPhone's GAP Device Name (unpair modal) and its **Current Time Service** (0x1805) as a **secondary** clock source — used only when Orion (primary) hasn't set the time; see `connectivity.md`.
- **Controls-mode BLE chars** (chars 10–13, no HOGP): `Keyboard Command` (notify P→C; internal name kept for historical reasons), `Host Volume State`, `Media Metadata`, `Media Album Art` (chunked JPEG) — `ble-protocol.md` §3, §12
- **Presence**: delivered via the **Device Settings** characteristic (char `000E`, key `"p"`) rather than a dedicated characteristic — pushed by Orion on Teams change. Handler → `widget_profile_card::set_presence()` → `theme::COLOR_PRESENCE_*`. **Fallback rule**: on BLE-PC link drop, immediately force `COLOR_PRESENCE_OFFLINE` in the connection state-change handler — never show stale presence. Colour map: `screen-layout.md`
- **Controls-mode UI** (internal: `kbd-mode`): 216 × 216 album-art image (tap = play/pause, h-swipe = prev/next, v-swipe = volume + HUD), title + artist, three shortcut buttons. Empty state: Ori wordmark on dark gradient. JPEG decoder: `LV_USE_SJPG` or `LV_USE_TJPGD` (decide at M5). Mode persists in NVS. Shortcut button icons are compiled-in flash assets; **adding new icon types requires a firmware update** — `media-mode.md`
- **Mode-toggle visibility**: show only when Orion is BLE-connected. On PC link drop: remove toggle + auto-revert to calendar. On reconnect: restore toggle (calendar mode — entering Controls is always the user's explicit choice)
- **State machine**: priority-ordered left panel — `state-machine.md`
- **Meeting list**: sorting, overlap display, live expiry — `meeting-list.md`
- **NVS persistence**: profile, Time Off, backlight state, pairing bonds — survives power cycles. **Meeting list and local time are RAM-only** (deliberately not persisted) — re-synced from Orion on reconnect; see `meeting-list.md`
- **Backlight**: always ON — set by `backlight::init()` at boot via CH422G EXIO2; no runtime control
- **Setup flow**: first-boot sequence, factory reset — `setup-flow.md`
- **5-minute alert timer**: countdown modal before each meeting — `state-machine.md`
- **Firmware update receiver**: USB CDC framed OTA only — **not BLE**. Feeds Arduino `Update` library into the inactive OTA slot; SHA-256 verification; bootloader rollback on unhealthy first boot. Bricked-unit recovery: internal UART (service path only, enclosure required). Screen already implemented: `src/screens/screen_ota_updating.cpp` — `ota.md`

## LVGL Rendering Rules

- **All circular progress rings sweep clockwise** — countdown modal, OTA ring, reconnect-syncing overlay, setup spinners (Steps 2/3/4), re-pair phone spinner, and any future progress visual. In LVGL, ensure the arc end-angle increases in the direction of clock motion (LVGL 0° = 3 o'clock, angle increases clockwise). Counter-clockwise is banned.

- **Pressed-state feedback: use `lv_obj_set_style_opa`, not style transitions** — animating `LV_STYLE_BG_OPA`, `LV_STYLE_SHADOW_WIDTH`, or `LV_STYLE_SHADOW_SPREAD` via `lv_style_transition_dsc_t` causes a brief black flash on label text mid-animation (LVGL composites the button interior against near-black before reblending children). Use `lv_obj_set_style_opa(widget, LV_OPA_60, LV_STATE_PRESSED)` — no transition, no animation. Applied globally in `ui_helpers.cpp::make_btn()` and all interactive widgets.
