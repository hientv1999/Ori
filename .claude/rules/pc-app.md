---
paths:
  - "PC_app/**"
---

# Ori — Orion PC App

The `PC_app/` folder contains **Orion**, the PC companion app that pairs with Ori over BLE.

## Role of Orion

Orion is the authoritative data source for Ori. While running and synced, it pushes the following to the device:

- Profile (name, job title, photo)
- Today's meeting list
- Next PTO entry (start, end, destination image)
- Current local time

When Orion is not running, Ori falls back to cached data. See `connectivity.md` for the full sync state model and `setup-flow.md` for the pairing flow.

## Target Platforms

Windows and macOS desktop. The installation URL shown on the device during setup is `ori.app/orion` (see `memory.md`).

## Branding

Orion uses the **Ori brand mark** (the accent-gold ring + inner solid circle defined in `memory.md` § Brand Assets) as its app icon and as a recurring visual anchor in the settings UI. The same mark appears on Ori itself as the album-art empty-state placeholder in Controls mode — one shared logo across both halves of the system.

## What the PC App Must Implement

Implementation decisions — UI framework, file structure, calendar provider integration — are open. Behavioral requirements:

- **BLE central role**: discover, pair (with 6-digit passkey confirmation), and maintain a connection to the Ori device. Pairing bonds are persisted.
- **Calendar data**: read today's meetings from the user's calendar (provider source open) and push them to Ori. Cancelled meetings must be removed; past meetings drop off the device automatically once their end time passes.
- **PTO**: surface the user's next PTO entry with destination metadata for the PTO scenic display.
- **Time sync**: push current local time to Ori on every sync — Ori has no RTC of its own to authoritatively trust.
- **Microsoft Teams presence**: read the signed-in user's Teams presence via the Microsoft Graph API (`GET /me/presence`) and push it to Ori as a 1-byte enum via the **Presence Status** characteristic (`ble-protocol.md` §3 char 16). Required mapping from Teams' rich presence enum to Ori's 4-state byte:
  - `Available` → `0x00 AVAILABLE` (green border)
  - `Busy`, `DoNotDisturb`, `BusyIdle`, `InACall`, `InAMeeting`, `InAConferenceCall`, `Presenting`, `UrgentInterruptionsOnly` → `0x01 BUSY` (red border)
  - `Away`, `BeRightBack`, `AwayIdle` → `0x02 AWAY` (yellow border)
  - `Offline`, `PresenceUnknown`, `OffWork`, `OutOfOffice` → `0x03 OFFLINE` (dark grey border)
  - Push cadence: subscribe to Graph **change notifications** (`/subscriptions` resource with `/communications/presences/{id}`) when available; fall back to polling every ~60 s when not. Only write to BLE when the mapped byte actually changes — no-op writes are dropped client-side to avoid BLE traffic and waste.
  - **Setup-time OAuth**: during initial Orion setup, the user signs into their Microsoft account once and grants the `Presence.Read` scope. Cache the refresh token in OS keychain. If the OAuth grant expires or is revoked, Orion writes `0x03 OFFLINE` to Ori and surfaces a re-auth prompt in its settings UI.
  - **Teams not installed / non-corporate accounts**: if Graph returns no presence data (some personal Microsoft accounts), Orion writes `0x03 OFFLINE` and the device renders dark grey indefinitely — no visible error on Ori. The setup wizard makes the Teams-presence step skippable for users who don't care.
- **Profile management**: capture name, job title, and profile photo; push to Ori during initial pairing and refresh on change. **Enforce display-friendly length limits in the input UI** so the device never receives a string that overflows the right panel:
  - **Name**: hard limit **24 characters** (live counter; input field blocks at 24).
  - **Job title**: hard limit **40 characters** (live counter; input field blocks at 40).
  These limits exist because the right panel's content area is 253 px wide and the device renders name + title each on a single line (Ori's firmware truncates with ellipsis as a defensive safety net, but the visual quality bar is "no truncation under normal use" — Orion's input limit is what guarantees that). The BLE protocol's stricter byte caps (`name: ≤ 64 UTF-8 bytes`, `title: ≤ 64 UTF-8 bytes`; see `ble-protocol.md` §10) are the wire-level safety margin, not the UX rule.
- **Background operation**: stay running and synced without requiring the user to keep a window focused.
- **Passkey confirmation UI**: during setup, display the 6-digit code from Ori and ask the user to confirm it matches.
- **Screen backlight ON/OFF toggle**: in settings, an explicit toggle button (not a slider) labeled "Screen backlight". Writes `0x00` (OFF) or `0x01` (ON) to the Backlight characteristic. Reflects the current state notified by Ori when the user toggles via the two-finger swipe gesture. Note: turning the backlight OFF does not put Ori to sleep — the device continues running and syncing. See `gestures.md` for the hardware constraint.
- **Factory reset Ori button**: in settings, a confirm-dialog-gated button that writes the Factory Reset Command (magic value) over the bonded link, then drops Orion's local bond record. See `ble-protocol.md` §7.2.
- **Firmware update path**: in settings, a "Check for Ori updates" pane that polls `ori.app` for the latest firmware version, compares it to the `fw_version` field of Ori's BLE Protocol Version characteristic, and exposes an "Install update" button when newer firmware exists. The actual transfer runs over **USB CDC, not BLE** — Orion opens the serial port to the USB-C-connected Ori (e.g. via `flutter_libserialport`) and streams the framed OTA protocol defined in `ota.md`. If no Ori serial port is enumerated, prompt the user to plug Ori into this PC. See `ota.md` for the full wire flow, framing, and failure modes.
- **Controls-mode OS bridge (runtime)**: Orion is the runtime bridge between Ori's Controls mode and the host OS — this is a core responsibility, not optional. Subscribes to the `Keyboard Command` notify characteristic (`ble-protocol.md` §3 char 12) and translates each command into the appropriate OS action:
  - `play_pause` / `prev` / `next` → inject the corresponding media key (Windows: `SendInput` with `VK_MEDIA_PLAY_PAUSE` / `_PREV_TRACK` / `_NEXT_TRACK`; macOS: `CGEventCreateMediaKeyEvent` for `NX_KEYTYPE_PLAY` / `_PREVIOUS` / `_NEXT`).
  - `vol_set` (with `arg`) → set the OS master volume directly (Windows: `IAudioEndpointVolume::SetMasterVolumeLevelScalar`; macOS: `AudioObjectSetPropertyData` on `kAudioHardwareServiceDeviceProperty_VirtualMainVolume`).
  - `shortcut` (with `arg` ∈ {1, 2, 3}) → look up the slot in the local configuration table and run the user's configured action (key combo, app launch, script, macro). Mute toggle is one of the actions a user can assign to a shortcut slot — there is no dedicated `mute` op in the `KeyboardCommand` enum.
  - On macOS, media-key + key-combo injection requires **Accessibility permission**, prompted on first launch (standard pattern — same as 1Password, Rectangle, Magnet, Raycast).
- **Controls-mode state push (runtime)**: Orion mirrors OS state to Ori so the Controls-mode UI stays honest:
  - Subscribe to OS master-volume changes (Windows: `IAudioEndpointVolumeCallback`; macOS: `AudioObjectAddPropertyListener`), debounce ~100 ms, write `HostVolumeState { level, mute }` to char 13.
  - Subscribe to OS now-playing changes (Windows: `GlobalSystemMediaTransportControlsSessionManager`; macOS: `MRMediaRemoteRegisterForNowPlayingNotifications` + `MPNowPlayingInfoCenter`). On every track change, read title + artist + thumbnail:
    - Write `MediaMetadata { title, artist }` to char 14 (usually fits in one ATT MTU).
    - Resize the thumbnail to 180 × 180, JPEG-encode (target ~8–15 KB), chunked-write the bytes to `MediaAlbumArt` char 15 via the §5 chunking protocol.
- **Keyboard-mode shortcut configuration UI (setup-time)**: a settings pane that lets the user assign each of the three shortcut slots to (a) an icon to display on the device and (b) the host-side action to trigger. The icon + action mapping is local to Orion (no need to push it to Ori — Ori only emits the slot number; Orion handles the action). The chosen icon for each slot is written to Ori via the Ori Sync Service so the device knows what to render in the shortcut row.
- **macOS Accessibility permission prompt during first-launch setup** — required for `CGEventPost`-based key injection. Surface a clear setup step explaining why; the Controls-mode features will be silently inert until the permission is granted.
- **BLE Protocol Version backward-compatibility.** Every Orion release MUST be able to GATT-talk to every Ori firmware version it could plausibly encounter in the field (every prior `proto_major.proto_minor` that has shipped to a customer). Rationale: with USB-MSC removed from the firmware-update path (`ota.md`), the only customer-facing update flow is Orion → USB CDC → Ori. If Orion can't connect to an older Ori over BLE, the user can't receive the firmware update that would close the gap. If a `proto_major` bump is ever proposed, prior GATT layouts must remain implemented in Orion alongside the new one.

## Cross-Reference

The BLE protocol (services, characteristics, payload formats) is a shared contract with the firmware. Any change to the protocol on this side must also be reflected in the firmware. See `firmware.md` for the device-side counterpart.
