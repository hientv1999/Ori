---
paths:
  - "PC_app/**"
---

# Ori — Orion PC App

**Orion** is the PC companion app that pairs with Ori over BLE — **one codebase for both Windows and macOS**: Tauri v2 (Rust backend + OS webview — WebView2 on Windows, WKWebView on macOS), decided 2026-07-08, superseding an earlier WinUI 3 + SwiftUI split (see `memory.md`). Rust was chosen over a cross-platform framework like Flutter for native BLE access via `btleplug` (no plugin layer) and minimal background-service footprint. The UI (`PC_app/orion/src/`) is identical on both platforms; platform differences live inside the Rust backend (`PC_app/orion/src-tauri/`) behind `#[cfg(target_os = ...)]`, calling each OS's native APIs directly (WinRT Bluetooth/GSMTC/WASAPI on Windows, CoreBluetooth/MediaRemote/CoreAudio on macOS). Platform-specific calls below are marked **Win:** / **mac (planned):**. Install URL shown during setup: `ori.app/orion`. App icon: the Ori wordmark.

A reference implementation of everything below (minus real calendar/Graph/OS integration) lives in `tools/mock_orion_ble.py` (BLE sync) and `tools/mock_orion_ota.py` (USB CDC firmware update) — runnable proof of one valid implementation when in doubt about wire behavior.

## Role

Orion is the authoritative data source for Ori. While running and synced, it pushes:

- Profile (name, job title, email, phone, photo)
- Today's meeting list
- Next Time Off entry (start, end, destination image)
- Current local time + timezone
- Microsoft Teams presence
- Shortcut icon assignments (Controls mode)
- Clock face preference (Digital/Analog)
- ANCS notification filter level

Orion also **reads** from Ori:
- **Firmware Revision String** characteristic (BLE SIG standard Device Information Service, `0x180A`/`0x2A26` — `ble-protocol.md` §3.1): plain UTF-8 semver, how Orion learns the running firmware version. Compared against the latest `ori.app` release for the optional "Install update" prompt; re-read after a successful update. Version check rides BLE; the image transfer is USB CDC only (`ota.md`).
- **Device Settings** characteristic (char `000E`): read once on every (re)connect to recover `clock_face` and `ancs_filter` — the two NVS-persisted values whose ground truth lives on Ori — so Orion's settings UI is correct without caching what it last wrote. See §6.4 in `ble-protocol.md`.

When Orion is not running, Ori falls back to cached data. See `connectivity.md` for the sync state model and `setup-flow.md` for pairing.

## What the PC App Must Implement

- **BLE central role**: discover, pair (6-digit passkey confirmation UI), maintain a bonded connection. Negotiate ATT MTU to 247 bytes, falling back to 23 if refused. Persist bond records.
  - **First-pair handshake timing**: a valid `SyncControl{op:"BEGIN"}` within ~5 s of bonding is Ori's proof the bonded peer is genuinely Orion (`ble-protocol.md` §6.1) — Ori commits the bond address on that BEGIN and drops any peer that never sends one in time. Orion must kick off the initial sync immediately after the passkey is confirmed, not lazily.
- **Initial + delta sync (hash-manifest)**: implement `ble-protocol.md` §6.1/§6.2 — first pair writes everything inside one `SyncControl{BEGIN..END}` session; every reconnect writes a `SyncManifest` of SHA-256 hashes per section, reads back which sections Ori needs, sends only those (Time Sync and Device Settings ride along unconditionally outside BEGIN/END, like the mock's `run_sync()`). Stream Profile Photo / Meeting List / Time Off Entry / Media Album Art **Write-No-Response** with a Write-with-response checkpoint every ≤ 32 fragments (~7.6 KB window) — `ble-protocol.md` §5.
- **Calendar data**: read today's meetings, push to Ori. Cancelled meetings removed; past meetings drop off the device automatically. Hash-check against the manifest before resending (§6.3: on calendar event or every 15 min).
- **Time Off**: push the next Time Off entry with destination metadata and image, resized to **528×396** JPEG, hard cap 512 KB (`ble-protocol.md` §10).
- **Time sync**: push current local time on every sync, and again every 10 min while connected (§6.3). The `tz` field is a **POSIX TZ string, not an IANA name** — Ori feeds it straight into `setenv("TZ", tz)`/`tzset()`, which silently falls back to UTC on `"America/Los_Angeles"`-style strings. Derive a fixed-offset POSIX string from the local UTC offset (e.g. `"PST8"`, `"LOC-2"`) — see `_local_posix_tz()` in `tools/mock_orion_ble.py`. No DST rule needed since Orion re-syncs periodically.
- **Microsoft Teams presence**: read via Microsoft Graph (`GET /me/presence`), push via **Device Settings** (char `000E`, key `"p"`, `ble-protocol.md` §3/§6.4). Teams → Ori mapping:
  - `Available` → `0x00`
  - `Busy`, `DoNotDisturb`, `BusyIdle`, `InACall`, `InAMeeting`, `InAConferenceCall`, `Presenting`, `UrgentInterruptionsOnly` → `0x01`
  - `Away`, `BeRightBack`, `AwayIdle` → `0x02`
  - `Offline`, `PresenceUnknown`, `OffWork`, `OutOfOffice` → `0x03`
  - **Push a fresh value immediately on every (re)connect** — Ori doesn't return presence on a Device Settings read (ephemeral; Orion is sole source of truth). On BLE drop, Ori immediately displays Offline regardless.
  - Cadence: subscribe to Graph change notifications (`/subscriptions` on `/communications/presences/{id}`); fall back to ~60 s polling. Write to BLE only when the mapped byte changes.
  - OAuth: user signs into Microsoft during setup, grants `Presence.Read`. Cache the refresh token in OS-native secure storage (**Win:** Credential Locker; **mac, planned:** Keychain). On expiry/revocation: write `0x03 OFFLINE`, surface re-auth prompt.
  - No Graph presence data (non-corporate accounts, Teams not installed): write `0x03 OFFLINE` silently. The setup wizard makes this step skippable.
- **Profile management**: capture name, job title, email, phone, photo; push on pairing and on change. **Enforce input limits in the UI** — name/title/email ≤ 32 chars, phone ≤ 16 (live counter, blocks at limit — a frontend concern, e.g. `app.js`'s `cc()`; not re-validated by the Rust backend, which trusts its own frontend as the sole caller). Resize the profile photo to **228×228** JPEG, hard cap 200 KB (`ble-protocol.md` §10, which also has the wire-level byte caps).
- **Clock face preference**: settings subscreen (Digital/Analog), Save/Discard header pattern. On Save, writes Device Settings (`"c"`) only on change (Ori persists to NVS). Orion reads it back on every (re)connect.
- **ANCS notification filter**: settings subscreen (Disabled/Call Only/Important/All), same Save/Discard pattern. On Save, writes Device Settings (`"f"`) only on change. Mapping: `0`=Disabled, `1`=Call Only (IncomingCall only), `2`=Important (IncomingCall or ANCS Important flag), `3`=All (default).
- **Ori Info/Stats modal**: tapping the header device name/connection state opens a read-only snapshot — name, connection dot, live signal bars, firmware version, Bluetooth address, serial number, manufacture date, iPhone bond, last-synced time. Merges `get_ori_info` (Rust, cached-only) with a live `read_device_settings` (char `000E`) read for signal bars/serial/manufacture date (piggybacked there — `ble-protocol.md` §4/§6.4). Serial/manufacture date are cached client-side so a disconnect doesn't blank them; signal bars are NOT cached (0 bars while disconnected — "don't show what can't be verified"). **Live while open**: a ~3 s poll timer re-fetches (Device Settings has no Notify, so this is the one polled modal), plus immediate push-driven refreshes off `setConn()` and `setPhoneBondStatus()`. See `provisioning.md` for how serial/manufacture date get onto the device and why they survive a firmware update and a factory reset.
- **iPhone Info/Stats modal + ANCS drill-down**: tapping the header phone icon (bonded, connected or not) opens a snapshot mirroring Ori's own — name, connection dot, live signal bars, missed-calls/messages/notifications counts (`PhoneBondStatus`, char `000F`), Unpair. Counts arrive already filtered through `ancs_filter` (same gate as Ori's on-device tiles); Orion just displays them, no local filter logic. **Live while open** (2026-07-11): every `PhoneBondStatus` notify, including the ~5 s RSSI poll, re-renders an already-open modal (`setPhoneBondStatus()` re-invoking `openIphoneInfoModal()`, idempotent). Tapping a non-zero count opens a filtered list of the underlying notifications (chars 16–17, §13) — already filtered by Ori, so badge and list are always consistent. **Resynced on every Orion (re)connect** (`resync_orion_relay()`/`resync_orion_call_state()`): chars 16/17 are notify-only with no read/replay of their own, so without a resync a notification or live call present before Orion connected would be counted in the aggregate but never reach Orion's local mirror. Fired from Ori's `onSubscribe()` GATT callback, not the earlier `OrionConnected` event (which fires before `run_sync` starts — a resync that early is dropped, not yet subscribed).

  **Reconnect resync race (found 2026-07) and its fix.** `onSubscribe` can fire two ways: Orion's own CCCD write, or — on a **bonded reconnect** — NimBLE's host stack itself (`ble_gatts_bonding_restored()`) restoring every persisted CCCD the instant encryption resumes, entirely server-side, no write from Orion needed. That second trigger is near-instantaneous, firing before Orion's soonest possible `subscribe()` (which still has to wait through connect → GATT discovery → an encrypted read) can win the race — confirmed on hardware: Ori's resync sent everything correctly while Orion's receiver was still not alive. Reordering on Orion's side alone cannot close this. **The real fix is on the firmware side**: `state_machine::on_reconnect_end()` fires only on a genuine reconnect's `SyncEnd` — causally guaranteed to be *after* Orion has subscribed, since Orion's `run_sync()` always subscribes to chars 0010/0011 before sending `SyncControl{END}` — and calls `resync_orion_relay()`/`resync_orion_call_state()` a second, unconditional, idempotent time. Orion's own `subscribe_ancs_relay_early()` (`central.rs`, subscribing before `run_sync` starts in both `reconnect()` and `submit_passkey`) is kept as a genuine improvement — it removes the extra latency Orion's old code added and guarantees the receiver is alive in time for the firmware-side second resync to land.

  Tapping a row opens a detail view (silent badge top-left, close top-right, dynamic Answer/Decline/Dismiss/Read-all buttons built from the notification's own `pos_label`/`neg_label`/`has_neg_action` — never hardcoded) or, for a ringing/active call, the call view instead. **If Ori reports the open detail's notification removed** (`{op:"remove"}`) while on screen, Orion closes it and returns to the list. Notifications sharing app+title collapse into one row/detail with a stacked-count badge and a single "Read all". Writes `AncsNotificationAction` (char `0012`) on every action tap; never updates its own UI optimistically — waits for the resulting notify.
- **Incoming call takeover**: `AncsCallState{st:1}` (char `0011`) brings the Orion window to the foreground (show + focus + un-minimize) and shows the ringing view. Never fires when `ancs_filter` is Disabled (the notify simply never arrives, so Orion needs no filter check of its own). `st:2` (active) resumes the in-call view with its timer seeded from the relayed elapsed seconds. `st:0` closes whatever call view is open. Both the ringing/in-call view and the header call chip render the calling app's real icon from `"k"` (§13), falling back to a generic glyph only when unrecognised. A call already live before Orion connects shows immediately via Ori's call-state resync.
- **Header call chip**: while a call is ringing/active, a quick-access chip with the calling app's icon appears left of the phone icon; clicking it reopens the ringing/in-call view. Hidden with no live call. The in-call timer runs independently of whether the modal is open. **Ring color mirrors Ori's own status-bar call tile**: solid yellow (`st:1`, `.ringing`) while unanswered, solid red (`st:2`, `.on-call`) once active — `styles.css`'s ring colors are pixel-matched to firmware `theme.h`'s `COLOR_CALL_RINGING`/`COLOR_DANGER`.
- **Close ANCS/call surfaces on link loss**: on a sustained Ori disconnect (`conn-state: "off"`), Orion force-closes every ANCS-derived surface (drill-down list, detail overlay, ringing/in-call view, iPhone Info hub) since their content can no longer be verified or acted on. Not done on transient Connecting/Syncing phases — those are reconciled by Ori's own resync, so a still-live call/notification reappears automatically rather than flickering closed-then-reopened.
- **Background operation**: Orion has **no main window** — only a compact, movable, frameless panel (`memory.md`'s "Orion UI model"). Auto-opens on launch, stays visible until explicitly minimized (in-app button or tray icon), shows a normal taskbar entry while open — does NOT auto-dismiss on click-away. No native title bar/resize; the custom titlebar doubles as the drag handle. Implemented via Tauri's `tray-icon` feature. The app stays running and synced whether or not the panel is open. Option to start at login (`tauri-plugin-autostart`).
- **Passkey entry UI (Windows)**: custom in-app modal — six single-digit boxes with auto-advance focus and paste support (`setup-flow.md`). Ori displays the 6-digit passkey; the user types it into this modal, which drives WinRT's `DeviceInformationCustomPairing` (`PairingRequested` → `ProvidePin`) — Orion is the entry side, Ori the display side. **macOS has no equivalent** (CoreBluetooth exposes no app-level pairing UI hook) and is deferred, not designed, until that build (M6b) starts.
- **Reset**: single settings button, gated by one confirm dialog offering two destructive actions:
  - **Factory Reset** — writes the Factory Reset Command over the bonded link; Ori wipes its NVS + bonds and reboots into setup, Orion drops its local bond record (`ble-protocol.md` §7.2). Orion's local profile cache is untouched.
  - **Clear All** — does the above, plus clears Orion's local cache (profile, calendar sign-in, shortcut config), then walks the user back through first-run setup.
- **Firmware update (optional, user-initiated)**: polls `ori.app`, compares to `fw_version` from the Firmware Revision String characteristic (`ble-protocol.md` §3.1), shows "Install update" when newer exists. Full sender algorithm is in `ota.md`; `tools/mock_orion_ota.py` covers all 9 failure scenarios. If no serial port: prompt "Plug Ori into this PC, then try again."
- **Controls-mode OS bridge + state push**: implement per `ble-protocol.md` §12. Subscribe to `Keyboard Command` notifies; translate each op to the OS API; after `vol_set` always write `HostVolumeState{level:N}` back to Ori. Shortcut actions (exactly 14): `vol-mute`, `mic-mute`, `screenshot` (UI label "Snip Tools", wire token unchanged), `lock-screen`, `favorite-1`/`favorite-2`/`favorite-3` (independently user-configured, own combo each), `calculator`, `copy`/`cut`/`paste`/`undo`/`redo`/`save` (Ctrl+C/X/V/Z/Y/S). **mac (planned):** `play_pause`/`prev`/`next` need Accessibility permission on first launch; `seek`/track-change use private `MediaRemote` — re-verify per OS version. State push: on (re)connect push current `HostVolumeState` + track (or empty); on OS-callback change (volume debounce ~100 ms; track change writes `MediaMetadata` + art resized 484×216 JPEG chunked to `MediaAlbumArt`). Push `playing` on every track change AND play/pause toggle. If position deviates > ~5 s from dead-reckoned while playing, push corrected `MediaMetadata` with `position_s`/`duration_s`.
- **Media-mode shortcut configuration UI**: settings subscreen with three slot rows (dropdown of the 14 actions + icon preview), same Save/Discard pattern. Selecting a **Favorite N** reveals that slot's own combo recorder (per-slot, not per-token — e.g. Favorite 1 in Slot 2 and Slot 3 record independently): click to arm, press the combo, Esc to cancel. **Favorite-empty guard:** Save stays disabled while any slot is a Favorite with no combo recorded. Slot tokens write to **Device Settings** (`"1"/"2"/"3"`) outside BEGIN/END, persisted to NVS on Ori; Orion reads them back on every (re)connect, writes only on change.

## Panel layout

Orion's only UI surface is a 348 px wide tray-anchored flyout panel, split into two visibility zones:

### Always visible (connected and disconnected)
- **Profile card** — circular presence-bordered photo (228×228 px), name, presence badge. Tapping opens the profile editor subscreen (Save/Back; live character counters at the Orion-enforced caps).
- **Time Off** — divider-separated; toggle + collapsible destination card (image, destination, date range). Tapping opens the Time Off editor subscreen.
- **Firmware update indicator** — header icon shown only when a newer version is available; tapping opens the update flow.

### Visible when Connected or Disconnected — hidden only during Connecting/Syncing
These four rows and subscreens (2026-07-11: revised from "connected and synced only") hide only during **Connecting**/**Syncing** — before the sync that would make their values trustworthy has finished. A gated subscreen left open when either state starts is force-closed (`dismissTransientScreen()`).
- **Notification Filter**, **Clock Face**, **Time Format** (24h/12h) — single-select subscreens, Save/Discard.
- **Quick Actions** — three-slot subscreen with per-slot combo recorder for Favorite slots.

**Disconnected differs from Connecting/Syncing: rows stay visible and editable.** The user can Save with no live link — Orion just can't confirm against Ori yet. `save_device_settings` persists the pending value to Orion's local store first (`store::SavedState::pending_*`, survives an app restart) and best-effort attempts the live write, swallowing failure (offline editing is the intended path, not an error). `run_sync` flushes any pending value on the next successful reconnect, alongside the unconditional shortcuts push, then clears the pending flag. Quick Actions needs none of this — its tokens already push unconditionally on every reconnect regardless of dirty-state.

On transition into Connected, Orion re-reads Device Settings (§6.4) to repopulate the rows — by then any pending edit has already been flushed by `run_sync`, so the read reflects the just-delivered value.

### Settings subscreen UX convention (Save/Discard)
- Header: **Back** (left) · title (centre) · **Save** (right, disabled until a change is made).
- Selecting an option/recording a combo arms Save but doesn't apply immediately.
- Back/Esc with no pending change: returns immediately.
- Back/Esc with a pending change: "Discard changes?" modal — **Keep editing** stays with the pending change; **Discard** reverts and returns without writing.
- **Save**: commits, writes to Ori via Device Settings, returns to the panel.

Profile and Time Off subscreens use the same convention but are accessible regardless of connection state.

## Cross-Reference

BLE protocol (services, characteristics, payloads) is a shared contract with the firmware — changes here must be reflected there. See `ble-protocol.md` and `firmware.md`. Firmware update transport and the sender algorithm are in `ota.md`.
