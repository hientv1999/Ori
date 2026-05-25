# Ori — Connectivity Model

Ori has **three logical channels** carrying distinct kinds of traffic:

| Channel | Direction | Carries |
|---|---|---|
| **USB CDC** (over USB-C, always-present power cable) | Orion → Ori | Firmware update only |
| **BLE Ori Sync Service** (custom UUID, encrypted+bonded, v1.2 = 15 chars) | bidirectional | Profile, photo, meetings, PTO, time, backlight, factory reset, sync manifest, **Controls-mode commands**, **host volume state**, **media metadata**, **media album art** |
| **Phone ANCS** (BLE, separate bond) | Phone → Ori | Notification-icon awareness (read-only, icons only — no content) |

There is no standard BLE HID Over GATT (HOGP) profile in v1.2 — all Controls-mode interactions ride the custom Ori Sync Service, with Orion bridging them into OS-level HID/volume/now-playing APIs at runtime. This is a hard dependency on Orion running, consistent with the existing same dependency for calendar/profile/PTO sync. The status-bar mode-toggle that lets the user enter Controls mode is **hidden whenever the BLE link to Orion is down**, so the user is never offered a mode that wouldn't work. See `ble-protocol.md` §12 for the bridging model and `keyboard-mode.md` for the offline-hide rule.

The full BLE GATT contract is in `ble-protocol.md`; the USB-C firmware-update path is in `ota.md`; the Controls-mode UI is in `keyboard-mode.md`. This file captures the higher-level connectivity model and behavioral rules.

## The USB-C cable is data, not just power

Ori is USB-C wall-powered (some units carry an optional LiPo as brief-blackout backup, but the firmware never assumes it — see `hardware.md`). So during normal operation the cable is physically attached to the device. Treat USB-C as **always-present power + opportunistic data**: when the other end is plugged into the Orion PC, the same cable becomes the firmware-update channel via USB CDC. When the other end is in a wall adapter or a non-Orion PC, only power flows; firmware updates wait until the user plugs Ori into the Orion PC. There is no removable-drive fallback, by deliberate UX choice — Ori must not appear in Explorer / Finder. This is why firmware updates run over USB CDC and not BLE — see `ota.md` and the `ble-protocol.md` v1.1 changelog.

## Bond policy

Ori accepts **at most two bonded peers**: one PC (Orion) and one phone (ANCS). Once both slots are filled, Ori stops public advertising and uses **directed advertising only** to the bonded peer addresses. Unknown devices cannot scan or connect — this is both power-saving and a security boundary.

Until both slots are filled, Ori advertises publicly so the missing peer can pair. Factory reset clears both bond slots, returning Ori to a fresh public-advertising state.

## 1. PC ↔ Ori (Orion App)

The Orion app is the PC companion (Windows/macOS). Its source lives under `PC_app/` in this repository — see `pc-app.md` for build details and `firmware.md` for the embedded counterpart.

Two distinct sub-states:

| Sub-state | What it means |
|---|---|
| BLE connected only | Bluetooth link exists but Orion is not running/synced. Cached data may be displayed but nothing is refreshed. |
| Orion running and synced | Authoritative source. Refreshes calendar, PTO, profile, and local time. |

Data synced by Orion:
- Full name and job title
- Profile photo
- Today's meeting list
- Next PTO entry (start datetime, end datetime, destination image)
- Current local time
- Brightness level (bidirectional — Orion's slider and Ori's two-finger gesture both update the same value)

All data is cached in flash after pairing and survives power cycles and connection loss. Only the **next** PTO entry is stored (not full future history).

### Reconnect sync model — hash manifest, not dirty flags

When Ori reconnects to Orion, Orion does **not** track which fields changed in memory (that's fragile). Instead it computes a SHA-256 of each data item from what it currently has and sends a manifest. Ori (the source of truth for its own NVS cache) compares against its stored hashes and replies with which items it actually needs re-pushed. Orion then pushes only those.

Behavioral implications:
- Typical reconnects complete in ~300 ms (manifest exchange only). The reconnect overlay on Ori auto-dismisses fast.
- When the photo or a meeting changed, the overlay stays only as long as that specific transfer takes.
- Self-healing: if Ori's cache or hashes drift for any reason, the next reconnect detects the mismatch and re-pushes automatically.

See `ble-protocol.md` §6.2 for the wire-level flow and `state-machine.md` for the Reconnect-Syncing overlay UX.

### Factory-reset detection on reconnect

If Ori has been factory-reset (locally via long-press or remotely via Orion's reset button), but Orion still holds a bond record for it, Orion must detect this and clean up its own bond before retrying:

1. **Preferred:** Orion's scanner reads Ori's advertising mode flag. If it's `0x01 SETUP` (fresh state), Orion deletes its bond record without attempting an encrypted connection and surfaces a re-pair prompt to the user.
2. **Fallback:** if Orion attempts a bonded reconnect anyway, link-layer encryption fails because Ori no longer holds the LTK. Orion catches that specific error, deletes its bond, and falls back to path 1.

In both cases Orion must **stop the reconnect loop** for that Ori until the user explicitly re-pairs. No silent retry storms.

### Remote factory reset

Orion exposes a "Factory reset Ori" button in its settings. The flow:

1. Orion shows a confirmation dialog.
2. On confirm, Orion writes the Factory Reset Command (a 4-byte magic value) over the encrypted/bonded link.
3. Ori validates the magic, wipes NVS + both bonds, reboots into first-boot setup.
4. Orion sees the disconnect, deletes its own bond record, and prompts the user to re-pair.

The local long-press-photo factory reset on Ori produces identical behavior. Both paths converge in the same firmware routine.

## 2. Phone ↔ Ori (ANCS)

- Phone connectivity is completely independent of PC connectivity.
- ANCS provides **notification icons** in the status bar. Tapping an icon opens a full-screen detail overlay showing the notification title and message body. Dismissed via the **Close** button only. No replying — ANCS is read-only.
- Icons appear and disappear based on unread notification state.
- When phone is not connected: show the phone-disconnect icon (phone + diagonal slash) in the status bar right area.
- When phone is not connected: hide all ANCS notification icons.
- Long-press the phone-disconnect icon for 3 seconds to re-initiate phone pairing from any runtime state.
