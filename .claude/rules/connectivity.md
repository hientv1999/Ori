# Ori — Connectivity Model

Ori has **three logical channels**:

| Channel | Direction | Carries |
|---|---|---|
| **USB CDC** (USB-C power cable) | Orion → Ori | Firmware update only — see `ota.md` |
| **BLE Ori Sync Service** (custom UUID, encrypted+bonded, 18 chars) | bidirectional | Profile, photo, meetings, Time Off, time, backlight, factory reset, sync manifest, Controls-mode commands, host volume state, media metadata, media album art, filtered ANCS notification/call relay (`ble-protocol.md` §13) |
| **iPhone ANCS** (BLE, separate bond) | iPhone → Ori (+ call actions Ori → iPhone) | Notification-icon awareness (view-only) + incoming/active call control (Answer/Decline/End) |

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
| Orion running and synced | Authoritative. Refreshes calendar, Time Off, profile, time. |

**Data synced by Orion:**
- Full name and job title
- Profile photo
- Today's meeting list
- Next Time Off entry (start, end, destination image) — only the next entry, not full history
- Current local time
- Weather condition + temperature (icon and text on the profile photo — `screen-layout.md`)

Profile, photo, Time Off, and pairing bonds are cached in NVS and survive power cycles and connection loss. **The meeting list, local time, and weather are RAM-only** — a power cycle clears them. Meetings are re-pushed by Orion on reconnect; the clock is re-supplied by Orion (primary) or the iPhone (secondary backup, see §2); weather is re-pushed by Orion on reconnect and hidden entirely until then (same "don't show what can't be verified" policy as Presence — `ble-protocol.md` §6.4). See `meeting-list.md` for why meetings aren't persisted.

**Reconnect:** hash-manifest delta sync — Orion sends SHA-256 of each item; Ori replies with what it needs. Typical reconnect ~300 ms when nothing changed. See `ble-protocol.md` §6.2 for the wire flow and `state-machine.md` for the overlay UX.

**Factory-reset handling:** adv flag `0x01 SETUP` → Orion deletes its bond without connecting; LTK mismatch is the fallback. Both paths stop the reconnect loop until the user re-pairs. See `ble-protocol.md` §7.1–7.2.

## 2. iPhone ↔ Ori (ANCS)

iPhone only — ANCS is Apple-proprietary; Android is explicitly out of scope.

- iPhone and PC connectivity are completely independent.
- ANCS provides **notification icons** in the status bar. Tapping an icon opens a full-screen overlay (title + body); dismissed via **Close** button only. Message/content notifications are view-only — no composing or sending a reply. The one exception is call control: a still-ringing or active call's overlay instead shows Answer/Decline (or End call), since ANCS exposes those as a binary accept/reject action on the notification itself, not authored content (`modal_incoming_call`, `ancs_client::answer_notification`/`dismiss_notification`). See `product-intent.md`.
- Icons appear and disappear based on unread notification state.
- A **phone icon** is always visible in the status bar: always the neutral colour — disconnected adds a diagonal slash across the glyph rather than swapping to danger red, so the state reads correctly regardless of red/grey colour perception (`memory.md`). ANCS notification icons are hidden while disconnected.
- Tap (or long-press) the phone icon: routing is by **bond state**, not connection state — **bonded (connected or not) → iPhone Info/Stats modal** (name, connection dot, live signal bars, missed-calls/messages/notifications counts, and its own Unpair button); **not bonded → re-pair iPhone screen** (a stale bond is wiped automatically so the iPhone slot opens). Available from any runtime state.
  - **Tap-to-drill-down (on-device):** tapping any of the three count tiles (while connected and non-zero) opens a scrollable list of that category's live notifications — grouped by (app, title) into one row per sender/conversation with a stacked-count badge, newest group first. Tap a row to open the same full-screen detail overlay the status-bar icons use (`modal_ancs_notification::open_for_uid` — stacked groups get "Read all"; a lone notification gets its normal dynamic pos/neg action buttons). **Swipe a row left** to clear it (or its whole stacked group) directly from the list — same fade-with-drag affordance as the swipe gestures documented in `gestures.md`, no confirmation. **Filter-gated:** the tile counts and the list show only notifications that pass the current `ancs_filter` — one shared counting rule (`ancs_client::count_filtered_stats()`, with the same `passes_current_filter()` gate inside `list_bucket_groups()`), identical to the ambient status-bar icons and the Orion relay, so a tile badge always matches the list a tap on it opens. (Policy changed 2026-07-11: the on-device drill-down previously bypassed the filter on purpose; that exception is revoked — the filter now applies to every notification entering these lists.) Ringing/active calls never appear as a list row (they have their own live `modal_incoming_call` UI). **Live while open** (`modal_ancs_list::refresh_active()`): an open list rebuilds itself on a filter change, a new notification arriving, or one leaving — from the iPhone, from Orion, from the list's own swipe-to-delete, or from FIFO eviction — so it never goes stale while the user is looking at it.
  - Orion's own mirror of this modal offers the identical drill-down remotely, filter-gated end to end the same way: both the tile counts (`PhoneBondStatus`, char `000F`, via `push_phone_stats()`) and the underlying list content (chars 16–18) are computed from only the notifications that currently pass `ancs_filter` — the same `passes_current_filter()` gate, so Orion's badge and its own drill-down list can never disagree. A filter change pushes updated `PhoneBondStatus` immediately (`ancs_client::set_filter()`), not just on the next queue event. Orion writes actions back the same way an on-device tap would. See `ble-protocol.md` §13 and `pc-app.md`.
- On connect, Ori reads the iPhone's GAP Device Name (0x2A00) over the encrypted link (e.g. "Xander's iPhone") and shows it in the Unpair modal. RAM only — cleared on disconnect.
- **Secondary time source.** Ori reads the iPhone's **Current Time Service** (0x1805 / Current Time 0x2A2B) — iOS exposes it to bonded peers. On initial connect, Ori seeds its clock from it only if Orion hasn't already set the time. While the iPhone stays connected and Orion is absent, Ori re-reads CTS **every 10 minutes** to correct ESP32 RTC drift. Orion's Time Sync (also every 10 min) always overrides CTS when Orion is connected. Display-only — RAM, never persisted; the CTS value is local time (no tz), so Ori runs on UTC0 while iPhone-sourced (no meetings exist in that state, so the meeting epoch logic is unaffected).
