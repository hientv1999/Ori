# Ori — Firmware Update

Firmware updates run over the **USB-C cable that already powers the device**. Ori is USB-C wall-powered (some units carry an optional LiPo as brief-blackout backup, but the firmware never assumes it — see `hardware.md`), so the USB-C link is physically present during normal operation. BLE OTA was considered for v1.0 of the BLE protocol and explicitly rejected in v1.1 — see `ble-protocol.md` changelog. Pushing ~1.5 MB over BLE took 3–6 minutes for what USB CDC does in ~10–30 s, while adding substantial firmware and Orion-app complexity.

## One path: USB CDC, Orion-driven

The single customer-facing update path is **USB CDC initiated by Orion**. USB-MSC (drag-and-drop) was considered and explicitly rejected — MSC requires Ori to enumerate as a removable drive, which is incompatible with the calm-desk-appliance UX (every USB plug-in would trigger AutoPlay / Finder mount popups; the device must never appear in File Explorer or Finder). Bricked-unit recovery, when needed, runs through the **internal UART port** (see "Bricked-unit recovery" below), which is a service / RMA path only — not a customer path.

| Property | Value |
|---|---|
| Initiator | Orion (Settings → "Install update") |
| Transport | USB CDC over the existing USB-C power cable |
| Time for ~1.5 MB image | ~10–30 s |
| Driver requirement | None — native USB CDC class is built into Win 10+, macOS, Linux |
| Visibility in file managers | None — CDC enumerates as a COM/tty port, never as a drive |

## Firmware implementation

- Use Arduino's built-in **`Update`** library: `Update.begin()`, `Update.write(buf, len)`, `Update.end(true)`.
- Use **`Update.canRollBack()`** and **`Update.rollBack()`** for the verification path after first boot of new firmware.
- The PlatformIO partition table reserves two 3 MB app slots (`ota_0`, `ota_1`) plus an OTA-data partition — see `firmware/partitions.csv`.
- Pending-verify window: the new firmware has **60 seconds** after first boot to mark itself valid. If it fails to come up cleanly (panic, watchdog, fails to enumerate USB, fails to advertise BLE), the second-stage bootloader rolls back to the previous slot automatically on the next boot.

## Transport

- ESP32-S3 native USB peripheral. No USB-to-serial chip in the path.
- Firmware already enables this: `-D ARDUINO_USB_CDC_ON_BOOT=1` in `firmware/platformio.ini`.
- Orion uses a Dart serial-port package (`flutter_libserialport` or equivalent) on Windows + macOS. The native USB CDC class is driverless on Win 10+, macOS, and Linux — no installer step.
- The same CDC port also carries the firmware boot log (`Serial.printf`). A framed protocol distinguishes log bytes from OTA bytes — see "Framing" below. (Composite USB with a second CDC interface is an acceptable v2 if framing proves awkward.)

## Wire flow

```
Orion checks ori.app for latest firmware version vs the fw_version
   exposed by Ori's BLE Protocol Version characteristic.
If newer exists:
   Orion UI: Settings → "Update available · 1.2.3" → tap "Install update"

Orion opens the serial port to the USB-C-connected Ori.
   If no port is found: prompt "Plug Ori into this PC, then try again."

Orion writes OTA frame: BEGIN { fw_version, total_size, sha256 }

Ori validates: version != current; size <= inactive slot capacity;
   no 5-minute pre-meeting countdown is active.
   On reject: respond REJECT { reason }; return to normal runtime.
   On accept: Update.begin(inactive_slot, total_size);
              switch screen to OTA-Updating;
              respond READY.

Orion streams DATA frames back-to-back. Firmware feeds each frame
   straight into Update.write().

Every ~5% of total_size, Ori responds PROGRESS { bytes_received }.

After last DATA, Orion writes END.

Ori computes SHA-256 over the received image.
   On match:    Update.end(true);       respond VALIDATED; sleep 200 ms; ESP.restart();
   On mismatch: Update.abort();         respond FAILED { "hash_mismatch" };
                resume normal runtime; firmware unchanged.

After reboot:
   Bootloader runs new firmware in pending-verify state.
   Firmware enumerates USB, brings up BLE, advertises to bonded peers.
   If first 60 s healthy → mark partition valid via Update.canRollBack/commit.
   If first 60 s unhealthy (panic, watchdog) → bootloader rolls back on next boot.

Orion reconnects to Ori over BLE, reads Protocol Version, confirms the
   new fw_version is live. Settings UI: "Up to date · 1.2.3".
```

## Framing (USB CDC OTA protocol)

A minimal framed binary protocol on top of the shared CDC stream. Each frame:

```
Offset  Size  Field
0       2     magic       = 0x4F54 ("OT" — distinguishes OTA frames from log bytes)
2       1     op          (see below)
3       3     payload_len (uint24, little-endian)
6       N     payload     (op-specific; CBOR for control ops, raw bytes for DATA)
```

Log bytes from `Serial.printf` are plain text and never start with the magic prefix. The OTA receiver on Orion's side reads bytes, splits on the magic, and treats anything outside framed messages as boot-log output to surface in a debug pane.

```
op  Name        Direction  Payload
01  BEGIN       PC → Ori   CBOR: { fw_version: text, total_size: uint, sha256: bytes(32) }
02  READY       Ori → PC   CBOR: {}
03  REJECT      Ori → PC   CBOR: { reason: text }
04  DATA        PC → Ori   raw bytes of the next image slice
05  PROGRESS    Ori → PC   CBOR: { bytes_received: uint }
06  END         PC → Ori   CBOR: {}
07  VALIDATED   Ori → PC   CBOR: {}
08  FAILED      Ori → PC   CBOR: { reason: text }
```

DATA payload size is bounded only by USB CDC's MTU (typically 64 B per packet on Full-Speed USB, batched by the host driver). Orion may send larger frames; the firmware reassembles by stream order and counts bytes against `total_size` — no per-frame sequence numbers needed because CDC delivers in order.

## On-device UX during update

- A full-screen "Updating firmware… N%" page replaces all other content. The existing `screen_ota_updating.cpp` is transport-agnostic — it reads a percentage from the active update state.
- **Status bar hidden**, **profile card hidden** — nothing competes for attention.
- All touch input is **inert**: two-finger backlight gesture, the mode-toggle tap, and 3-second long-press triggers (factory reset, re-pair phone) are all disabled.
- The page is **non-dismissable** while a transfer is in flight. The only escape is unplugging USB-C — which safely discards the partial image (the inactive slot is what was being written; the active slot still holds the previous firmware).
- Progress percentage updates from the cumulative `bytes_received` against the announced `total_size`.

## What is suspended during update

While an update is in progress, the firmware:

- Hides the status bar and profile card; takes over the screen with the update progress page
- NACKs BLE writes to all data characteristics (Profile Info, Photo, Meeting List, PTO Entry, Backlight, Factory Reset Command) with a generic refusal — Orion retries after the device reboots
- Suspends the 5-minute pre-meeting countdown timer (the update is only allowed to start if no countdown is currently active)
- Suspends Controls-mode command notifies (the `Keyboard Command` characteristic stops emitting while the update is in flight)
- Treats touch as inert (no gestures, no long-presses, no mode-toggle taps)

## When an update may not start

The firmware rejects an OTA `BEGIN` with `REJECT` and a reason string when any of these are true:

- A 5-minute pre-meeting countdown modal is currently displayed (`reason: "countdown_active"`)
- The announced `total_size` exceeds the inactive partition slot's capacity (`reason: "too_large"`)
- The announced `fw_version` matches the currently running firmware (`reason: "already_current"`)

USB CDC OTA does **not** require a BLE bond. The USB link is its own physical-access channel — a user with the cable plugged in already has full update authority.

## Bricked-unit recovery

If a firmware bug or partition corruption ever leaves Ori in a state where USB CDC won't enumerate (or won't respond to OTA `BEGIN`), the recovery path is the **internal UART port** on the PCB, accessible by opening the enclosure. Standard `esptool.py write_flash` over UART will reflash the device from a known-good image.

This is a **service / RMA path only** — the UART port is intentionally not exposed through the enclosure to keep the customer-facing surface to a single port. End users do not have access to UART under normal operation. See `hardware.md` for the enclosure design rule.

## Resumability — explicit non-goal

If USB-C is unplugged mid-transfer, the partial image is discarded. Orion must restart from `BEGIN` on the next attempt. Justification: USB CDC OTA completes in well under a minute and the cable is already part of the device's permanent power setup, so mid-transfer disconnects are rare to begin with. Adding resumability would require persistent state for the offset, partial-image hashing, and recovery logic on both sides — significant complexity for marginal benefit.

## Failure modes

| Failure | Behavior |
|---|---|
| USB unplugged mid-transfer | Discard partial image; resume normal runtime; user retries from start |
| Final SHA-256 mismatch | `FAILED { "hash_mismatch" }`; discard; resume runtime; Orion shows error |
| Slot full / size overflow | `REJECT { "too_large" }` at BEGIN |
| Update attempted during countdown | `REJECT { "countdown_active" }` — user retries after the meeting starts |
| Bootloader detects unhealthy first boot | Auto-rollback to previous slot on next boot; user sees old firmware again; Orion can re-attempt |
| Orion can't find Ori's serial port | UI prompt: "Plug Ori into this PC, then try again." If repeated failure, the bricked-unit recovery path (open enclosure, reflash over UART) applies — service-only. |
