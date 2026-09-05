# Ori — Connectivity Model

Ori has **two logical channels**, both BLE:

| Channel | Direction | Carries |
|---|---|---|
| **BLE Ori Sync Service** (custom UUID, encrypted+bonded, 22 chars) | bidirectional | Profile, photo, meetings, Time Off, time, backlight, factory reset, sync manifest, Controls-mode commands, host volume state, media metadata, media album art, filtered ANCS notification/call relay, lunar holiday list, the ANCS app-passthrough allowlist (`ble-protocol.md` §13), **firmware updates** (`ble-protocol.md` §14) |
| **iPhone/iPad ANCS** (BLE, separate bond) | phone → Ori (+ call actions Ori → phone) | Notification-icon awareness (view-only) + incoming/active call control (Answer/Decline/End) |

No standard BLE HOGP — all Controls-mode interactions ride the custom Ori Sync Service; Orion bridges to OS-level APIs. The mode-toggle is **hidden until Orion has fully synced** — not just whenever the BLE link is down; a bonded reconnect keeps it hidden through the Reconnect-Syncing window too. Full GATT contract: `ble-protocol.md`. Controls-mode UI: `media-mode.md`.

## USB-C: power only

The USB-C cable is always physically attached and carries **wall power, nothing else**. It used to double as the firmware-update channel over USB CDC; that moved to BLE on 2026-08-16 because the enclosure exposes no reachable data port on a deployed unit (`ota.md`). Ori must **never appear as a removable drive**, and there is no customer-facing data path over this cable at all. The internal UART header remains the service-only recovery route — see `hardware.md`.

## Bond policy

Two bonded peer slots: one PC (Orion), one iPhone (ANCS). New bonds only in state-gated pairing windows; outside them an unknown device can connect but can neither bond nor read/write encrypted data. When both are bonded and connected, advertising stops; if one drops, Ori advertises public undirected so it can reconnect. Factory reset clears both slots. Full advertising state machine: `ble-protocol.md` §2.

## 1. PC ↔ Ori (Orion app)

Source: the sibling `../Orion/` repo. See `../Orion/.claude/rules/pc-app.md` for app details.

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

Profile, photo, Time Off, and pairing bonds are cached in NVS and survive power cycles and connection loss. **The meeting list, local time, and weather are RAM-only** — a power cycle clears them. Meetings are re-pushed by Orion on reconnect; the clock is re-supplied by Orion (primary) or the iPhone (secondary backup, see §2); weather is re-pushed by Orion on reconnect and hidden entirely until then (same "don't show what can't be verified" policy — `ble-protocol.md` §6.4). See `meeting-list.md` for why meetings aren't persisted.

**Reconnect:** hash-manifest delta sync — Orion sends SHA-256 of each item; Ori replies with what it needs. Typical reconnect ~300 ms when nothing changed. See `ble-protocol.md` §6.2 for the wire flow and `state-machine.md` for the overlay UX.

**Factory-reset handling:** adv flag `0x01 SETUP` → Orion deletes its bond without connecting; LTK mismatch is the fallback. Both paths stop the reconnect loop until the user re-pairs. See `ble-protocol.md` §7.1–7.2.

## 2. iPhone/iPad ↔ Ori (ANCS)

iPhone or iPad — ANCS is Apple-proprietary; Android is explicitly out of scope. Both product lines expose the identical GATT surface Ori relies on (ANCS, Device Information Service's Model Number String, Battery Service, Current Time Service), so the same bond slot, pairing flow, and firmware/Orion code paths serve either device — see `ble-protocol.md`'s `iphone_model_map` note and `provisioning.md`-style read-only resolution. **UI text adapts to whichever is actually bonded**: the generic "iPhone or iPad" before any device has connected long enough to report its Model Number String (setup Step 3, the runtime re-pair screen), then the specific "iPhone" or "iPad" everywhere else once known — `ancs_client::phone_kind_word()` on Ori, the matching `phoneKindWord()` helper in Orion's `app.js`, both a plain prefix check on Apple's own iPhone*/iPad* naming, no extra wire state.

- The bonded phone and PC connectivity are completely independent.
- ANCS provides **notification icons** in the status bar. Tapping an icon opens a full-screen overlay (title + body); dismissed via **Close** button only. Message/content notifications are view-only — no composing or sending a reply. The one exception is call control: a still-ringing or active call's overlay instead shows Answer/Decline (or End call), since ANCS exposes those as a binary accept/reject action on the notification itself, not authored content (`modal_incoming_call`, `ancs_client::answer_notification`/`dismiss_notification`). See `product-intent.md`.
- Icons appear and disappear based on unread notification state.
- A **phone icon** is always visible in the status bar: always the neutral colour — disconnected adds a diagonal slash across the glyph rather than swapping to danger red, so the state reads correctly regardless of red/grey colour perception (`memory.md`). ANCS notification icons are hidden while disconnected.
- Tap (or long-press) the phone icon: routing is by **bond state**, not connection state — **bonded (connected or not) → iPhone Info/Stats modal** (name, device type/model subtitle, connection dot, live signal bars + battery level, missed-calls/messages/notifications counts, and its own Unpair button); **not bonded → re-pair iPhone screen** (a stale bond is wiped automatically so the iPhone slot opens). Available from any runtime state.
  - **Tap-to-drill-down (on-device):** tapping any of the three count tiles (connected, non-zero) opens a scrollable list of that category's live notifications, grouped by (app, title) into one row per sender with a stacked-count badge, newest first. Tap a row for the same full-screen detail overlay the status-bar icons use (`modal_ancs_notification::open_for_uid` — stacked groups get "Read all"; a lone notification gets its normal pos/neg buttons). **Swipe a row left** to clear it (or its group) directly — same fade-with-drag affordance as `gestures.md`, no confirmation. **Filter-gated:** tile counts and list both come from the same `ancs_filter`-gated counting rule (`ancs_client::count_filtered_stats()`/`passes_current_filter()`) as the status-bar icons and the Orion relay, so a badge always matches the list it opens (every notification entering these lists is filter-checked — no exceptions). Ringing/active calls never appear as a list row (their own `modal_incoming_call` UI). **Live while open** (`modal_ancs_list::refresh_active()`): rebuilds on a filter change or any notification arriving/leaving (iPhone, Orion, swipe-delete, FIFO eviction) — never stale while viewed.
  - Orion's own mirror offers the identical drill-down remotely, filter-gated the same way end to end: both the tile counts (`PhoneBondStatus`, char `000F`) and list content (chars 16–18) come from the same `ancs_filter` gate, so Orion's badge and list can never disagree. A filter change pushes updated `PhoneBondStatus` immediately, not just on the next queue event. Orion writes actions back the same way an on-device tap would. See `ble-protocol.md` §13 and `pc-app.md`.
- On connect, Ori reads the iPhone's GAP Device Name (0x2A00) and model (Device Information Service, 0x180A/0x2A24) over the encrypted link (e.g. "Xander's iPhone" / "iPhone 15 Pro") and shows both in the iPhone Info/Stats modal. RAM only, but both **survive a runtime disconnect** — a later reopen of the iPhone Info modal still shows the last-known name/model rather than blanking it, since a device's identity doesn't change for a given bond (same reasoning Orion applies to its own cached copy of the model, `ble-protocol.md`'s `PhoneBondStatus.d`). Cleared only when the bond is actually unpaired (`ancs_client::clear_phone_identity()`), not on every disconnect. Live link stats shown alongside them (signal bars, battery) are NOT cached this way — those reset to their unverified state (0 / all-grey) on disconnect, same "don't show what can't be verified" policy as everywhere else.
- **Phone-derived overlays close when the phone link drops** (2026-09-05). On the connected → disconnected edge, `ancs_client::on_iphone_disconnected()` tears down the ANCS drill-down list, any notification detail overlay, the call banner (already the case), and an open Low Battery warning (`modal_device_alert::close_low_battery_alert()`). What these have in common is that their *content* stops existing with the link — rows for notifications that are gone, action buttons that would be no-ops, a battery percentage that can no longer be read. **Two deliberate exemptions:** the Weather Alert (it comes from Orion's weather poll, nothing to do with the phone), and the **iPhone Info/Stats modal**, whose subject is the bond itself rather than any one live reading — "disconnected" is a state it can legitimately display, so it stays open and shows it (`modal_iphone_info::set_connected(false)`, driven from `state_machine::set_phone_connected()`: grey dot, "Disconnected" label, zeroed battery/signal bars, stat badges rebuilt non-clickable, cached name/model retained).
  - **On the edge, not the state.** Bonded-but-disconnected still opens the Info modal on a phone-icon tap (above), so closing on the standing `!connected` condition would make it unreachable offline. Orion mirrors this exactly — `closeAncsSurfacesOnDrop()` fires from `setPhoneBondStatus` only when `wasConnected && !status.c`, and from `setConn`'s disconnected branch when the *Ori* link is what dropped (either one makes the relayed content unverifiable). Orion's Low Battery modal is closed by `checkLowBattery()` on both paths instead, so the flush-pending-reminder step isn't skipped, and its iPhone Info modal is exempt for the same reason Ori's is — `setPhoneBondStatus` re-renders it in place via `openIphoneInfoModal()`.
- **Secondary time source.** Ori reads the iPhone's **Current Time Service** (0x1805 / Current Time 0x2A2B) — iOS exposes it to bonded peers. On initial connect, Ori seeds its clock from it only if Orion hasn't already set the time. While the iPhone stays connected and Orion is absent, Ori re-reads CTS **every 10 minutes** to correct ESP32 RTC drift. Orion's Time Sync (also every 10 min) always overrides CTS when Orion is connected. Display-only — RAM, never persisted; the CTS value is local time (no tz), so Ori runs on UTC0 while iPhone-sourced (no meetings exist in that state, so the meeting epoch logic is unaffected).
