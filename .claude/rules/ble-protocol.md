# Ori — BLE GATT Protocol Specification

**Protocol version:** 1.2
**Date:** 2026-06-10
**Status:** Authoritative — `esp32-connectivity` (peripheral) and `orion-sync` (central) must conform.

This document defines the single BLE GATT contract between Ori (peripheral) and the Orion PC app (central).

**Out of scope for BLE:** ANCS phone link (`connectivity.md`); firmware updates run over USB CDC (`ota.md`).

---

## 1. Roles

| Side | Role |
|---|---|
| **Ori (Arduino on ESP32-S3)** | GATT Peripheral + Advertiser |
| **Orion (Flutter, Windows/macOS)** | GATT Central |

LE Secure Connections with Passkey Entry (6-digit numeric, MITM-protected) is mandatory. After first pairing the device is bonded; subsequent reconnects are silent.

---

## 2. Advertising and bond policy

Ori accepts **at most two bonded peers**: one PC (Orion) and one iPhone (ANCS). New bonds are accepted only in the state-gated pairing windows below; outside them an unknown device that connects can neither bond (pairing is rejected) nor read/write any data characteristic (all are encrypted), so it gains nothing. When both slots are full and both peers are connected, Ori stops advertising; if a bonded peer drops, Ori advertises **public undirected** so it can reconnect on its own.

### Bond slot enforcement

1. **State-machine-gated pairing.** New bonds accepted only during:

   | Firmware state | Bond slot open |
   |---|---|
   | Setup Step 2 | Orion slot only |
   | Setup Step 4 / Runtime re-pair-iPhone | iPhone slot only |
   | All other states | Neither — rejected |

2. **Typed NVS slots.**

   ```
   NVS namespace "bonds":
     "orion_addr"  → 6-byte BLE address  (all-zero = slot empty)
     "iphone_addr" → 6-byte BLE address  (all-zero = slot empty)
   ```

   Step 2 bond → `orion_addr`; Step 4 / re-pair → `iphone_addr`. On reconnect, peer address determines role (Orion sync vs. ANCS).

3. **Directed advertising once full.** Link layer only accepts the two stored addresses.

Factory reset zeroes both NVS entries and clears both NimBLE bond records.

### Advertising state machine

| Bond state | Advertising mode | Manufacturer-data flag |
|---|---|---|
| 0 bonded (fresh / post-factory-reset) | Public undirected | `0x01 SETUP` |
| 1 bonded — PC only, iPhone slot empty | Public undirected | `0x02 RUNTIME` |
| 1 bonded — iPhone only | Public undirected | `0x01 SETUP` |
| 2 bonded — PC + iPhone, ≥1 disconnected | **Public undirected** (so *either* central can reconnect on its own — iPhone off the advert, Orion via its own scan). No accept-list (bonding is state-gated + all data chars encrypted; iPhone uses rotating RPAs which make an accept-list fragile). | `0x02 RUNTIME` |
| 2 bonded — PC + iPhone, both connected | **Advertising stopped** (nothing to advertise for). Re-armed on either disconnect. | (none) |
| Runtime re-pair-iPhone in progress | Public undirected until iPhone re-bonds | `0x02 RUNTIME` |

### Advertising mode transitions

| Event | Action required |
|---|---|
| iPhone bond formed | Both slots full → once both peers are connected, advertising stops; if either later disconnects, restart **public undirected** (`0x02 RUNTIME`) so it can reconnect |
| Bonded peer reconnects (both bonded) | BLE auto-stops adv on connect → `onConnect` restarts undirected adv so the *other* bonded peer can also reconnect; stops again once both are connected |
| Bonded peer disconnects (both bonded) | `onDisconnect` restarts **public undirected** (`0x02 RUNTIME`) so the dropped peer can reconnect |
| iPhone unpaired (`on_unpair_phone`) | Delete iPhone bond + zero `iphone_addr` → restart **public undirected** with flag `0x02 RUNTIME`. The **ANCS UUID is advertised only once the user opens the re-pair screen** (`set_iphone_pairing_window(true)`), not immediately on unpair — see Advertising payload below. |
| Orion bond formed (Step 2) | iPhone slot still empty → remain public undirected, flag `0x01 SETUP` → `0x02 RUNTIME` |
| Factory reset | Wipe both bonds + NVS addresses → restart public undirected, flag `0x01 SETUP` |

### Advertising payload

- **Device name:** `Ori-XX-XX` (per-device suffix, e.g. `Ori-XT-9F`)
- **Service UUIDs:** the public undirected advert has two mutually-exclusive flavours (two 128-bit UUIDs don't fit one 31-byte packet):
  - **Orion-discovery (default):** `Ori Sync Service` UUID + manufacturer mode flag in the **primary advertising packet**, so Orion can discover/classify Ori.
  - **iPhone-pairing (pairing screen on-screen + iPhone slot empty — Setup Step 4 or runtime re-pair, `set_iphone_pairing_window(true)`):** the **ANCS UUID** (`7905F431-B5CE-4E99-A40F-4B1E122D00D0`) goes in the **primary advertising packet** as a **Service Solicitation** (AD type `0x15`, "list of 128-bit Service Solicitation UUIDs") — meaning "I want a device that *provides* ANCS" — **not** as a provided-service list (`0x06`/`0x07`). Ori consumes ANCS; the solicitation is what makes iOS engage it. (Advertising ANCS as a *provided* service made the device visible in nRF — "Services: ANCS" — but iOS never listed/engaged it.) Plus a generic **Appearance** (`0x0180`). Built by hand via `addData()` (NimBLE has no solicitation setter). The Ori Sync UUID / mode flag are dropped during this window (Orion is connected through Step 4 / re-pair). In normal runtime (iPhone slot empty, not actively pairing) the ANCS solicitation is omitted, so Ori does not solicit iPhone connections.
  - Device name is in the scan response in both flavours. Ori no longer uses directed advertising — see §2 (both-bonded reconnect is public undirected).
- **Manufacturer data:** byte 0 = `0xFF FF` (placeholder company ID); byte 1 = `0x01 SETUP` / `0x02 RUNTIME`
- **Interval:** 100 ms for all public undirected advertising — the 0-bonded setup state, the Orion-bonded/iPhone-slot-empty runtime state, and the both-bonded reconnect state (≥1 peer disconnected). Fast advertising keeps both reconnection and iPhone (ANCS) discovery snappy. (Ori is wall-powered, so continuous fast advertising is fine; advertising stops entirely once both bonded peers are connected.)

Orion uses the mode flag to detect "Ori factory-reset since last bond" without connecting (§7).

---

## 3. Service and characteristics

**One service, fifteen characteristics.**

```
Ori Sync Service:  6F726900-0000-4F72-9F00-000000000000
```

Each characteristic UUID replaces bytes 4–5 of the base with the offset below.

| # | Name | UUID offset | Properties | Direction | Encrypted? |
|---|---|---|---|---|---|
| 1 | Protocol Version | `0001` | Read | C→P read | No |
| 2 | Device Status | `0002` | Read, Notify | P→C notify | No |
| 3 | Time Sync | `0003` | Write (response) | C→P | Yes |
| 4 | Profile Info | `0004` | Write (response) | C→P | Yes |
| 5 | Profile Photo | `0005` | Write (response) | C→P chunked | Yes |
| 6 | Meeting List | `0006` | Write (response) | C→P chunked | Yes |
| 7 | PTO Entry | `0007` | Write (response) | C→P chunked | Yes |
| 8 | Sync Control | `0008` | Write, Notify | bidirectional | Yes |
| 9 | Factory Reset Command | `0009` | Write (response) | C→P | Yes |
| 10 | Sync Manifest | `000A` | Write, Notify | bidirectional | Yes |
| 11 | **Keyboard Command** | `000B` | Notify | P→C notify | Yes |
| 12 | **Host Volume State** | `000C` | Read, Write (response) | C→P | Yes |
| 13 | **Media Metadata** | `000D` | Write, Notify | C→P | Yes |
| 14 | **Media Album Art** | `000E` | Write (no response) | C→P chunked | Yes |
| 15 | **Presence Status** | `000F` | Read, Write (response) | C→P | Yes |

Reads/writes on encrypted characteristics over an unencrypted link return `INSUFFICIENT_AUTHENTICATION`.

---

## 4. Payload encoding — CBOR

All structured payloads use CBOR (RFC 8949). Libraries: Arduino — `ArduinoCBOR`; Dart — `cbor` pub package. Unknown keys are silently ignored (forward-compatible).

### Schemas

```cbor
ProfileInfo = {
  "name":  text,       // UTF-8, ≤ 32 chars (≤ 96 bytes wire)
  "title": text,       // UTF-8, ≤ 32 chars (≤ 96 bytes wire)
  "email": text,       // optional; UTF-8, ≤ 32 chars; absent or "" = not shown
  "phone": text        // optional; UTF-8, ≤ 16 chars; absent or "" = not shown
}

TimeSync = {
  "epoch_utc": uint,   // seconds since 1970-01-01 UTC
  "tz":        text,   // IANA timezone, e.g. "Europe/Lisbon"
  "tx_ms":     uint    // sender's monotonic ms at send time (for round-trip correction)
}

Meeting = {
  "id":    text,       // calendar-provider-stable id
  "start": uint,       // epoch UTC
  "end":   uint,       // epoch UTC
  "title": text,       // no length cap (chunking handles size)
  "loc":   text,
  "org":   text
}

MeetingList = {
  "date":  uint,       // epoch UTC of local-midnight (day-rollover detection)
  "items": [Meeting, ...]
}

PtoEntry = {
  "start":       uint,
  "end":         uint,
  "destination": text, // e.g. "Lisbon, Portugal"
  "image":       bytes // JPEG (528×396 target); may be empty. Orion resizes to this before sending.
}

SyncControl = {
  "op":     "BEGIN" | "END" | "ACK" | "NACK",
  "seq":    uint,
  "reason": text,      // optional, populated for NACK
  "total":  uint       // optional, BEGIN only — total application-payload bytes the
                        // central will write across Time Sync, Profile Info, Profile
                        // Photo, Meeting List, PTO Entry for this sync session (only the
                        // items actually being sent — e.g. just the `needed` subset on a
                        // reconnect delta sync). Absent or 0 = progress is indeterminate.
}

SyncManifest_Write = {           // central → peripheral
  "profile_sha":  bytes(32),
  "photo_sha":    bytes(32),
  "meetings_sha": bytes(32),
  "pto_sha":      bytes(32)
}

SyncManifest_Notify = {          // peripheral → central
  "needed": [text, ...]          // subset of {"profile","photo","meetings","pto"}
}

ProtocolVersion = {
  "proto_major": uint,
  "proto_minor": uint,
  "fw_version":  text            // semver, e.g. "1.2.3"
}

KeyboardCommand = {              // peripheral → central, notify
  "op":  "vol_set" | "play_pause" | "prev" | "next" | "shortcut" | "seek",
  "arg": uint                    // vol_set → 0..100; shortcut → slot 1..3; seek → seconds
  // No dedicated `mute` op — mute is a user-configurable shortcut action in Orion settings.
}

HostVolumeState = {              // central → peripheral, write+read
  "level": uint,                 // 0..100
  "mute":  bool
}

MediaMetadata = {                // central → peripheral, write+notify
  "title":    text,
  "artist":   text,
  "can_seek": bool               // optional; absent = false. Ori hides scrubber when false.
}

// Media Album Art — raw JPEG bytes (not CBOR), via §5 chunking. Orion resizes to 484×216.

// Presence Status — single byte (NOT CBOR):
//   0x00  AVAILABLE   — Teams "Available"
//   0x01  BUSY        — Teams "Busy", "Do Not Disturb", "In a call", "In a meeting", "Presenting"
//   0x02  AWAY        — Teams "Be Right Back", "Appear Away"
//   0x03  OFFLINE     — Teams "Appear Offline" (or unknown / null)
// Any other value → NACK_CBOR_DECODE.
```

### Device Status (single-byte enum, no CBOR)

```
0x00 SETUP_WAITING_PAIRING       — Step 2, before bond
0x01 SETUP_BONDED_AWAITING_SYNC  — Step 2 done, before first sync
0x02 SETUP_SYNCING               — Step 3 (Orioning)
0x03 SETUP_SYNC_COMPLETE         — advances Ori UI to Step 4
0x10 RUNTIME_READY
0x11 RUNTIME_RECONNECTING        — bonded reconnect in progress
0x12 RUNTIME_SYNCING             — periodic refresh while RUNTIME_READY
0xF0 ERROR_GENERIC
```

(`0x20` reserved — do not reuse. OTA screen is driven locally via USB CDC; see `ota.md`.)

### Factory Reset Command (4-byte payload)

```
0xFA 0xC7 0x5E 0x5E    // "FAC75E5E"
```

Any other value → `NACK_BAD_MAGIC`.

---

## 5. Chunking protocol

Used by Profile Photo, Meeting List, PTO Entry, and Media Album Art.

### MTU strategy

- Request **247 bytes** on connect. Fall back to **23 bytes** if refused.
- Per-fragment payload at MTU 247: `247 - 3 (ATT header) - 6 (frame header) = 238 bytes`.

### Frame format

```
Offset  Size  Field
0       2     seq_num     (uint16, little-endian) — 0-based fragment index
2       2     total_frags (uint16, little-endian)
4       2     payload_len (uint16, little-endian)
6       N     payload
```

### Reassembly

- Accumulate in order. When `seq_num == total_frags - 1`, concatenate and decode.
- Gap → `SyncControl{op:"NACK", seq:<expected>, reason:"chunk_missing"}`; sender restarts from seq=0.
- Timeout (10 s no progress) → NACK `"chunk_timeout"`.

---

## 6. Connection sequences

### 6.0 Sync staging and progress

For **every** sync session (initial pairing or reconnect delta), Ori stages incoming
items in PSRAM as they arrive and commits them to NVS **atomically at `SyncControl{op:"END"}`**
— mirroring the USB CDC OTA pathway (`ota.md`), which stages the firmware image in
PSRAM before a single flash commit.

- Between `BEGIN` and `END`, nothing is written to NVS and no cached state (profile
  card, meeting list, PTO, hashes) changes. If the link drops mid-session, Ori discards
  the staged data and keeps serving the previously cached data — a partial transfer
  can never corrupt or partially overwrite NVS.
- At `END`, Ori parses each staged item, computes its SHA-256, persists it to NVS, and
  updates the live UI — in one batch.
- **Progress.** If `BEGIN` includes `total` (> 0), Ori tracks cumulative bytes received
  across Time Sync, Profile Info, Profile Photo, Meeting List, and PTO Entry writes —
  for chunked characteristics, each fragment's `payload_len` (not the 6-byte frame
  header) counts towards the total — and derives `pct = received * 100 / total`
  (capped at 99 until `END` finishes the commit, then 100). This drives the **Step
  2/3 "Orioning" progress ring** during first-time setup only (`setup-flow.md`). The
  reconnect "Reconnecting…" overlay (`state-machine.md`) does not show a percentage
  and ignores `total`.
- If `total` is absent or 0, Ori still stages-then-commits as above, but the orioning
  ring is not driven by byte progress (legacy/indeterminate).

### 6.1 First-time pairing

```
Ori boots fresh → public undirected adv, mode=0x01 SETUP
Orion scans → finds Ori-XT-9F → user taps "Pair"
Orion connects → ATT MTU exchange → BLE bonding (LE SC, Passkey Entry)
  • Ori displays the 6-digit code (Step 2 passkey modal)
  • User confirms on Orion → LTK stored on both sides
  • Ori holds the peer address PROVISIONALLY (RAM only) — the orion_addr NVS
    slot is NOT written yet (see handshake note below)

Ori notifies Device Status = SETUP_BONDED_AWAITING_SYNC
Orion computes total = byte-length of (Time Sync + Profile Info + Profile Photo
                                        + Meeting List + PTO Entry) payloads
Orion writes SyncControl{op:"BEGIN", seq:1, total:total}
  → HANDSHAKE: a valid SyncControl{BEGIN} on the encrypted link is Ori's proof
    that the bonded peer is the Orion app (not a phone or other PC someone paired
    off the passkey screen). On this BEGIN, Ori commits the provisional address
    to the orion_addr slot. A peer that bonds but never sends a valid BEGIN
    within ~5 s is disconnected and its LTK bond deleted — never saved as Orion.
Orion writes Time Sync
Orion writes Profile Info
Orion writes Profile Photo (chunked)
Orion writes Meeting List (chunked)
Orion writes PTO Entry (chunked)
Orion writes SyncControl{op:"END", seq:1}

Ori commits all staged items to NVS + computes per-item SHA-256 hashes (§6.0).
Ori notifies Device Status = SETUP_SYNC_COMPLETE
  → Ori UI advances to Step 4 (phone pairing, optional)
```

### 6.2 Bonded reconnect — hash-manifest delta sync

```
Ori boots OR loses connection → public undirected adv (§2)
Orion sees adv → reconnects → encrypted via stored LTK

Ori notifies Device Status = RUNTIME_RECONNECTING
  → Reconnecting overlay appears on left panel

Orion writes Time Sync (always — ~20 bytes, clock may drift)
Orion writes Sync Manifest: { profile_sha, photo_sha, meetings_sha, pto_sha }
Ori compares against NVS hashes → notifies Sync Manifest: { needed: [...] }

Orion computes total = byte-length of (Time Sync + only the `needed` items)
Orion writes SyncControl{op:"BEGIN", seq:N, total:total}
Orion writes only requested items (profile → photo → meetings → pto)
Orion writes SyncControl{op:"END", seq:N}

Ori commits all staged items to NVS + hashes atomically (§6.0).
Ori notifies Device Status = RUNTIME_READY → overlay dismissed.
```

**Hash content:** SHA-256 of canonical deterministic CBOR (sorted keys, smallest-encoding ints). If hashes drift for any reason, next reconnect re-pushes automatically.

### 6.3 Periodic refresh (while RUNTIME_READY)

| Trigger | Cadence | Effect |
|---|---|---|
| Time Sync | Every 60 min | Write Time Sync |
| Meeting List | Calendar event or every 15 min | Hash-check via Manifest, push if needed |
| PTO Entry | Calendar event | Hash-check, push if needed |
| Profile Info / Photo | User edit in Orion | Hash-check, push if needed |
| Presence Status | Teams change or ~60 s poll | Write Presence Status (only when value changes) |

Periodic refreshes set `RUNTIME_SYNCING` briefly but do **not** trigger the reconnecting overlay.

### 6.4 Presence push (no manifest, no hash)

Presence Status is ephemeral — not in the manifest flow, not persisted to NVS.

- On (re)connect: Ori **reads** Presence Status from Orion (source of truth).
- Between reads: Orion **writes** on Teams state change.
- Before first read/write: display `Offline`.
- On BLE link drop: immediately render `Offline` (stale presence would lie about reality).

---

## 7. Disconnect, reconnect, and cache semantics

- Synced data persists to NVS on `SyncControl{op:"END"}`. Ori displays cached data with "SYNCED · X min ago" pill while offline.
- Factory reset wipes NVS and both bonds; next connection is first-time pairing.

### 7.1 Factory-reset-during-reconnect

**Ori side:** drops both bonds, wipes NVS, reboots into setup with flag `0x01 SETUP`.

**Orion side:**
1. **Adv-flag check (preferred):** adv flag `0x01 SETUP` → Orion deletes its bond without connecting. UI: "Ori has been factory reset. Re-pair."
2. **Encryption-failure fallback:** LTK mismatch → Orion deletes bond and treats as path 1.

In both cases: **stop the reconnect loop** until the user re-pairs.

### 7.2 Remote factory reset (Orion → Ori)

1. Orion shows confirm dialog.
2. User confirms → Orion writes magic `0xFA C7 5E 5E` over encrypted link.
3. Ori wipes NVS + both bonds → reboots into first-boot setup.
4. Orion sees disconnect → deletes its bond → prompts re-pair.

Local long-press-photo reset is identical; both paths converge in the same firmware routine.

---

## 8. Errors

| Code | Meaning |
|---|---|
| `NACK_CHUNK_MISSING` | Reassembly gap; sender restarts from seq=0 |
| `NACK_CHUNK_TIMEOUT` | Reassembly stalled; sender restarts |
| `NACK_CBOR_DECODE` | Payload malformed; sender re-encodes, retries once |
| `NACK_TOO_LARGE` | Payload exceeds cap |
| `NACK_BAD_MAGIC` | Factory Reset Command wrong magic |

Link-layer encryption failure (`BLE_HS_ENC_FAIL`) signals a stale bond — see §7.1. Surface it cleanly; do not retry-loop.

---

## 9. Versioning

- Protocol Version characteristic: `{ proto_major, proto_minor, fw_version }`. This spec's wire version = **v1.1** (`PROTO_MAJOR=1, PROTO_MINOR=1` — `SyncControl.total` is the additive minor change).
- **Major bump** = breaking. Orion refuses sync, shows "Update Ori firmware".
- **Minor bump** = additive. Unknown CBOR keys silently ignored.
- `fw_version` (semver) used by Orion to detect available updates; update runs over USB CDC (`ota.md`).

---

## 10. Caps and limits

| Item | Limit |
|---|---|
| `ProfileInfo.name` | ≤ 32 chars (Orion input cap, `pc-app.md`); ≤ 96 UTF-8 bytes wire |
| `ProfileInfo.title` | ≤ 32 chars; ≤ 96 UTF-8 bytes wire |
| `ProfileInfo.email` | ≤ 32 chars; optional |
| `ProfileInfo.phone` | ≤ 16 chars; optional |
| Profile Photo (JPEG, 228×228) | hard cap 200 KB |
| Meeting `title` | no cap (chunking handles size) |
| Meeting list total | ≤ 32 meetings/day |
| `PtoEntry.destination` | ≤ 128 UTF-8 bytes |
| PTO image (JPEG, 528×396) | hard cap 512 KB — Orion resizes to 528×396 before sending |
| `MediaMetadata.title` | ≤ 192 UTF-8 bytes |
| `MediaMetadata.artist` | ≤ 96 UTF-8 bytes |
| Media Album Art (JPEG, 484×216) | target 15–30 KB; hard cap 64 KB |
| Presence Status | exactly 1 byte (0x00–0x03) |

---

## 11. Implementation owners

- **`esp32-connectivity`** — GATT server, bonding, chunk reassembly, NVS persistence + hashes, factory-reset routine, ANCS client, chars 11–15. No HOGP.
- **`orion-sync`** — scanning + connection lifecycle, bonding storage, hash-manifest delta, chunked writes, background keep-alive, USB CDC OTA path (`ota.md`), media-mode OS bridge (§12).

Any change to this file is a protocol change — bump the version header.

---

## 12. Media-mode bridging — the Orion-mediated model

Ori uses chars 11–14 instead of HOGP. Orion bridges each `KeyboardCommand` notify to OS APIs and mirrors OS state changes back to Ori.

### Command flow — Ori → Orion → OS

| User action | `KeyboardCommand` | Windows | macOS |
|---|---|---|---|
| Tap art | `{op:"play_pause"}` | `SendInput VK_MEDIA_PLAY_PAUSE` | `CGEventCreateMediaKeyEvent NX_KEYTYPE_PLAY` ¹ |
| Swipe right | `{op:"next"}` | `SendInput VK_MEDIA_NEXT_TRACK` | `NX_KEYTYPE_NEXT` |
| Swipe left | `{op:"prev"}` | `SendInput VK_MEDIA_PREV_TRACK` | `NX_KEYTYPE_PREVIOUS` |
| Vertical swipe release | `{op:"vol_set", arg:N}` | `IAudioEndpointVolume::SetMasterVolumeLevelScalar(N/100.0)` → write back `HostVolumeState` | `AudioObjectSetPropertyData kAudioHardwareServiceDeviceProperty_VirtualMainVolume` → write back |
| Tap shortcut slot N | `{op:"shortcut", arg:N}` | Orion runs configured action — see supported tokens below | same |

¹ Requires macOS Accessibility permission, granted on first launch.

Supported shortcut actions (configured per slot in Orion settings): `vol-mute`, `mic-mute`, `screenshot`, `lock-screen`, `favorite` (user-defined custom action). No dedicated `mute` op — mute is the `vol-mute` shortcut.

### State push flow — OS → Orion → Ori

- **Volume change:** `IAudioEndpointVolumeCallback` (Win) / `AudioObjectAddPropertyListener` (macOS) → debounce ~100 ms → write `HostVolumeState`
- **Track change:** `GlobalSystemMediaTransportControlsSessionManager` (Win) / `MRMediaRemoteRegisterForNowPlayingNotifications` (macOS) → write `MediaMetadata` + resize art to 484×216 JPEG (target 15–30 KB) → chunk-write `MediaAlbumArt`

### Swipe-vs-push race (vertical volume swipe)

While a vertical swipe is active (≥ 25 px threshold): Ori ignores incoming `HostVolumeState` writes. On lift: override drops, but Ori delays accepting the next push for ~800 ms (HUD linger). Orion debounces its OS volume push by ~100 ms.

### Cache + reconnect semantics

- `HostVolumeState` is read on (re)connect so the next swipe starts from the correct level.
- `MediaMetadata` and `MediaAlbumArt` are PSRAM-cached (not NVS); on reconnect Orion re-pushes the current track, or writes `MediaMetadata{title:"", artist:""}` if nothing is playing.
