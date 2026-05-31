# Ori — Firmware Update

Firmware updates run over **USB CDC** on the existing USB-C power cable. No BLE OTA, no USB-MSC (Ori must not appear as a removable drive). Bricked-unit recovery uses the internal UART port — service path only.

| Property | Value |
|---|---|
| Initiator | Orion (Settings → "Install update") |
| Transport | USB CDC over the existing USB-C power cable |
| Time for ~1.5 MB image | ~10–30 s |
| Driver requirement | None — native USB CDC on Win 10+, macOS, Linux |
| Visibility in file managers | None — enumerates as COM/tty, never as a drive |

## Firmware implementation

- Arduino **`Update`** library: `Update.begin()`, `Update.write(buf, len)`, `Update.end(true)`.
- `Update.canRollBack()` / `Update.rollBack()` for the post-boot verify path.
- Two 3 MB OTA slots (`ota_0`, `ota_1`) + OTA-data partition — see `firmware/partitions.csv`.
- Pending-verify window: **60 seconds** after first boot. If new firmware fails (panic, watchdog, no USB/BLE), the bootloader rolls back automatically on next boot.

## Transport

- ESP32-S3 native USB peripheral; no USB-to-serial chip. Already enabled: `-D ARDUINO_USB_CDC_ON_BOOT=1` in `platformio.ini`.
- Orion uses `flutter_libserialport` (or equivalent). Driverless on Win 10+, macOS, Linux.
- The same CDC port carries firmware boot logs (`Serial.printf`). The framing protocol below separates log bytes from OTA frames.

## Wire flow

```
Orion checks ori.app for latest version vs fw_version in BLE Protocol Version char.
If newer exists:
   Orion UI: Settings → "Update available · 1.2.3" → tap "Install update"

Orion opens the serial port to the USB-C-connected Ori.
   If no port found: prompt "Plug Ori into this PC, then try again."

Orion writes OTA frame: BEGIN { fw_version, total_size, sha256 }

Ori validates: version != current; size <= inactive slot capacity; no countdown active.
   On reject: respond REJECT { reason }; return to normal runtime.
   On accept: Update.begin(inactive_slot, total_size);
              switch screen to OTA-Updating;
              respond READY.

Orion streams DATA frames. Firmware feeds each into Update.write().
Every ~5% of total_size, Ori responds PROGRESS { bytes_received }.
After last DATA, Orion writes END.

Ori computes SHA-256 over the received image.
   On match:    Update.end(true); respond VALIDATED; sleep 200 ms; ESP.restart().
   On mismatch: Update.abort();   respond FAILED { "hash_mismatch" }; resume runtime.

After reboot:
   Bootloader runs new firmware in pending-verify state.
   Firmware enumerates USB, brings up BLE, advertises to bonded peers.
   60 s healthy → mark partition valid.
   60 s unhealthy (panic / watchdog) → bootloader rolls back on next boot.

Orion reconnects over BLE, reads Protocol Version, confirms new fw_version.
```

## Framing (USB CDC OTA protocol)

Each frame:

```
Offset  Size  Field
0       2     magic       = 0x4F54 ("OT" — distinguishes OTA frames from log bytes)
2       1     op
3       3     payload_len (uint24, little-endian)
6       N     payload     (CBOR for control ops; raw bytes for DATA)
```

Log bytes from `Serial.printf` never start with `0x4F54`; Orion surfaces them as boot-log output.

```
op  Name        Direction  Payload
01  BEGIN       PC → Ori   CBOR: { fw_version: text, total_size: uint, sha256: bytes(32) }
02  READY       Ori → PC   CBOR: {}
03  REJECT      Ori → PC   CBOR: { reason: text }
04  DATA        PC → Ori   raw bytes
05  PROGRESS    Ori → PC   CBOR: { bytes_received: uint }
06  END         PC → Ori   CBOR: {}
07  VALIDATED   Ori → PC   CBOR: {}
08  FAILED      Ori → PC   CBOR: { reason: text }
```

DATA has no per-frame sequence numbers — CDC delivers in order; firmware counts bytes against `total_size`.

## On-device UX during update

- Full-screen "Updating firmware… N%" page (`screen_ota_updating.cpp`); status bar and profile card hidden.
- All touch inert — mode-toggle and long-press triggers do nothing.
- **BLE writes NACKed** for all data characteristics (Profile Info, Photo, Meeting List, PTO Entry, Factory Reset Command) — Orion retries after reboot.
- **`Keyboard Command` notifies suspended** for the duration.
- Non-dismissable. Unplugging USB-C safely discards the partial image; the active slot is untouched. No resumability — Orion restarts from `BEGIN` on the next attempt.

## When an update may not start

Ori rejects `BEGIN` with `REJECT { reason }` when:

- 5-minute countdown modal is active (`"countdown_active"`)
- `total_size` exceeds inactive slot capacity (`"too_large"`)
- `fw_version` matches the currently running firmware (`"already_current"`)

USB CDC OTA does **not** require a BLE bond — physical cable access is sufficient authority.

## Bricked-unit recovery

If USB CDC won't enumerate or won't respond to `BEGIN`: open the enclosure, connect the internal UART port, reflash with `esptool.py write_flash`. Service / RMA path only — UART is not customer-accessible. See `hardware.md`.

## Failure modes

| Failure | Behavior |
|---|---|
| USB unplugged mid-transfer | Partial image discarded; Orion restarts from `BEGIN` |
| SHA-256 mismatch | `FAILED { "hash_mismatch" }`; image discarded; resume runtime |
| Size overflow | `REJECT { "too_large" }` at BEGIN |
| Countdown active | `REJECT { "countdown_active" }` — retry after meeting starts |
| Unhealthy first boot | Auto-rollback on next boot; Orion can re-attempt |
| Serial port not found | Orion prompts "Plug Ori into this PC, then try again." |
