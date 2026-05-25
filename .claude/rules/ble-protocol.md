# Ori — BLE GATT Protocol Specification

**Protocol version:** 1.6
**Date:** 2026-05-24
**Status:** Authoritative — implementations of `esp32-connectivity` (peripheral, firmware) and `orion-sync` (central, PC) must conform to this spec.

This document defines the single BLE GATT contract between the Ori device (peripheral) and the Orion PC companion app (central). Everything that crosses that link is specified here.

**Out of scope for BLE:**
- The phone↔Ori ANCS link — see `connectivity.md`.
- **Firmware updates** — Ori is USB-C wall-powered, so during normal operation the cable is physically connected to a host. Firmware updates run over **USB CDC**, not BLE. See `ota.md`.

### Changelog
- **v1.6 (2026-05-24):** Added `"email"` and `"phone"` to `ProfileInfo`. Both are optional CBOR fields (absent = empty string on Ori). Ori displays them in the profile detail overlay (tap the profile photo to open). Orion enforces ≤ 128 UTF-8 bytes for email and ≤ 32 UTF-8 bytes for phone at input time. No new characteristic — additive fields on `Profile Info` char (`0x0004`).
- **v1.5 (2026-05-24):** Added `"can_seek": bool` to `MediaMetadata`. Orion reads the seek capability of the active media session per-app (`IsPlaybackPositionEnabled` on Windows SMTC; presence of `kMRMediaRemoteNowPlayingInfoElapsedTime` in the now-playing dict on macOS) and includes it in every `MediaMetadata` write. When absent or `false`, Ori hides the timeline scrubber entirely — no dead affordance for apps that lock seek. When `true`, the scrubber renders and drag-to-seek is enabled. Default assumption for a missing key: `false` (safe — suppresses the scrubber rather than showing a broken one). No new characteristic or encoding change — additive CBOR field on `MediaMetadata` (`0x000E`).
- **v1.4 (2026-05-24):** Added `"seek"` op to `KeyboardCommand`. When the user drags the timeline scrubber on the Controls-mode screen, Ori emits `KeyboardCommand{op:"seek", arg:<position_s>}` (position in seconds, uint). Orion bridges this to the OS seek API: on Windows, `GlobalSystemMediaTransportControlsSession.TryChangePlaybackPositionAsync(position_s × 10_000_000)` (100-ns ticks); on macOS, `MRMediaRemoteSetElapsedTime(position_s)` (private but stable API). Works for apps that expose seek control via SMTC / MRMediaRemote (Spotify, YouTube Music, Windows Media Player, Apple Music, and most major players). No new characteristic — the existing `Keyboard Command` char (`0x000C`) carries the new op. No protocol surface-area change.
- **v1.3 (2026-05-21):** Added the **Teams presence** feature as one new characteristic on the existing Ori Sync Service: `0x0012 Presence Status` (Orion → Ori, Write + Read). Orion reads the user's Microsoft Teams presence via the Microsoft Graph API (`/me/presence`), maps the rich Teams enum to a 4-state byte (Available / Busy / Away / Offline), and pushes the value to Ori on every change. The device renders this as the **border colour of the profile photo** on the right panel: green / red / yellow / dark grey. When the BLE PC link is down Ori falls back to dark grey regardless of the last cached value (it can't claim a presence it can't currently verify). See `pc-app.md` for the Microsoft Graph integration and `screen-layout.md` for the rendering rule. Characteristic count: 15 → 16.
- **v1.2 (2026-05-20):** Added the **keyboard-mode** feature set as four new characteristics on the existing Ori Sync Service: `0x000C Keyboard Command`, `0x000D Host Volume State`, `0x000E Media Metadata`, `0x000F Media Album Art`. Orion bridges these to OS-level HID/volume/now-playing APIs (see `keyboard-mode.md` and `pc-app.md`). **No standard BLE HID Over GATT (HOGP) profile is used** — keyboard-mode interactions travel over the custom Ori Sync Service. Rationale: Orion is always running in the background and can translate any custom command into any OS-level action, which unlocks an honest volume slider, now-playing metadata + album art display, and arbitrary user-shortcut actions — none of which HOGP can carry. Characteristic count: 11 → 15.
- **v1.1 (2026-05-20):** Removed BLE OTA. Dropped characteristics `0x0010` (OTA Control) and `0x0011` (OTA Chunk), the `OtaControl` CBOR schema, Device Status `0x20 OTA_UPDATING`, error codes `NACK_OTA_IN_PROGRESS` and `NACK_OTA_HASH_MISMATCH`, and the entire §7b. Rationale: Ori is USB-C wall-powered (some units carry an optional LiPo as brief-blackout backup, but the firmware never assumes it — see `hardware.md`), so the cable is part of normal operation. Pushing firmware over BLE was solving a problem the product doesn't have. USB CDC is ~10–30× faster and removes a substantial chunk of M5 complexity. Characteristic count: 13 → 11.
- **v1.0 (2026-05-15):** Initial spec.

---

## 1. Roles

| Side | Role |
|---|---|
| **Ori (firmware, Arduino on ESP32-S3)** | GATT Peripheral + Advertiser |
| **Orion (Flutter app, Windows/macOS)** | GATT Central |

LE Secure Connections with Passkey Entry (6-digit numeric, **MITM-protected**) is mandatory. After first pairing, the device is bonded; subsequent reconnects are silent.

---

## 2. Advertising and bond policy

Ori accepts **at most two bonded peers**: one PC (Orion) and one phone (ANCS). Once both slots are filled, Ori stops public advertising entirely and uses **directed advertising** to the bonded peer addresses only. Unknown devices cannot scan or connect.

### Advertising state machine

| Bond state | Advertising mode | Manufacturer-data flag |
|---|---|---|
| 0 bonded (fresh / post-factory-reset) | Public undirected | `0x01 SETUP` |
| 1 bonded — PC only, phone slot empty | Public undirected | `0x02 RUNTIME` |
| 1 bonded — phone only (defined for completeness; not normally reached) | Public undirected | `0x01 SETUP` |
| 2 bonded — PC + phone | **Directed only**, alternating between the two bonded addresses | (no flag in directed adv) |
| Runtime re-pair-phone in progress | Public undirected until phone re-bonds | `0x02 RUNTIME` |

### Advertising payload

- **Device name:** `Ori-XX-XX` (suffix is per-device, e.g. `Ori-XT-9F`)
- **Service UUID:** Ori Sync Service (so Orion can scan-filter by service)
- **Manufacturer data (2 bytes):**
  - Byte 0: company ID prefix (use `0xFF FF` placeholder until a real Bluetooth SIG ID is assigned)
  - Byte 1: mode flag — `0x01 SETUP`, `0x02 RUNTIME`
- **Advertising interval:** 100 ms in setup mode, 1 s in runtime mode

Orion uses the mode flag to detect "Ori has been factory-reset since last bond" without needing to attempt a connection (§7).

---

## 3. Service and characteristics

**One service, sixteen characteristics.** Each data type has its own characteristic to keep the protocol legible and let each side share one chunking helper.

### Service UUID

```
Ori Sync Service:  6F726900-0000-4F72-9F00-000000000000
```

The first three bytes `6F 72 69` are "Ori" in ASCII. Each characteristic UUID derives from this base by replacing bytes 4-5 with a 16-bit offset (shown in the table below).

### Characteristic catalogue

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
| 9 | Backlight | `0009` | Read, Write, Notify | bidirectional | Yes |
| 10 | Factory Reset Command | `000A` | Write (response) | C→P | Yes |
| 11 | Sync Manifest | `000B` | Write, Notify | bidirectional | Yes |
| 12 | **Keyboard Command** | `000C` | Notify | P→C notify | Yes |
| 13 | **Host Volume State** | `000D` | Read, Write (response) | C→P | Yes |
| 14 | **Media Metadata** | `000E` | Write, Notify | C→P | Yes |
| 15 | **Media Album Art** | `000F` | Write (no response) | C→P chunked | Yes |
| 16 | **Presence Status** | `0012` | Read, Write (response) | C→P | Yes |

UUID offsets `0010` and `0011` were defined in v1.0 for OTA Control / OTA Chunk and are now **reserved — do not reuse**. Firmware updates run over USB CDC (see `ota.md`).

All characteristics except Protocol Version and Device Status require the link to be encrypted (i.e. the peer must be bonded). Reads/writes on encrypted-only characteristics over an unencrypted link return `INSUFFICIENT_AUTHENTICATION`.

---

## 4. Payload encoding — CBOR

All structured payloads use CBOR (RFC 8949). Reasons:

- Compact like binary, self-describing like JSON
- Naturally handles UTF-8 strings (multi-line meeting titles), arrays (meeting list), and byte strings (images)
- Mature libraries: Arduino — `ArduinoCBOR` or similar; Dart — the `cbor` pub package
- Unknown CBOR keys are ignored by older sides, making minor-version evolution free

### Schemas

```cbor
ProfileInfo = {
  "name":  text,       // UTF-8, max 64 bytes
  "title": text,       // UTF-8, max 64 bytes
  "email": text,       // optional; UTF-8, max 128 bytes; absent or "" = not shown
  "phone": text        // optional; UTF-8, max 32 bytes;  absent or "" = not shown
}

TimeSync = {
  "epoch_utc": uint,   // seconds since 1970-01-01 UTC
  "tz":        text,   // IANA timezone, e.g. "Europe/Lisbon"
  "tx_ms":     uint    // sender's monotonic ms at send time (for round-trip correction)
}

Meeting = {
  "id":    text,       // calendar-provider-stable id (Google/Outlook/macOS event id)
  "start": uint,       // epoch UTC
  "end":   uint,       // epoch UTC
  "title": text,       // never truncated; no length cap (chunking handles size)
  "loc":   text,
  "org":   text
}

MeetingList = {
  "date":  uint,       // epoch UTC of local-midnight, to detect day rollover
  "items": [Meeting, ...]
}

PtoEntry = {
  "start":       uint,
  "end":         uint,
  "destination": text, // e.g. "Lisbon, Portugal"
  "image":       bytes // JPEG (228x228 target); may be empty
}

SyncControl = {
  "op":     "BEGIN" | "END" | "ACK" | "NACK",
  "seq":    uint,      // batch sequence number
  "reason": text       // optional, populated for NACK
}

SyncManifest_Write = {           // central → peripheral
  "profile_sha":  bytes(32),
  "photo_sha":    bytes(32),
  "meetings_sha": bytes(32),
  "pto_sha":      bytes(32)
}

SyncManifest_Notify = {          // peripheral → central
  "needed": [text, ...]          // subset of {"profile","photo","meetings","pto"}; may be empty
}

ProtocolVersion = {
  "proto_major": uint,
  "proto_minor": uint,
  "fw_version":  text            // semver, e.g. "1.2.3"
}

KeyboardCommand = {              // peripheral → central, notify
  "op":  "vol_set" | "play_pause" | "prev" | "next" | "shortcut" | "seek",
  "arg": uint                    // optional per op:
                                 //   vol_set  → level 0..100
                                 //   shortcut → slot 1..3
                                 //   seek     → position in seconds (uint32)
  // Note: there is no dedicated `mute` op — mute is delivered as a
  // user-configurable shortcut action mapped in Orion's settings (the user
  // assigns one of the three shortcut slots to "toggle OS master mute").
}

HostVolumeState = {              // central → peripheral, write+read
  "level": uint,                 // 0..100
  "mute":  bool
}

MediaMetadata = {                // central → peripheral, write+notify
  "title":    text,              // UTF-8; usually fits in one ATT MTU
  "artist":   text,              // UTF-8
  "can_seek": bool               // optional; absent = false. True when the active
                                 // media session supports position-seek via the OS
                                 // API (SMTC IsPlaybackPositionEnabled on Windows;
                                 // presence of elapsed-time key on macOS). Ori hides
                                 // the timeline scrubber when false or absent.
}

// Media Album Art payload is raw JPEG bytes (not CBOR), transferred via
// the §5 chunking protocol. Orion resizes to 180×180 before sending.

// Presence Status payload is a single byte enum (NOT CBOR — small enough
// that the framing overhead would dwarf the data).
//
//   0x00  AVAILABLE   — Teams "Available"
//   0x01  BUSY        — Teams "Busy", "Do Not Disturb",
//                       "In a call", "In a meeting", "Presenting"
//   0x02  AWAY        — Teams "Be Right Back", "Appear Away"
//   0x03  OFFLINE     — Teams "Appear Offline" (or unknown / null)
//
// Any other value is rejected with `NACK_CBOR_DECODE` (the firmware reuses
// the same NACK code for this even though no CBOR is involved — saves an
// error code from the surface area).
```

### Device Status (single-byte enum, no CBOR)

```
0x00 SETUP_WAITING_PAIRING       — Step 2, before bond
0x01 SETUP_BONDED_AWAITING_SYNC  — Step 2 done, before first sync
0x02 SETUP_SYNCING               — Step 3 (Orioning)
0x03 SETUP_SYNC_COMPLETE         — advances Ori UI to Step 4
0x10 RUNTIME_READY
0x11 RUNTIME_RECONNECTING        — bonded reconnect in progress (full manifest exchange + delta push)
0x12 RUNTIME_SYNCING             — periodic refresh while already RUNTIME_READY
0xF0 ERROR_GENERIC
```

(`0x20 OTA_UPDATING` was defined in v1.0 and is now **reserved — do not reuse**. The "Updating firmware…" screen state is driven locally by the USB CDC OTA path; see `ota.md`.)

### Backlight (single-byte payload)

A single `uint8`:

```
0x00  OFF
0x01  ON
```

Any other value is rejected with `NACK_CBOR_DECODE`. The Waveshare board's backlight is binary on/off only — see `gestures.md` and `memory.md` for the hardware rationale.

Notes on semantics:
- Turning the backlight OFF does **not** put the device to sleep. The ESP32-S3 continues to run; BLE, calendar refresh, and timers stay active. Only the visible LED is gated.
- On boot, Ori restores the saved state from NVS before the panel powers on (no flash).
- Default after factory reset: ON.

### Factory Reset Command (4-byte payload)

```
0xFA 0xC7 0x5E 0x5E    // "FAC75E5E" — Factory Erase
```

Any other value returns `NACK_BAD_MAGIC` and is ignored.

---

## 5. Chunking protocol

Used by Profile Photo, Meeting List, and PTO Entry. Anything that may exceed one ATT MTU.

### MTU strategy

- Request **247 bytes** on connect (ATT_MTU exchange).
- Fall back to **23 bytes** (default) if the peer refuses.
- Per-fragment payload at MTU 247: `247 - 3 (ATT header) - 6 (frame header) = 238 bytes`.

### Frame format

```
Offset  Size  Field
0       2     seq_num     (uint16, little-endian) — 0-based fragment index
2       2     total_frags (uint16, little-endian) — total fragments in this transfer
4       2     payload_len (uint16, little-endian) — bytes in this fragment
6       N     payload     (CBOR bytes for this slice)
```

### Reassembly

- Receiver accumulates fragments in order.
- When `seq_num == total_frags - 1` is received, concatenate all payload bytes and CBOR-decode.
- If a gap is detected (`seq_num != expected_next`), receiver writes `SyncControl{op:"NACK", seq:<expected>, reason:"chunk_missing"}`. Sender restarts the transfer from `seq=0`.
- Receivers SHOULD enforce a per-transfer timeout (e.g. 10 s of no progress) and NACK with reason `"chunk_timeout"`.

---

## 6. Connection sequences

### 6.1 First-time pairing (Setup flow)

```
Ori boots in fresh state → public undirected adv, mode=0x01 SETUP
Orion scans → finds Ori-XT-9F → user taps "Pair"
Orion connects → ATT MTU exchange → BLE bonding (LE SC, Passkey Entry)
  • Ori displays the 6-digit code in its passkey modal (Step 2)
  • Orion displays the same code in its PC dialog
  • User confirms "matches" on Orion → bond stored on both sides

Ori writes Device Status = SETUP_BONDED_AWAITING_SYNC (notify)
Orion writes SyncControl{op:"BEGIN", seq:1}
Orion writes Time Sync
Orion writes Profile Info
Orion writes Profile Photo (chunked)
Orion writes Meeting List (chunked)
Orion writes PTO Entry (chunked)
Orion writes SyncControl{op:"END", seq:1}

Ori persists everything to NVS, computes and stores per-item SHA-256 hashes.
Ori writes Device Status = SETUP_SYNC_COMPLETE (notify)
  → Ori UI advances Step 3 → Step 4 (phone pairing, optional)
```

### 6.2 Bonded reconnect — hash-manifest delta sync

Orion does not track changes via dirty flags. It computes a SHA-256 of each data item from what it currently has, sends a manifest, and Ori (source of truth for its own cache) replies with which items it actually needs.

```
Ori boots OR loses connection → directed adv to bonded peer(s)
Orion sees adv → reconnects → encrypted via stored LTK (silent)

Ori writes Device Status = RUNTIME_RECONNECTING (notify)
  → Ori UI overlays the left panel with a "Reconnecting…" progress ring
    (same component as Step 3 Orioning). Status bar + profile card stay visible.

Orion writes Time Sync (always — cheap, ~20 bytes, clock may have drifted)
Orion writes Sync Manifest:
    { profile_sha, photo_sha, meetings_sha, pto_sha }

Ori compares each hash against its NVS-stored hash for that item.
Ori notifies Sync Manifest reply:
    { needed: [...] }            // subset; may be empty

Orion writes SyncControl{op:"BEGIN", seq:N}
Orion writes only the requested items, in their fixed order
    (profile → photo → meetings → pto)
Orion writes SyncControl{op:"END", seq:N}

Ori updates the matching NVS items and their stored hashes atomically.
Ori writes Device Status = RUNTIME_READY (notify)
  → Ori UI dismisses the reconnecting overlay; normal state-machine routing resumes.
```

**UX consequence:** when nothing changed, the manifest exchange completes in ~300 ms and the overlay auto-dismisses. When data did change, the overlay persists only for the duration of the relevant transfers.

**Hash content:** each hash is the SHA-256 of the canonical CBOR-encoded bytes that would be written to the corresponding characteristic. CBOR encoding must be deterministic (sorted map keys, smallest-encoding integers).

**Self-healing:** if Ori's NVS or its stored hash drifts for any reason, the next reconnect detects a mismatch and re-pushes automatically. No manual recovery needed.

### 6.3 Periodic refresh (while RUNTIME_READY)

| Trigger | Cadence | Effect |
|---|---|---|
| Time Sync | Every 60 minutes | Write Time Sync only |
| Meeting List | On calendar provider event, or every 15 minutes safety net | Hash-check via Sync Manifest, push if needed |
| PTO Entry | On calendar provider event | Hash-check, push if needed |
| Profile Info / Photo | On user edit in Orion | Hash-check, push if needed |
| Backlight (PC toggle) | On user tap in Orion | Write Backlight |
| Backlight (Ori gesture) | On two-finger swipe up/down | Ori notifies Backlight |
| Presence Status | On Teams presence change (or every ~60 s as a polling fallback if Microsoft Graph webhooks aren't available) | Write Presence Status only when the byte actually changes — no-op writes are dropped client-side to avoid BLE traffic |

Periodic refreshes set `Device Status = RUNTIME_SYNCING` briefly but do **not** trigger the full reconnecting overlay — that is reserved for fresh reconnects.

### 6.4 Presence push (no manifest, no hash)

Presence Status is **ephemeral runtime state**, not synced cache. It does not participate in the hash-manifest flow (§6.2) and is not persisted to NVS — it lives in RAM on Ori for the lifetime of the connection.

- On (re)connect: Ori **reads** the `Presence Status` characteristic (a single byte) to recover the current value from Orion, who is the source of truth.
- Between reads, Orion **writes** the characteristic whenever the Teams state changes.
- Until the first read or write succeeds after (re)connect, Ori displays `Offline` (dark grey border).
- When the BLE link drops: Ori discards the cached value and renders `Offline` immediately. The user must know the presence is no longer being verified — keeping a stale green/red/yellow indicator would lie about reality.

---

## 7. Disconnect, reconnect, and cache semantics

- All synced data is persisted to **NVS** on Ori at the moment a complete transfer finishes (`SyncControl{op:"END"}` received).
- On disconnect, Ori continues to display cached data with a "SYNCED · X min ago" pill per `meeting-list.md`.
- On reconnect, Orion runs the hash-manifest flow (§6.2). Ori shows the Reconnecting overlay until END.
- Factory reset wipes NVS and both bonds. The next connection is treated as first-time pairing.

### 7.1 Factory-reset-during-reconnect

When Ori is factory-reset (locally or remotely) but Orion still has its bond cached:

**Ori side:** drops both bonds, wipes NVS, reboots into setup with mode flag `0x01 SETUP`.

**Orion side detects two ways:**

1. **Adv-mode flag check (preferred):** if a scan sees Ori with mode `0x01 SETUP`, Orion immediately deletes its own bond record for that Ori (LTK + IRK + device record) without attempting an encrypted connect. UI: "Ori has been factory reset. Open Orion's setup wizard to pair again."
2. **Encryption-failure fallback:** if Orion connects before noticing the flag, link-layer encryption fails because Ori no longer holds the LTK. Orion catches this error specifically, deletes its bond, and falls back to path 1.

In both cases the background sync service **stops the reconnect loop** for that Ori until the user re-pairs. No silent retry storms.

### 7.2 Remote factory reset (Orion → Ori)

```
Orion UI: confirm dialog
   "This will erase your profile, meetings, PTO, and bond from Ori. Continue?"
User confirms → Orion writes Factory Reset Command (magic 0xFA C7 5E 5E)
   over the encrypted+bonded link.
Ori validates the magic value, ACKs the write, then:
   1. Wipes NVS (profile, meetings, PTO, backlight state, hashes)
   2. Wipes both bonds (PC + phone)
   3. Reboots into first-boot setup
Orion sees the disconnection, deletes its own bond record for this Ori,
   shows "Ori reset complete. Run pairing to reconnect."
```

The local long-press-photo path on Ori produces identical NVS+bond-wipe behavior; both paths converge in the same firmware routine.

---

## 8. Errors

The protocol signals only these errors:

| Code | Meaning |
|---|---|
| `NACK_CHUNK_MISSING` | Reassembly gap detected; sender restarts transfer from seq=0 |
| `NACK_CHUNK_TIMEOUT` | Reassembly stalled; sender restarts |
| `NACK_CBOR_DECODE` | Payload malformed; sender re-encodes and retries once |
| `NACK_TOO_LARGE` | Payload exceeds cap (e.g. photo > 40 KB) |
| `NACK_BAD_MAGIC` | Factory Reset Command had wrong magic value |

Link-layer encryption failure (`BLE_HS_ENC_FAIL` or the platform-specific equivalent in Dart) is the **specific signal** that the bond is stale — see §7.1. Both sides must surface it cleanly rather than retry-looping.

No other negotiation. Keep the surface area small.

---

## 9. Versioning

- The Protocol Version characteristic returns `{ proto_major, proto_minor, fw_version }`.
- This spec is **Protocol version 1.1**.
- **Major bump** = breaking change. Orion refuses to sync and shows "Update Ori firmware".
- **Minor bump** = additive only. Older parsers ignore unknown CBOR keys, so it's free.
- `fw_version` is informational (semver of the firmware build), used by Orion to detect available firmware updates and surface the "Install update" CTA. The update itself runs over USB CDC (see `ota.md`), not BLE.

---

## 10. Caps and limits

| Item | Limit |
|---|---|
| `ProfileInfo.name` | ≤ 64 UTF-8 bytes at the wire layer. **Orion enforces a stricter display-friendly limit of 24 characters at input time** (`pc-app.md`) so the device never has to truncate under normal use. |
| `ProfileInfo.title` | ≤ 64 UTF-8 bytes at the wire layer. **Orion enforces a stricter display-friendly limit of 40 characters at input time** (`pc-app.md`). |
| `ProfileInfo.email` | ≤ 128 UTF-8 bytes. Optional — absent or empty string means not shown in the profile overlay. |
| `ProfileInfo.phone` | ≤ 32 UTF-8 bytes. Optional — absent or empty string means not shown in the profile overlay. |
| Profile Photo (JPEG, 228×228) | target ≤ 25 KB; hard cap 40 KB |
| Meeting `title` | no length cap; titles are never truncated |
| Meeting list total | ≤ 32 meetings/day (safety net; well above typical) |
| `PtoEntry.destination` text | ≤ 128 UTF-8 bytes |
| PTO image (JPEG) | hard cap 64 KB |
| `MediaMetadata.title` | ≤ 192 UTF-8 bytes (truncated/ellipsised on Ori display) |
| `MediaMetadata.artist` | ≤ 96 UTF-8 bytes |
| Media Album Art (JPEG, 180×180) | target ~8–15 KB; hard cap 40 KB |
| Presence Status | exactly 1 byte (enum: 0x00 / 0x01 / 0x02 / 0x03 — anything else NACKed) |

---

## 11. Implementation pointers

This spec is consumed by two implementation agents:

- **`esp32-connectivity`** — peripheral side (Ori firmware). Owns the GATT server, bonding flow, chunk reassembly, NVS persistence of synced data + hashes, factory-reset routine, ANCS client (separate concern, see `connectivity.md`), and the four new Controls-mode characteristics (§12). Firmware updates are not handled here — they run over USB CDC; see `ota.md`. No standard HOGP profile is implemented.
- **`orion-sync`** — central side (Orion app). Owns scanning + connection lifecycle, bonding storage, the hash-manifest delta protocol, chunked writes, backlight toggle sync, the background service that keeps the link alive across PC sleep/wake, the USB CDC firmware-update path (see `ota.md`), and **the keyboard-mode OS bridge** (§12): subscribes to `Keyboard Command` notifies, translates each into the appropriate OS-level action (volume API, media-key injection, configured shortcut macro), reads the OS's now-playing info + album art on every track change and pushes them to Ori via `Media Metadata` and `Media Album Art`, monitors OS master-volume changes and pushes them to Ori via `Host Volume State`.

Any change to this file is a protocol change. Bump the version header + changelog at the top and flag it explicitly when proposing the change.

---

## 12. Keyboard-mode bridging — the Orion-mediated model

Ori does **not** implement BLE HID Over GATT (HOGP). Instead, all keyboard-mode interactions travel as custom commands on the Ori Sync Service (§3, characteristics 12–15) and Orion (the always-running PC companion) acts as the bridge to the host OS. This unlocks a richer feature set than HOGP can carry (truthful volume slider, now-playing metadata + album art, arbitrary user-configurable shortcut actions) at the cost of requiring Orion to be running — a dependency the product already has for calendar / profile / PTO sync.

### Command flow — Ori → Orion → OS

When the user interacts with Controls mode on Ori:

```
User taps the album art (a tap, not a swipe)
  → Ori notifies KeyboardCommand { op: "play_pause" }
  → Orion receives the notify
  → Orion calls the OS media-key injection API:
      • Windows: SendInput with VK_MEDIA_PLAY_PAUSE
      • macOS:   CGEventCreateMediaKeyEvent with NX_KEYTYPE_PLAY
                 (requires Accessibility permission, granted on first launch)
  → OS routes the media key to the focused/foreground audio app

User swipes left/right on the album art
  → Ori notifies KeyboardCommand { op: "prev" } or { op: "next" }
  → Orion injects VK_MEDIA_PREV_TRACK / VK_MEDIA_NEXT_TRACK (Win)
    or NX_KEYTYPE_PREVIOUS / NX_KEYTYPE_NEXT (macOS)

User swipes vertically on the album art (e.g. ~130 px upward → +65% to volume)
  → During the swipe, Ori shows a momentary HUD overlay tracking the level
  → On release, Ori notifies KeyboardCommand { op: "vol_set", arg: 95 }
  → Orion sets the OS master volume directly (not via media keys):
      • Windows: IAudioEndpointVolume::SetMasterVolumeLevelScalar(0.95)
      • macOS:   AudioObjectSetPropertyData on
                 kAudioHardwareServiceDeviceProperty_VirtualMainVolume
  → Orion immediately writes back HostVolumeState { level: 95, mute: false }
    confirming the achieved level; Ori HUD fades out shortly after

User taps a shortcut slot (1, 2, or 3) on Ori
  → Ori notifies KeyboardCommand { op: "shortcut", arg: 1|2|3 }
  → Orion looks up the slot in its local config table and runs the user's
    configured action — could be a key combo (SendInput / CGEventPost), an
    app launch, a script, a macro, etc.
  → Mute is one such configurable action: a user may bind a shortcut to
    "toggle OS master mute", and Orion will read+toggle HostVolumeState.mute
    when that shortcut fires. There is no dedicated `mute` op in the
    KeyboardCommand enum — see notes on `op` below.
```

### State push flow — OS → Orion → Ori

When something on the host changes the audio state or the now-playing media, Orion mirrors it to Ori so the UI stays honest:

```
OS master volume changes (any source: laptop key, headphone button, another app)
  → OS notifies Orion via subscription callback
      • Windows: IAudioEndpointVolumeCallback
      • macOS:   AudioObjectAddPropertyListener
  → Orion debounces (~100 ms) and writes HostVolumeState to Ori
  → Ori updates the cached volume value; next vertical swipe on the
    album art will start from this level

OS now-playing track changes (new song starts, app changes track, etc.)
  → OS notifies Orion via the now-playing subscription
      • Windows: GlobalSystemMediaTransportControlsSessionManager
      • macOS:   MRMediaRemoteRegisterForNowPlayingNotifications
  → Orion reads:
      - title + artist               → writes MediaMetadata
      - album art (raw thumbnail)    → resizes to 180×180, JPEG-encodes
                                       (target 8–15 KB), writes MediaAlbumArt
                                       in chunks via the §5 chunking protocol
  → Ori updates the now-playing display and the album-art widget
```

### Swipe-vs-push race condition (vertical volume swipe)

While the user is actively performing a vertical swipe on the album art, both directions of the loop are running simultaneously and would fight each other (Ori shows live HUD level → OS volume changes asynchronously from other sources → Orion pushes back → Ori HUD jumps). Resolved with:

- **Swipe-wins local override:** while a vertical swipe is in progress (touch down, vertical engagement crossed the 25 px threshold), Ori ignores incoming `HostVolumeState` writes. Touch lifts → override drops → next push from Orion becomes truth again, but Ori delays accepting it until the HUD has faded (~800 ms) so the user can see the final value they set.
- **Orion debounce on push-back:** Orion waits ~100 ms after the last OS volume change before writing, so it doesn't flood the BLE link during a sustained swipe.

### Cache + reconnect semantics

- `HostVolumeState` is read on (re)connect so the volume HUD shows the correct value the next time the user swipes.
- `MediaMetadata` and `MediaAlbumArt` are **not persisted across reboots** — both are PSRAM-cached on Ori and cleared on power cycle. On reconnect, Orion re-pushes whatever is currently playing.
- If nothing is playing when Orion reconnects, it writes `MediaMetadata { title: "", artist: "" }` and skips `MediaAlbumArt`. The Ori UI shows the "Nothing playing" empty state.

### What this is NOT

- **No HOGP.** No HID Service, no Battery Service stub, no Device Information Service. Ori advertises only its custom Ori Sync Service.
- **No fallback when Orion is closed.** The status-bar mode toggle hides entirely when the BLE-PC link drops (and Ori auto-reverts to calendar mode if it was in Controls), so the user simply cannot enter Controls mode without Orion. If Orion drops *during* an active session in Controls mode (rare race condition), the album-art gestures and shortcut buttons silently no-op for the moment between drop-detection and the auto-revert. This matches the existing "Orion must be running for calendar sync" assumption.
- **Not carried over USB.** USB-C is power + firmware update only.

### Scope and timeline

The four new characteristics' GATT plumbing (advertising, registration, notify+write handlers, chunk reassembly for album art) is in scope for **M5**. The keyboard-mode UI (216×216 album-art image with tap/horizontal-swipe/vertical-swipe gesture handling and volume HUD, title/artist metadata, three shortcut buttons — see `keyboard-mode.md`) and the Orion-side bridging implementation (`SendInput` / `CGEventPost` wrappers, OS volume API integration, now-playing subscription, shortcut configuration UI) are in scope for **M8**.
