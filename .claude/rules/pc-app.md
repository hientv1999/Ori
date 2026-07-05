---
paths:
  - "PC_app/**"
---

# Ori — Orion PC App

**Orion** is the PC companion app that pairs with Ori over BLE — two separate **native** codebases sharing one protocol contract, not one cross-platform app: **WinUI 3 (Windows App SDK), C#/XAML** under `PC_app/`, building now; **SwiftUI (Swift)** for macOS, planned next but not yet started (see `memory.md`). Both were chosen over a cross-platform framework like Flutter for native BLE access (no plugin layer) and minimal background-service footprint. Everything in this file is the shared feature/protocol spec both implementations must satisfy — platform-specific API calls are called out per bullet as **Win:** / **mac (planned):**. Install URL shown during setup: `ori.app/orion`. App icon: the Ori wordmark.

A reference implementation of everything below (minus real calendar/Graph/OS integration) lives in `tools/mock_orion_ble.py` (BLE sync) and `tools/mock_orion_ota.py` (USB CDC firmware update) — when in doubt about exact wire behavior, those scripts are runnable proof of one valid implementation.

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
- **Protocol Version** characteristic (`{proto_major, proto_minor, fw_version}`): how Orion learns the running firmware version. Compared against the latest release at `ori.app` for the optional "Install update" prompt; re-read after a successful update to confirm. Version check rides BLE; the image transfer itself is USB CDC only (`ota.md`).
- **Device Settings** characteristic (char `000F`): read once on every (re)connect to recover `clock_face` and `ancs_filter` — the two NVS-persisted values whose ground truth lives on Ori. Orion uses these to populate its settings UI correctly without needing to cache what it last wrote across app restarts or BLE drops. See §6.4 in `ble-protocol.md`.

When Orion is not running, Ori falls back to cached data. See `connectivity.md` for the sync state model and `setup-flow.md` for the pairing flow.

## What the PC App Must Implement

- **BLE central role**: discover, pair (6-digit passkey confirmation UI), and maintain a bonded connection to Ori. Negotiate ATT MTU to 247 bytes on connect, falling back to 23 if refused. Persist bond records.
  - **First-pair handshake timing**: a valid `SyncControl{op:"BEGIN"}` sent within ~5 s of bonding is Ori's proof that the bonded peer is genuinely Orion (`ble-protocol.md` §6.1) — Ori commits the bond address on that BEGIN and drops any peer that bonds but never sends one in time. Orion must kick off the initial sync immediately after the passkey is confirmed, not lazily.
- **Protocol compatibility gate (mandatory, automatic)**: on every connect, read **Protocol Version** char *before* the manifest or `BEGIN`. Compare `proto_major` to Orion's implemented major:
  - **Match** → proceed. `proto_minor` difference never blocks (additive, §9).
  - **Mismatch** → skip the sync; automatically run USB CDC firmware update (`ota.md`) to bring Ori to a compatible `proto_major`. If USB not reachable, show "Ori needs a firmware update — connect it to this PC" and retry when the port appears. Once updated, reconnect and re-read Protocol Version.
  - Distinct from the optional user-initiated "Install update" (newer version available). The "BLE backward-compatibility" requirement below ensures Orion can always read Protocol Version and drive a USB update even on an incompatible `proto_major`.
- **Initial + delta sync (hash-manifest)**: implement the unified sync flow in `ble-protocol.md` §6.1/§6.2 — on first pair, write everything inside one `SyncControl{BEGIN..END}` session; on every reconnect, write a `SyncManifest` of SHA-256 hashes per section (profile/photo/meetings/to), read back which sections Ori says it needs, and send only those (Time Sync and Device Settings — shortcuts + presence — ride along unconditionally on every (re)connect outside the BEGIN/END pipeline, like the mock's `run_sync()`). Stream Profile Photo / Meeting List / Time Off Entry / Media Album Art **Write-No-Response** with a Write-with-response checkpoint every ≤ 32 fragments (~7.6 KB window) — see `ble-protocol.md` §5.
- **Calendar data**: read today's meetings from the user's calendar and push to Ori. Cancelled meetings removed; past meetings drop off the device automatically. Hash-check against the manifest before resending (§6.3: on calendar event or every 15 min).
- **Time Off**: push the user's next Time Off entry with destination metadata and image. Resize the destination photo to **528×396**, JPEG, hard cap 512 KB (`ble-protocol.md` §10) before sending.
- **Time sync**: push current local time on every sync, and again every 10 min while connected (`ble-protocol.md` §6.3). The `tz` field is a **POSIX TZ string, not an IANA name** — Ori feeds it straight into `setenv("TZ", tz)`/`tzset()`, which silently falls back to UTC on an IANA string like `"America/Los_Angeles"`. Derive a fixed-offset POSIX string from the local UTC offset (e.g. `"PST8"`, `"LOC-2"`); see `_local_posix_tz()` in `tools/mock_orion_ble.py` for a reference implementation. No DST rule is required since Orion re-syncs periodically.
- **Microsoft Teams presence**: read the signed-in user's presence via Microsoft Graph (`GET /me/presence`) and push via the **Device Settings** characteristic (char `000F`, key `"p"`, `ble-protocol.md` §3/§6.4). Teams → Ori mapping:
  - `Available` → `0x00`
  - `Busy`, `DoNotDisturb`, `BusyIdle`, `InACall`, `InAMeeting`, `InAConferenceCall`, `Presenting`, `UrgentInterruptionsOnly` → `0x01`
  - `Away`, `BeRightBack`, `AwayIdle` → `0x02`
  - `Offline`, `PresenceUnknown`, `OffWork`, `OutOfOffice` → `0x03`
  - **Push a fresh value immediately on every (re)connect** — Ori does not return presence on Device Settings read (presence is ephemeral; Orion is the sole source of truth). On BLE drop, Ori immediately displays Offline regardless (`ble-protocol.md` §6.4).
  - Push cadence thereafter: subscribe to Graph change notifications (`/subscriptions` on `/communications/presences/{id}`); fall back to polling every ~60 s. Only write to BLE when the mapped byte actually changes.
  - OAuth: user signs into Microsoft account during setup and grants `Presence.Read`. Cache the refresh token in OS-native secure storage (**Win:** Windows Credential Locker; **mac, planned:** Keychain). On grant expiry/revocation: write `0x03 OFFLINE` to Ori, surface re-auth prompt in settings.
  - If Graph returns no presence data (non-corporate accounts, Teams not installed): write `0x03 OFFLINE` silently. The setup wizard makes the Teams-presence step skippable.
- **Profile management**: capture name, job title, email, phone, photo; push on initial pairing and on change. **Enforce input limits** — name ≤ 32 characters, job title ≤ 32 characters, email ≤ 32 characters, phone ≤ 16 characters (live counter; field blocks at limit). Resize the profile photo to **228×228**, JPEG, hard cap 200 KB (`ble-protocol.md` §10) before sending. Wire-level byte caps are in `ble-protocol.md` §10.
- **Clock face preference**: settings toggle (Digital/Analog) written to Ori via **Device Settings** (char `000F`, key `"c"`, `ble-protocol.md` §3/§6.4) only when the user changes it — not resent on every reconnect, because Ori persists it to NVS. **On every (re)connect, Orion reads Device Settings** to recover the current `clock_face` value and reflect it correctly in the settings UI — without this read, Orion would have to cache the last value it wrote and risk showing a stale setting after an app restart or BLE drop.
- **ANCS notification filter**: settings control (Disabled / Call Only / Important / All) written to Ori via **Device Settings** (char `000F`, key `"f"`, `ble-protocol.md` §3/§6.4) only when the user changes it — not resent on every reconnect, because Ori persists it to NVS. Like clock face, Orion reads Device Settings on (re)connect to recover the current `ancs_filter` value for its settings UI. Mapping: `0` = Disabled, `1` = Call Only (IncomingCall only), `2` = Important (IncomingCall or ANCS Important flag), `3` = All (default).
- **Background operation**: Orion has **no main window** — its only UI surface is a compact flyout/panel anchored to the system tray icon (`memory.md`'s "Orion UI model"), opened by clicking the icon and dismissed on click-away. No title bar, no resize, no taskbar entry. **Win:** `NotifyIcon` / Windows App SDK tray APIs. **mac (planned):** SwiftUI `MenuBarExtra`. The app stays running and synced whether or not the panel is open — there's no separate "minimized" state to enter, since open-vs-closed panel is the only state that exists. Option to start at user login.
- **Passkey confirmation UI**: during setup, display the 6-digit code from Ori and ask the user to confirm it matches.
- **Factory reset Ori**: settings button (confirm-dialog gated) that writes the Factory Reset Command over the bonded link, then drops Orion's local bond record. See `ble-protocol.md` §7.2.
- **Firmware update (optional, user-initiated)**: polls `ori.app` for the latest version, compares to `fw_version` in Protocol Version char, shows "Install update" when newer exists. Uses the same USB CDC transfer as the Protocol compatibility gate. Full sender algorithm (`OriFwVer=` marker extraction, VID `0x303A` port discovery, DTR assertion, windowed flow control, reject/fail table) is in `ota.md`; `tools/mock_orion_ota.py` covers all 9 failure scenarios. If no serial port: prompt "Plug Ori into this PC, then try again."
- **Controls-mode OS bridge + state push**: implement per `ble-protocol.md` §12 (canonical API table, swipe race conditions, reconnect semantics). Key requirements: subscribe to `Keyboard Command` notifies; translate each op to the appropriate OS API; after `vol_set` always write `HostVolumeState{level:N}` back to Ori. Shortcut actions (exactly 5): `vol-mute` (mute audio), `mic-mute` (mute mic), `screenshot`, `lock-screen`, `favorite` (user-configured). **mac (planned):** `play_pause`/`prev`/`next` require Accessibility permission on first launch; `seek` and track-change use private `MediaRemote` — distribution-safe but re-verify per OS version. State push: on (re)connect push current `HostVolumeState` + current track (`MediaMetadata`, or empty if nothing playing); on change push via OS callbacks (volume debounce ~100 ms; track change writes `MediaMetadata` + art resized 484×216 JPEG chunked to `MediaAlbumArt`). Push `playing` on every track change AND every play/pause toggle. PC-side seek: if position deviates > ~5 s from dead-reckoned while playing, push corrected `MediaMetadata` with `position_s`/`duration_s`.
- **Media-mode shortcut configuration UI**: settings pane where the user assigns each of the three shortcut slots one of the 5 supported icon tokens and a host-side action. The host-side action mapping is local to Orion. The chosen icon token for each slot is written to Ori via **Device Settings** (char `000F`, keys `"1"`/`"2"`/`"3"`, `ble-protocol.md` §3/§12) — outside the BEGIN/END staging pipeline, resent unconditionally on every (re)connect (RAM-only on Ori, no hash, like Time Sync); it's cheap enough that hash-checking isn't worth the protocol complexity.
- **BLE backward-compatibility**: every Orion release must be able to GATT-connect to every prior `proto_major.proto_minor` that has shipped. Rationale: Orion → USB CDC is the only firmware-update path — if Orion can't connect to an older Ori, the user is stuck. If a `proto_major` bump is ever needed, the prior GATT layout must remain supported in Orion alongside the new one.

## Cross-Reference

BLE protocol (services, characteristics, payloads) is a shared contract with the firmware — changes here must be reflected there. See `ble-protocol.md` and `firmware.md`. Firmware update transport and the sender algorithm are in `ota.md`.
