# Ori — Project Map

Ori is a desk-based status and awareness display built on an ESP32-S3 touch panel, paired with a PC companion app called **Orion**. This repository holds the embedded firmware, the device UI prototype, and the product specification. **Orion (the PC companion app) lives in its own sibling repo, `../Orion/`** — split out 2026-07-26 as the Orinari ecosystem grew to include other products (Origale, Orimat) alongside Ori. The two repos are meant to be cloned as siblings; cross-repo doc links here and in `../Orion/` are written relative-path (e.g. `../Orion/.claude/rules/pc-app.md`) and assume that layout.

## Repository Layout

```
Ori/
├── .claude/
│   ├── memory.md            # Stable project facts (names, constants, hardware)
│   ├── rules/                # Topic-scoped behavioral rules
│   └── agents/                # Specialized agent definitions
├── firmware/                # ESP32-S3 firmware — PlatformIO + Arduino
├── tools/                   # Standalone test scripts — mock the Orion side
│                            # of BLE sync (mock_orion_ble.py) and USB CDC
│                            # OTA (mock_orion_ota.py) against real hardware,
│                            # plus manufacturing provisioning (factory_provision.py)
├── Ori_UI_Prototype.html    # Browser-based UI simulator
├── Ori_UI_Prototype.js      # Prototype logic, screen catalogue, renderers
├── Device Description.docx  # Authoritative product / behavior specification
└── CLAUDE.md                # This file — orientation map only

../Orion/                    # PC companion app — sibling repo, see its own CLAUDE.md
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
| **Mass-production identity (serial number, manufacture date) — storage, provisioning, BLE exposure** | [.claude/rules/provisioning.md](.claude/rules/provisioning.md) |
| Touch gestures | [.claude/rules/gestures.md](.claude/rules/gestures.md) |
| **Media mode UI (album-art transport + volume gestures, shortcuts)** | [.claude/rules/media-mode.md](.claude/rules/media-mode.md) |
| First-time setup, factory reset, re-pair phone | [.claude/rules/setup-flow.md](.claude/rules/setup-flow.md) |
| UI prototype internals (loads when editing the prototype) | [.claude/rules/ui-prototype.md](.claude/rules/ui-prototype.md) |
| Firmware build system and scope (loads under `firmware/`) | [.claude/rules/firmware.md](.claude/rules/firmware.md) |
| **Orion PC companion app — now a sibling repo** | [../Orion/CLAUDE.md](../Orion/CLAUDE.md), spec at [../Orion/.claude/rules/pc-app.md](../Orion/.claude/rules/pc-app.md) |
| Hand-test BLE sync against real Ori hardware (no Orion build needed) | [tools/mock_orion_ble.py](tools/mock_orion_ble.py) |
| Hand-test USB CDC firmware update against real Ori hardware | [tools/mock_orion_ota.py](tools/mock_orion_ota.py) |
| Full authoritative product spec | `Device Description.docx` |
| Catalogue of every UI screen / state | Open `Ori_UI_Prototype.html` in a browser |

## UI Prototype Quick-Start

Open [Ori_UI_Prototype.html](Ori_UI_Prototype.html) directly in any modern browser. No server, no build step. Use the left sidebar to switch between every screen and edge case.

## Milestones

> Update checkboxes as work completes. Critical path: M1 → (M2–M5 ∥ M6) → M7 → M8.

- [x] **M1 — Shared BLE contract** *(unblocks everything)* — locked 2026-05-15, spec v1.0 (2026-06-02)
  15 characteristics, passkey bonding, hash-manifest delta reconnect. Full spec: [ble-protocol.md](.claude/rules/ble-protocol.md).

- [x] **M2 — Firmware skeleton on hardware** — locked 2026-05-16
  PlatformIO (16 MB flash, 8 MB PSRAM, 3 MB OTA slots), GT911 touch driver, CH422G expander, NVS, Arduino_GFX RGB panel, LVGL 9.5.0 partial-buffer renderer. `scripts/patch_lvgl_helium.py` stubs out ARM Helium assembly that the Xtensa toolchain can't assemble — runs automatically via `extra_scripts` before every build.

- [x] **M3 — Firmware UI port (offline/mock data)** — locked 2026-05-22
  All screens built with mock data: status bar, profile card, meeting list, clock, Time Off, countdown modal, setup flow, factory reset, media mode, OTA screen, overlays (reconnect, profile detail, ANCS notification). Build state: RAM 39.2%, Flash 24.1% (of 3 MB OTA slot).

- [x] **M4 — Firmware state machine + persistence** — locked 2026-05-23
  Left-panel priority logic, 1 s tick, 5-min pre-meeting alert, Calendar ↔ Media mode toggle with NVS persistence, OTA/reconnect hooks, long-press handlers, ANCS icon registry (23 apps). Post-lock: LVGL 9.5.0, Hanken Grotesk font, mode-toggle crash fix, shortcut button press feedback. Build state: RAM 7.1%, Flash 41.3% (of 3 MB OTA slot). **Deferred to M5:** factory reset `ESP.restart()` inside LVGL callback can trigger DMA ISR cache fault.

- [x] **M5 — Firmware BLE + USB CDC firmware update** — locked 2026-06-29
  GATT server (17 chars, v1.0, including Clock Face) + ANCS client (48-app icon registry, stored as LVGL indexed I1/I2/I4/I8 images via `firmware/img/ANCS_icons/convert_ancs_indexed.py` to keep flash usage down), passkey bonding, dual-connection (Orion + iPhone bond slots), connection-status border, album-art JPEG decode (TJPGD), USB CDC OTA receiver, boot splash screen (Ori wordmark shown immediately on boot while PSRAM/BLE init runs), separate NS/DS ANCS backlog-replay queues + `g_pmeta` sizing fix. `h2zero/NimBLE-Arduino@2.5.0`. **Post-lock:** weather badge + temperature bubble on the profile card (Device Settings `"w"`/`"d"`), 12/24-hour time format toggle (Device Settings `"h"`, `time_format.cpp`), ANCS notification filter (Device Settings `"f"`). Build state: RAM 24.6%, Flash 86.0% (of 3 MB OTA slot).

- [ ] **M6 — Orion PC app (Windows + macOS, one codebase)** *(parallel with M2–M5; supersedes the old M6/M6b WinUI+SwiftUI split, pivoted 2026-07-08 — see `memory.md`; code moved out to the sibling `../Orion/` repo 2026-07-26)*
  Tauri v2 (Rust backend + OS webview — WebView2 on Windows, WKWebView on macOS), built as a single tray-anchored panel — never a full window (`../Orion/.claude/memory.md` "Orion UI model"). UI at `../Orion/src/` (`index.html`/`styles.css`/`app.js`), ported near-verbatim from `../Orion/Orion_UI_Prototype.html/css/js`; backend at `../Orion/src-tauri/src/` (`commands.rs` — Tauri IPC surface; `lib.rs` — tray icon + frameless transparent window). Scaffold, tray/window shell, and full UI port are done and verified in a real build (`npm run tauri dev`). Real `btleplug` BLE central + CBOR sync protocol (`ble-protocol.md`), USB CDC OTA sender (`ota.md`), and calendar integration (XML-import + ICS fetch — Microsoft Graph/OAuth is explicitly not used, see `../Orion/.claude/rules/pc-app.md`) are implemented; check `../Orion/CLAUDE.md`'s own Status section for current detail before assuming this checkbox reflects it precisely. Agents: `orion-sync`, `calendar-integration` (both now live in `../Orion/.claude/agents/`).
  **Known gotcha:** a leading UTF-8 BOM in an HTML/CSS/JS asset silently breaks WebView2's rendering (blank page, no console error) — strip BOMs from any text asset copied into `../Orion/src/`.

- [ ] **M7 — End-to-end integration**
  Full loop: calendar → Orion → BLE → Ori → screen. Cross-subsystem tests, offline cache, factory-reset round-trip. Agent: `integration-qa`.

- [ ] **M8 — Hardening & polish**
  Memory/animation tuning, signed installers, orinari.net landing, factory-provisioning docs, Orion media-mode OS bridge.
