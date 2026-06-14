# Ori — Connectivity Model

Ori has **three logical channels**:

| Channel | Direction | Carries |
|---|---|---|
| **USB CDC** (USB-C power cable) | Orion → Ori | Firmware update only — see `ota.md` |
| **BLE Ori Sync Service** (custom UUID, encrypted+bonded, 16 chars) | bidirectional | Profile, photo, meetings, PTO, time, backlight, factory reset, sync manifest, Controls-mode commands, host volume state, media metadata, media album art |
| **iPhone ANCS** (BLE, separate bond) | iPhone → Ori | Notification-icon awareness (read-only) |

No standard BLE HOGP — all Controls-mode interactions ride the custom Ori Sync Service; Orion bridges to OS-level APIs. The mode-toggle is **hidden whenever the BLE link to Orion is down**. Full GATT contract: `ble-protocol.md`. Controls-mode UI: `media-mode.md`.

## USB-C: power + opportunistic data

The USB-C cable is always physically attached (wall power). When the other end is plugged into the Orion PC it also becomes the firmware-update channel via USB CDC. Ori must **never appear as a removable drive**. See `ota.md` and `hardware.md`.

## Bond policy

Two bonded peer slots: one PC (Orion), one iPhone (ANCS). New bonds only in state-gated pairing windows; outside them an unknown device can connect but can neither bond nor read/write encrypted data. When both are bonded and connected, advertising stops; if one drops, Ori advertises public undirected so it can reconnect. Factory reset clears both slots. Full advertising state machine: `ble-protocol.md` §2.

## 1. PC ↔ Ori (Orion app)

Source: `PC_app/`. See `pc-app.md` for app details.

| Sub-state | What it means |
|---|---|
| BLE connected only | Link exists; Orion not running. Cached data displayed; nothing refreshed. |
| Orion running and synced | Authoritative. Refreshes calendar, PTO, profile, time. |

**Data synced by Orion:**
- Full name and job title
- Profile photo
- Today's meeting list
- Next PTO entry (start, end, destination image) — only the next entry, not full history
- Current local time

Profile, photo, PTO, and pairing bonds are cached in NVS and survive power cycles and connection loss. **The meeting list and local time are RAM-only** — a power cycle clears them. Meetings are re-pushed by Orion on reconnect; the clock is re-supplied by Orion (primary) or the iPhone (secondary backup, see §2). See `meeting-list.md` for why meetings aren't persisted.

**Reconnect:** hash-manifest delta sync — Orion sends SHA-256 of each item; Ori replies with what it needs. Typical reconnect ~300 ms when nothing changed. See `ble-protocol.md` §6.2 for the wire flow and `state-machine.md` for the overlay UX.

**Factory-reset handling:** adv flag `0x01 SETUP` → Orion deletes its bond without connecting; LTK mismatch is the fallback. Both paths stop the reconnect loop until the user re-pairs. See `ble-protocol.md` §7.1–7.2.

## 2. iPhone ↔ Ori (ANCS)

iPhone only — ANCS is Apple-proprietary; Android is explicitly out of scope.

- iPhone and PC connectivity are completely independent.
- ANCS provides **notification icons** in the status bar. Tapping an icon opens a full-screen overlay (title + body); dismissed via **Close** button only. No replying — read-only.
- Icons appear and disappear based on unread notification state.
- A **phone icon** is always visible in the status bar: neutral colour when the iPhone is connected, danger red when disconnected. ANCS notification icons are hidden while disconnected.
- Tap (or long-press) the phone icon: **connected → Unpair iPhone modal; disconnected → re-pair iPhone screen** (a stale bond is wiped automatically so the iPhone slot opens). Available from any runtime state.
- On connect, Ori reads the iPhone's GAP Device Name (0x2A00) over the encrypted link (e.g. "Xander's iPhone") and shows it in the Unpair modal. RAM only — cleared on disconnect.
- **Secondary time source.** On connect, Ori also reads the iPhone's **Current Time Service** (0x1805 / Current Time 0x2A2B) — iOS exposes it to bonded peers, like the GAP name. Ori seeds its clock from it **only if Orion (the primary source) hasn't already set the time**, so the status-bar clock works even when Orion is absent (e.g. after a power cycle with the PC off). Orion's Time Sync always overrides it. Display-only — RAM, never persisted; the CTS value is local time (no tz), so Ori runs on UTC0 while iPhone-sourced (no meetings exist in that state, so the meeting epoch logic is unaffected).
