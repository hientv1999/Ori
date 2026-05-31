---
paths:
  - "PC_app/**"
---

# Ori — Orion PC App

**Orion** is the PC companion app (`PC_app/`) that pairs with Ori over BLE. Windows + macOS. Install URL shown during setup: `ori.app/orion`. App icon: the Ori wordmark.

## Role

Orion is the authoritative data source for Ori. While running and synced, it pushes:

- Profile (name, job title, photo)
- Today's meeting list
- Next PTO entry (start, end, destination image)
- Current local time

When Orion is not running, Ori falls back to cached data. See `connectivity.md` for the sync state model and `setup-flow.md` for the pairing flow.

## What the PC App Must Implement

- **BLE central role**: discover, pair (6-digit passkey confirmation UI), and maintain a bonded connection to Ori. Persist bond records.
- **Calendar data**: read today's meetings from the user's calendar and push to Ori. Cancelled meetings removed; past meetings drop off the device automatically.
- **PTO**: push the user's next PTO entry with destination metadata.
- **Time sync**: push current local time on every sync.
- **Microsoft Teams presence**: read the signed-in user's presence via Microsoft Graph (`GET /me/presence`) and push as a 1-byte enum via the **Presence Status** characteristic (`ble-protocol.md` §3). Teams → Ori mapping:
  - `Available` → `0x00`
  - `Busy`, `DoNotDisturb`, `BusyIdle`, `InACall`, `InAMeeting`, `InAConferenceCall`, `Presenting`, `UrgentInterruptionsOnly` → `0x01`
  - `Away`, `BeRightBack`, `AwayIdle` → `0x02`
  - `Offline`, `PresenceUnknown`, `OffWork`, `OutOfOffice` → `0x03`
  - Push cadence: subscribe to Graph change notifications (`/subscriptions` on `/communications/presences/{id}`); fall back to polling every ~60 s. Only write to BLE when the mapped byte actually changes.
  - OAuth: user signs into Microsoft account during setup and grants `Presence.Read`. Cache the refresh token in OS keychain. On grant expiry/revocation: write `0x03 OFFLINE` to Ori, surface re-auth prompt in settings.
  - If Graph returns no presence data (non-corporate accounts, Teams not installed): write `0x03 OFFLINE` silently. The setup wizard makes the Teams-presence step skippable.
- **Profile management**: capture name, job title, photo; push on initial pairing and on change. **Enforce input limits** — name ≤ 24 characters, job title ≤ 40 characters (live counter; field blocks at limit). Wire-level byte caps are in `ble-protocol.md` §10.
- **Background operation**: stay running and synced without requiring a focused window.
- **Passkey confirmation UI**: during setup, display the 6-digit code from Ori and ask the user to confirm it matches.
- **Factory reset Ori**: settings button (confirm-dialog gated) that writes the Factory Reset Command over the bonded link, then drops Orion's local bond record. See `ble-protocol.md` §7.2.
- **Firmware update**: settings pane that polls `ori.app` for the latest version, compares to `fw_version` in Ori's BLE Protocol Version characteristic, and shows "Install update" when newer firmware exists. Transfer runs over **USB CDC** (`flutter_libserialport` or equivalent) using the framed protocol in `ota.md`. If no serial port is found, prompt "Plug Ori into this PC, then try again."
- **Controls-mode OS bridge**: subscribes to the `Keyboard Command` notify characteristic and translates each command to the appropriate OS action:
  - `play_pause` / `prev` / `next` → media-key injection (Windows: `SendInput VK_MEDIA_PLAY_PAUSE / _PREV_TRACK / _NEXT_TRACK`; macOS: `CGEventCreateMediaKeyEvent NX_KEYTYPE_PLAY / _PREVIOUS / _NEXT`)
  - `vol_set arg:N` → set OS master volume (Windows: `IAudioEndpointVolume::SetMasterVolumeLevelScalar(N/100.0)`; macOS: `AudioObjectSetPropertyData kAudioHardwareServiceDeviceProperty_VirtualMainVolume`)
  - `shortcut arg:N` → look up slot N in the local config table and run the configured action. Supported actions: **mute audio** (`vol-mute`), **mute mic** (`mic-mute`), **screenshot** (`screenshot`), **launch app** (`app-launch`), **lock screen** (`lock-screen`).
  - macOS media-key + key-combo injection requires **Accessibility permission** — prompt during first launch; Controls-mode features are inert until granted.
- **Controls-mode state push**: mirror OS state to Ori so the Controls UI stays accurate:
  - Volume changes: `IAudioEndpointVolumeCallback` (Win) / `AudioObjectAddPropertyListener` (macOS) → debounce ~100 ms → write `HostVolumeState { level, mute }`.
  - Track changes: `GlobalSystemMediaTransportControlsSessionManager` (Win) / `MRMediaRemoteRegisterForNowPlayingNotifications` (macOS) → write `MediaMetadata { title, artist }` + resize thumbnail to 180 × 180, JPEG-encode (~8–15 KB), chunk-write to `MediaAlbumArt`.
- **Media-mode shortcut configuration UI**: settings pane where the user assigns each of the three shortcut slots an icon (displayed on Ori) and a host-side action. The mapping is local to Orion. The chosen icon for each slot is written to Ori so the device knows what to render.
- **BLE backward-compatibility**: every Orion release must be able to GATT-connect to every prior `proto_major.proto_minor` that has shipped. Rationale: Orion → USB CDC is the only firmware-update path — if Orion can't connect to an older Ori, the user is stuck. If a `proto_major` bump is ever needed, the prior GATT layout must remain supported in Orion alongside the new one.

## Cross-Reference

BLE protocol (services, characteristics, payloads) is a shared contract with the firmware — changes here must be reflected there. See `ble-protocol.md` and `firmware.md`.
