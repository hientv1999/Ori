# Ori — Project Map

Ori is a desk-based status and awareness display built on an ESP32-S3 touch panel, paired with a PC companion app called **Orion**. This repository holds both halves of the system — the embedded firmware and the Orion PC app — along with the UI prototype and the product specification.

## Repository Layout

```
Ori/
├── .claude/
│   ├── memory.md            # Stable project facts (names, constants, hardware)
│   └── rules/               # Topic-scoped behavioral rules
├── firmware/                # ESP32-S3 firmware — PlatformIO + Arduino
├── PC_app/                  # Orion PC companion app — Windows + macOS
├── Ori_UI_Prototype.html    # Browser-based UI simulator
├── Ori_UI_Prototype.js      # Prototype logic, screen catalogue, renderers
├── Device Description.docx  # Authoritative product / behavior specification
└── CLAUDE.md                # This file — orientation map only
```

## Where to Look

| Looking for… | Go to |
|---|---|
| Names, URLs, supported platforms, BLE name format | [.claude/memory.md](.claude/memory.md) |
| Fixed numeric constants (work hours, long-press duration, etc.) | [.claude/memory.md](.claude/memory.md) |
| Hardware specs and permanent constraints | [.claude/memory.md](.claude/memory.md) |
| Product goals, non-goals, design philosophy | [.claude/rules/product-intent.md](.claude/rules/product-intent.md) |
| Hardware-derived behavioral rules | [.claude/rules/hardware.md](.claude/rules/hardware.md) |
| Status bar and panel layout | [.claude/rules/screen-layout.md](.claude/rules/screen-layout.md) |
| Left panel state priority and transitions | [.claude/rules/state-machine.md](.claude/rules/state-machine.md) |
| Meeting list rendering, sorting, offline behavior | [.claude/rules/meeting-list.md](.claude/rules/meeting-list.md) |
| BLE model (Orion PC + phone ANCS) | [.claude/rules/connectivity.md](.claude/rules/connectivity.md) |
| **BLE GATT protocol — services, characteristics, payloads, sequences** | [.claude/rules/ble-protocol.md](.claude/rules/ble-protocol.md) |
| **OTA firmware update UX and rules** | [.claude/rules/ota.md](.claude/rules/ota.md) |
| Touch gestures | [.claude/rules/gestures.md](.claude/rules/gestures.md) |
| **Media mode UI (album-art transport + volume gestures, shortcuts)** | [.claude/rules/media-mode.md](.claude/rules/media-mode.md) |
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

- [x] **M1 — Shared BLE contract** *(unblocks everything)* — locked 2026-05-15, spec **v1.7 (2026-05-30)**
  Spec in [.claude/rules/ble-protocol.md](.claude/rules/ble-protocol.md). 15 characteristics: profile, photo, meetings, PTO, time sync, factory reset, sync manifest, sync control, 4 media-mode chars (Keyboard Command, Host Volume State, Media Metadata, Media Album Art), Presence Status. Passkey bonding, hash-manifest delta reconnect. No BLE OTA (USB CDC instead). No HOGP.

- [x] **M2 — Firmware skeleton on hardware** — locked 2026-05-16
  PlatformIO project (16 MB flash, 8 MB PSRAM, 3 MB OTA slots), GT911 touch driver (INT-driven), CH422G I/O expander driver, NVS scaffolding, Arduino_GFX RGB16 panel driver, LVGL 9.5.0 wired to a partial draw buffer, LVGL input adapter.

- [x] **M3 — Firmware UI port (offline/mock data)** — locked 2026-05-22
  All screens implemented: status bar, profile card, meeting list, digital clock, PTO visual, 5-min countdown modal, setup flow (3-step: Install → Link Orion → iPhone), factory reset confirm, media mode, OTA-updating, reconnect-syncing overlay, profile detail overlay, ANCS notification overlay. Key decisions locked: Close-button-only policy on all overlays; ANCS tap opens full-screen detail overlay (title + body). Build state: RAM 39.2%, Flash 24.1% (of 3 MB OTA slot).

- [x] **M4 — Firmware state machine + persistence** — locked 2026-05-23
  Left-panel priority logic, 1 s tick (meeting-list refresh + pre-meeting countdown), 5-min one-shot alert per boot, mode toggle (Calendar ↔ Media) with NVS persistence, Media auto-revert on PC disconnect, `on_ota_begin` / `on_reconnect_begin` / `on_reconnect_end` hooks. Long-press handlers wired (profile photo → factory reset, phone-disconnect → re-pair). ANCS icon registry (`ancs_icons.h/cpp`) covering 23 apps. **Known unfixed bug:** factory reset calls `ESP.restart()` inside an LVGL callback → "Cache disabled but cached memory region accessed" on some runs when LCD DMA ISR fires during NVS sector erase. Fix deferred to M5. Build state: RAM 39.2%, Flash 24.1%.

- [ ] **M5 — Firmware BLE + USB CDC firmware update**
  GATT server (15 chars, full v1.7 spec) + ANCS client, 6-digit passkey bonding, dual-connection management, phone-disconnect icon logic, status-bar mode-toggle visibility tied to BLE-PC link state, profile-photo border driven by Presence Status (falls to offline grey on link drop). JPEG-decoder integration for album art. Validate against M1 contract with mock central. Plus USB CDC firmware-update receiver (framed protocol per `ota.md`, feeds Arduino `Update.write`). **Stack note:** NimBLE-Arduino (`h2zero/NimBLE-Arduino@2.5.0`) required — uses ~40 KB SRAM vs ~80 KB for stock ESP32 BLE; add to `lib_deps` and `-I include/ble` to `build_flags` in `platformio.ini`. LVGL is already pinned at `9.5.0` in `platformio.ini`. Agent: `esp32-connectivity`.

- [ ] **M6 — Orion PC app** *(parallel with M2–M5)*
  Flutter: pairing wizard + passkey confirm, calendar source selection (Google/Outlook/macOS), profile editor, PTO entry, background BLE central service, settings, connection status, and the USB CDC firmware-update sender (`flutter_libserialport` or equivalent). Windows + macOS. Agents: `flutter-frontend`, `orion-sync`, `calendar-integration`.

- [ ] **M7 — End-to-end integration**
  Full loop: calendar provider → Orion → BLE → Ori → screen state. Cross-subsystem tests, offline-cache validation, factory-reset round-trip, re-pair phone from runtime. Agent: `integration-qa`.

- [ ] **M8 — Hardening & polish**
  LVGL animation/memory tuning on real hardware, font/icon sizing review, ori.app/orion landing + app installers + signed builds, factory-provisioning procedure, user-facing setup docs. Orion-side media-mode bridge implementation (OS volume API, now-playing subscription, shortcut configuration UI — see `media-mode.md`, `pc-app.md`, `ble-protocol.md` §12). Carry-over:
  - **Large clock font.** Currently renders at Montserrat 48. Target ~96 px digits-only font (`0`–`9` + `:`; estimated ~37 KB flash). Apply in [`firmware/src/screens/screen_clock.cpp`](firmware/src/screens/screen_clock.cpp) (see `// TODO(M8): large-digit clock font` marker) and expose from [`firmware/src/theme.cpp`](firmware/src/theme.cpp) as `font_clock_xl()`.
