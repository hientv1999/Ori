# Ori — Project Map

Ori is a desk-based status and awareness display built on an ESP32-S3 touch panel, paired with a PC companion app called **Orion**. This repository holds both halves of the system — the embedded firmware and the Orion PC app — along with the UI prototype and the product specification.

## Repository Layout

```
Ori/
├── .claude/
│   ├── memory.md            # Stable project facts (names, constants, hardware)
│   └── rules/               # Topic-scoped behavioral rules
├── firmware/                # ESP32-S3 firmware — PlatformIO + Arduino (to be built)
├── PC_app/                  # Orion PC companion app — Windows + macOS (to be built)
├── Ori_UI_Prototype.html    # Browser-based UI simulator
├── Ori_UI_Prototype.js      # Prototype logic, screen catalogue, renderers
├── Device Description.docx  # Authoritative product / behavior specification
└── CLAUDE.md                # This file — orientation map only
```

## Where to Look

| Looking for… | Go to |
|---|---|
| Names, URLs, supported platforms, BLE name format | [.claude/memory.md](.claude/memory.md) |
| Fixed numeric constants (work hours, brightness range, long-press duration, etc.) | [.claude/memory.md](.claude/memory.md) |
| Hardware specs and permanent constraints | [.claude/memory.md](.claude/memory.md) |
| Product goals, non-goals, design philosophy | [.claude/rules/product-intent.md](.claude/rules/product-intent.md) |
| Hardware-derived behavioral rules | [.claude/rules/hardware.md](.claude/rules/hardware.md) |
| Status bar and panel layout | [.claude/rules/screen-layout.md](.claude/rules/screen-layout.md) |
| Left panel state priority and transitions | [.claude/rules/state-machine.md](.claude/rules/state-machine.md) |
| Meeting list rendering, sorting, offline behavior | [.claude/rules/meeting-list.md](.claude/rules/meeting-list.md) |
| BLE model (Orion PC + phone ANCS) | [.claude/rules/connectivity.md](.claude/rules/connectivity.md) |
| **BLE GATT protocol — services, characteristics, payloads, sequences** | [.claude/rules/ble-protocol.md](.claude/rules/ble-protocol.md) |
| **OTA firmware update UX and rules** | [.claude/rules/ota.md](.claude/rules/ota.md) |
| Touch gestures and brightness control | [.claude/rules/gestures.md](.claude/rules/gestures.md) |
| **Controls mode UI (album-art transport + volume gestures, shortcuts; BLE-bridged via Orion)** | [.claude/rules/keyboard-mode.md](.claude/rules/keyboard-mode.md) |
| First-time setup, factory reset, re-pair phone | [.claude/rules/setup-flow.md](.claude/rules/setup-flow.md) |
| UI prototype internals (loads when editing the prototype) | [.claude/rules/ui-prototype.md](.claude/rules/ui-prototype.md) |
| Firmware build system and scope (loads under `firmware/`) | [.claude/rules/firmware.md](.claude/rules/firmware.md) |
| Orion PC app scope (loads under `PC_app/`) | [.claude/rules/pc-app.md](.claude/rules/pc-app.md) |
| Full authoritative product spec | `Device Description.docx` |
| Catalogue of every UI screen / state | Open `Ori_UI_Prototype.html` in a browser |

## UI Prototype Quick-Start

Open [Ori_UI_Prototype.html](Ori_UI_Prototype.html) directly in any modern browser. No server, no build step. Use the left sidebar to switch between every screen and edge case.

## Milestones

> Update checkboxes as work completes. Critical path: M1 → (M2–M5 ∥ M6) → M7 → M8.

- [x] **M1 — Shared BLE contract** *(unblocks everything)* — locked 2026-05-15, current spec **v1.6 (2026-05-24)**
  Spec lives in [.claude/rules/ble-protocol.md](.claude/rules/ble-protocol.md). Covers profile, photo, meetings, PTO, time sync, brightness (bidirectional), passkey bonding, hash-manifest delta reconnect, and remote factory reset. Changelog summary: v1.1 removed BLE OTA (USB CDC instead); v1.2 added keyboard-mode chars (Keyboard Command, Host Volume State, Media Metadata, Media Album Art); v1.3 added Presence Status char; v1.4 added `seek` op to KeyboardCommand; v1.5 added `can_seek` to MediaMetadata; v1.6 added optional `email`/`phone` fields to ProfileInfo. All post-v1.1 changes are additive. Companion UX rules in `ota.md`, `state-machine.md`, `connectivity.md`, `gestures.md`, `hardware.md`.

- [x] **M2 — Firmware skeleton on hardware** — locked 2026-05-16
  Firmware-core slice: PlatformIO project (16 MB flash, 8 MB PSRAM, 3 MB OTA slots), GT911 touch driver (INT-driven), CH422G I/O expander driver, NVS scaffolding via Arduino `Preferences`. esp32-lvgl slice: Arduino_GFX RGB16 panel driver, LVGL 8.4.0 wired to a partial draw buffer, LVGL input adapter, touch-echo demo. Post-M2 work: replumbed TP_RST + LCD_BL + LCD_RST through CH422G (Pattern C confirmed — backlight is binary on/off only; PWM impossible on stock hardware), refactored `backlight::set_on(bool)` / `is_on()` end-to-end, updated `memory.md` / `gestures.md` / `ble-protocol.md` / `pc-app.md` / `firmware.md` to match. Build state: RAM 41.2%, Flash 16.2% (509 KB of the 3 MB app slot).

- [x] **M3 — Firmware UI port (offline/mock data)** — locked 2026-05-22
  Translate HTML prototype into LVGL screens: status bar, profile card, meeting list, digital clock, PTO visual, 5-min countdown modal, setup flow, factory reset confirm, re-pair phone screen. Agent: `esp32-lvgl`. Delivered ahead of schedule: Controls mode UI (`screen_keyboard_mode.cpp`), OTA-updating screen (`screen_ota_updating.cpp`), and reconnect-syncing overlay (`screen_reconnect_syncing.cpp`) — all originally scoped to M5/M8. **Post-lock polish (2026-05-24 — session 1):** Profile detail overlay (`modal_profile.cpp`) finalised — two-column layout (scrollable info left, photo right at 269 px matching the right panel), photo Y pinned exactly to the calendar-mode position via `lv_obj_get_coords()` on the live widget at tap time, presence glow (shadow) matching `widget_profile_card`. `theme::SCRIM_OPA = LV_OPA_90` constant added to `theme.h` and applied to all 8 overlay scrims (was `LV_OPA_80` hardcoded per-file). Build state at polish lock: RAM 39.2%, Flash 24.0% (of the 3 MB OTA slot). **Post-lock polish (2026-05-24 — session 2):** Close-button-only policy enforced across all overlays — no overlay uses tap-to-dismiss anywhere. `screen_pto.cpp::show_pto_detail()` rewritten: scrim click removed, content wrapped in a scrollable container (max 312 px) with `lv_obj_set_style_max_height`, Close button added below scroll area matching the `modal_profile` pattern. ANCS notification model updated: tapping an icon opens a full-screen detail overlay (title + body); dismissed via Close button only. HTML prototype: PTO detail, countdown, and meeting detail overlays all converted to Close-button-only; `dismissModalIfTappable()` emptied. Desktop sim: fixed two linker errors (`AppState evaluate()` stub in `arduino_shim.cpp`; `modal_countdown::create()` 4th-arg type); added `10b_modal_profile.bmp` to the render catalogue; all 30 screenshots regenerated. **Post-lock polish (2026-05-25):** `modal_countdown.cpp` updated to Close-button-only — removed `on_scrim_press` handler and "Tap to dismiss" hint, added gap spacer + tertiary Close button; stale tap-to-dismiss comments cleaned from `screen_meeting_list.cpp` and `state_machine.cpp`. Build state: RAM 39.2%, Flash 24.1% (of the 3 MB OTA slot); 31 screenshots regenerated.

- [x] **M4 — Firmware state machine + persistence** — locked 2026-05-23
  `state_machine.cpp`: left-panel priority logic, 1 s tick (work-hour boundary + meeting-list refresh + pre-meeting countdown), 5-min one-shot alert per boot, mode toggle (Calendar ↔ Controls) with NVS persistence, Controls auto-revert on PC disconnect, `on_ota_begin` / `on_reconnect_begin` / `on_reconnect_end` hooks ready for M5. `nvs_store.cpp`: backlight debounce (~2 s), mode persist, first-boot flag (`"prov"`), factory wipe. Long-press handlers wired: profile photo → factory reset confirm, phone-disconnect icon → re-pair phone. PTO active and meeting expiry are stubs pending real NVS-backed BLE data (M5 scope). **Known unfixed bug:** factory reset calls `ESP.restart()` inside an LVGL event callback, which triggers "Cache disabled but cached memory region accessed" on some runs when the LCD DMA ISR fires during NVS sector erase. Fix deferred to M5 (restructuring the BLE/NVS layer addresses the same write pattern). **Build state (post-lock polish 2026-05-25):** RAM 39.2%, Flash 24.1% (of the 3 MB OTA slot). BLE stack (M5) will consume ~40 KB SRAM, leaving comfortable headroom. **Pre-M5 UI prep (2026-05-25):** Touch driver ISR gate finalised — `touch::poll()` skips the I²C bus entirely when `touch_pending` is false, eliminating idle I²C transactions. ANCS mock app list expanded from 5 to 23 apps (`mock_data.cpp`). Centralised ANCS icon registry added (`firmware/include/assets/ancs_icons.h`, `firmware/src/assets/ancs_icons.cpp`) — a compiled-in token→colour table covering all 23 apps; replacing app icons requires a firmware update by design. Status-bar tiles and ANCS notification modal both updated to use solid-colour circles (no abbreviation text) from this registry, replacing the two separate 5-entry duplicate tables.

- [ ] **M5 — Firmware BLE + USB CDC firmware update**
  GATT server (**16 chars, full v1.6 spec** — 11 sync chars + 4 Controls-mode chars: Keyboard Command, Host Volume State, Media Metadata, Media Album Art + 1 presence char: Presence Status) + ANCS client, 6-digit passkey bonding, dual-connection management, phone-disconnect icon logic, status-bar mode-toggle visibility tied to BLE-PC link state, profile-photo border driven by Presence Status (auto-falls to offline grey on link drop). JPEG-decoder integration for album art rendering. Validate against M1 contract with mock central. **Plus** the USB CDC firmware-update receiver (framed protocol per `ota.md`, feeds Arduino `Update.write`). No standard BLE HID Over GATT (HOGP) — Controls-mode commands ride the custom Ori Sync Service; Orion bridges to OS HID. No USB-MSC and no BLE OTA — by design. **Stack note:** NimBLE-Arduino (`h2zero/NimBLE-Arduino@^1.4.2`) is the preferred BLE library (uses ~40 KB SRAM vs ~80 KB for stock ESP32 BLE); add it to `lib_deps` and `-I include/ble` to `build_flags` in `platformio.ini` when starting this milestone. Agent: `esp32-connectivity`.

- [ ] **M6 — Orion PC app** *(parallel with M2–M5)*
  Flutter: pairing wizard + passkey confirm, calendar source selection (Google/Outlook/macOS), profile editor, PTO entry, background BLE central service, settings, connection status, **and the USB CDC firmware-update sender** (`flutter_libserialport` or equivalent — opens the serial port to the USB-C-connected Ori, streams the framed OTA protocol per `ota.md`). Windows + macOS. Agents: `flutter-frontend`, `orion-sync`, `calendar-integration`.

- [ ] **M7 — End-to-end integration**
  Full loop: calendar provider → Orion → BLE → Ori → screen state. Cross-subsystem tests, offline-cache validation, factory-reset round-trip, re-pair phone from runtime. Agent: `integration-qa`.

- [ ] **M8 — Hardening & polish**
  LVGL animation/memory tuning on real hardware, font/icon sizing review, ori.app/orion landing + app installers + signed builds, factory-provisioning procedure, user-facing setup docs. **Plus** the Controls-mode UI implementation on hardware: 240 × 240 album-art image widget as the dominant interaction surface — tap (play/pause), horizontal swipe (prev/next), and vertical swipe (volume with momentary HUD overlay); centred title/artist metadata; three icon-only user-shortcut buttons (one of which can be configured as "toggle mute" if the user wants it within reach). **Plus** the matching Orion-side implementation: subscribe to `Keyboard Command` and bridge to OS via `SendInput` (Win) / `CGEventPost` (macOS, with Accessibility permission); subscribe to OS volume changes and push `HostVolumeState`; subscribe to OS now-playing changes and push `MediaMetadata` + chunked JPEG `MediaAlbumArt`; shortcut-configuration settings UI. See `keyboard-mode.md`, `pc-app.md`, and `ble-protocol.md` §12. **Plus** carry-over polish items deferred from earlier milestones:
    - **Large clock font for the after-hours digital clock.** Currently renders at Montserrat 48 — the largest font bundled today. The HTML prototype targets ~170 px; M3 tried a 2× widget zoom via `lv_obj_set_style_transform_zoom` but reverted, because `transform_zoom` (and `transform_angle`) require `LV_COLOR_SCREEN_TRANSP = 1` in `lv_conf.h`, which is disabled to keep the per-flush memory budget down. The intended M8 fix is a **digits-only large font** containing just `0`–`9` and `:` at ~96 px (estimated ~37 KB flash, far cheaper than a full alphabet at that size). Apply it in [`firmware/src/screens/screen_clock.cpp`](firmware/src/screens/screen_clock.cpp) — see the file header comment and the `// TODO(M8): large-digit clock font` marker — and expose it from [`firmware/src/theme.cpp`](firmware/src/theme.cpp) (e.g. `font_clock_xl()`).
