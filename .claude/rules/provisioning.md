# Ori — Mass-Production Identity Provisioning

Every Ori unit gets one fact burned in at manufacturing time: a **serial
number**. It must survive everything short of a chip-level reflash —
firmware updates, factory resets, years of ordinary use — since it backs
warranty/RMA lookups and batch investigations. Firmware side:
`esp32-connectivity`/`firmware-core`. Manufacturing tooling: whoever runs the
flashing station.

**There is deliberately no separate on-device manufacture-date field**
(removed 2026-07-27). The serial's own leading `DDMMYY` digits (§2 below)
already are the manufacture date — anything that wants one derives it from
the serial it already has (Orion's Ori Info modal is the only consumer
today) instead of being sent a second, independently-stored copy of the same
fact that could in principle drift from the first. The factory ledger
(`tools/factory_serials.csv`) still records the full ISO `mfg_date` per row
— that's off-device bookkeeping for the flashing station's own records, a
different concern from what's stored on the unit itself.

---

## 1. Why a separate NVS partition

The main "nvs" partition (`firmware/partitions.csv`) is wiped wholesale by
`nvs::factory_reset()` (`Preferences::clear()`) on every factory reset — the
serial number must NOT be in that blast radius.

**Chosen: a physically separate NVS partition**, not just a different
namespace in the main one. A namespace would already survive today's
`factory_reset()` (which only clears the `"ori"` namespace), but a separate
partition is also immune to any *future* mistake — a debug `nvs_flash_erase()`
on the whole partition, a "wipe everything" troubleshooting step, a wrong
namespace string — since erasing a partition requires naming it explicitly.
Costs 16 KB of flash carved from the ~9.8 MB `spiffs` partition (ample
headroom — shrinking `spiffs` is already a known-safe operation, done once
before for the `coredump` partition). Worth it given the cost of getting this
wrong (re-flashing a boxed unit, or permanently losing its identity).

```
# firmware/partitions.csv
factory,   data, nvs,      0xFEC000, 0x4000,   # 16 KB, see below
```

- **Type/subtype**: plain `nvs`, a second instance of the same format. Opened
  via `Preferences::begin(name, readOnly, "factory")` (`firmware/src/factory_info.cpp`).
- **Namespace**: `"factory"`, one key `"sn"` (serial number).
- **Firmware never writes to it.** `factory_info.h` exposes only a
  `serial_number()` read accessor — no setter anywhere in firmware, BLE, or
  USB CDC. The only writer is the one-time manufacturing flash below —
  smallest possible attack/bug surface for permanent data.
- **Survives an OTA update**: USB CDC firmware updates (`ota.md`) only ever
  write the `ota_0`/`ota_1` app-slot partitions; every NVS partition is
  untouched by construction, same as bonds/profile today.

## 2. Serial number format

**One format across the whole Orinari ecosystem** — Ori, Origale and Orimat all
use it (Orion's `.claude/rules/pc-app-usb-serial.md`). Twelve digits, nothing
else: no product-code prefix, no separators.

```
DDMMYYNNNNCC
```

| Segment | Width | Meaning |
|---|---|---|
| `DD` | 2 | day of the month the unit was programmed |
| `MM` | 2 | month |
| `YY` | 2 | 2-digit year |
| `NNNN` | 4 | 0-based index of this unit among those programmed that same day, zero-padded |
| `CC` | 2 | device type — `01` Ori, `02` Origale, `03` Orimat |

Example: the first Ori programmed on 2026-07-26 is `260726000001`.

**Why the prefix went away.** `CC` already says what the product is, and it
says it in a form software can compare (it is the same value as the IDENTITY
frame's `device_type`). A leading `ORI`/`ORIGALE`/`ORIMAT` string was a second
encoding of the same fact, in a second place, that could disagree with the
first — and it made the serial a different width per product for no gain.

**Scope of `NNNN` is the day, not the month**, and it is shared across the
whole day's run for that device type — so the ledger's uniqueness constraint is
`(date, device_type, index)`. 10,000 units of one product in one day is the
ceiling; if a run ever approaches that, the format needs a real revision rather
than a rollover.

**There is no check digit.** The previous format carried a Luhn digit for
catching a mis-typed serial by eye. It is gone because the serial is now
machine-checked where it matters: Orion compares it byte-for-byte against the
IDENTITY frame before a firmware update, and a wrong digit fails that
comparison outright rather than being caught by arithmetic. Nothing in the
system asks a human to validate a serial unaided.

**`DDMMYY` is the only manufacture-date representation anywhere in the
system now** (revised 2026-07-27 — an earlier draft additionally stored a
full ISO-8601 `manufacture_date` in the factory partition and exposed it
over BLE as `"b"`; both are gone, see the top of this file). A consumer that
wants a 4-digit year prefixes `20` — every unit provisioned by this format
is a 21st-century one — rather than being sent a second, independently-set
copy of the same date that could disagree with the first. Orion's Ori Info
modal, the one place this is displayed, does exactly that (`pc-app-usb-serial.md`).

**The stored string is exactly 12 ASCII digits.** `identify_responder.cpp`
parses it to the u64 the wire carries and reports 0 for anything that isn't
exactly that — a partially-parsed serial would be a plausible *wrong* answer,
and Orion gates a destructive write on it.

## 3. Writing it at manufacturing time

**No custom firmware protocol** — a pre-built NVS partition image flashed
directly, via ESP-IDF's own tooling:

```
generate a 1-row NVS CSV (sn)
        │
        ▼
nvs_partition_gen.py generate <csv> <bin> 0x4000   ← ESP-IDF's own tool
        │
        ▼
esptool.py write_flash 0xFEC000 <bin>               ← same tool already used
                                                        to flash bootloader/app
```

Just one more `write_flash` call alongside whatever the flashing station
already does — same tool, same station, nothing Ori-specific beyond
`nvs_partition_gen.py` (ships with ESP-IDF, or `pip install
esp-idf-nvs-partition-gen`). Order vs. the app firmware flash doesn't matter
(independent partitions).

**Reference implementation:** `tools/factory_provision.py`, which provisions
**all three products** — it owns the shared ledger, so one tool is what keeps
`(date, device_type, index)` unique across the ecosystem. It:

- Generates the next serial for a date and product, tracked in a local
  append-only ledger (`tools/factory_serials.csv`) so re-running never
  collides or reissues a number. Old `ORI-YYMM-NNNNNN-C` rows are retained as
  history and skipped when computing the next index.
- **Ori:** builds the one-row NVS CSV and shells out to `nvs_partition_gen.py`,
  then either flashes over USB (`--port COM9`) or writes a batch of `.bin`
  images for an offline station (`--count N --out-dir batch017/`). `--mfg-date`
  still exists as an input — it's what supplies the serial's own `DDMMYY`
  digits — but nothing from it is written to the device beyond that; the
  ledger records the full date too, for the factory's own bookkeeping.
- **Origale/Orimat:** patches the serial straight into a built firmware image
  (`--image`), finding the `ORISER01` magic that `serial_id.c` declares and
  overwriting the 8 bytes after it with the serial as a little-endian u64. One
  compile, N stamped copies — and no runtime flash write anywhere in that
  firmware. It refuses to patch an image where the magic appears zero times
  (firmware built without `serial_id.c`) or more than once (ambiguous).

```
python tools/factory_provision.py --port COM9
python tools/factory_provision.py --count 50 --out-dir batch017/ --note "order #4821"
python tools/factory_provision.py --device origale --count 50        --image ../Origale/firmware/.pio/build/genericCH32V003F4P6/firmware.bin        --out-dir batch018/
```

Origale/Orimat images are flashed with the WCH ISP tool, not esptool, so
`--port` is rejected for those: generate the images and hand them to that
station.

**Keep the ledger file durable** — it's the only record of which serials
have already been issued.

### Firmware updates and the serial — Ori vs. the CH32 devices

**Ori's serial survives a firmware update, and that is a property of where it lives**, not luck:
USB CDC OTA (`ota.md`) writes only the `ota_0`/`ota_1` app slots, and the `factory` partition is a
different partition entirely (§1). The same goes for a factory reset, which clears the `"ori"`
namespace in the *main* NVS partition.

**Origale and Orimat do not get this for free.** Their serial is a constant inside the application
image (there is no NVS on a CH32V003), so flashing new firmware overwrites it and the unit comes
back reporting `serial == 0`. Any updater for those devices must read the serial over IDENTIFY
first, patch it into the replacement image — `patch_serial_into_image()` in
`tools/factory_provision.py` is exactly that operation, and is meant to be reused here — and flash
the patched result. Full rationale, including why a reserved flash page is the wrong answer, is in
[`../Orimat/orimat_design.md`](../../../Orimat/orimat_design.md) § "Consequence: a firmware update
destroys the serial unless it is carried forward".

### Verifying a unit after provisioning

Read char `000E` (Device Settings) back over BLE and confirm `"s"` matches
what was just written (Orion's own connect-time read path). Quick tools:
`tools/mock_orion_ble.py`'s BLE test harness, or Orion's Ori Info modal once
paired.

## 4. Exposure to Orion — no new BLE characteristic

The serial rides the **existing** Device Settings characteristic (char
`000E`) rather than a dedicated one, to avoid growing the GATT table for one
more static string. Wire detail: `ble-protocol.md` §4/§6.4.

- On **Read**, Ori's response gains `"s"` (serial_number), from
  `factory_info::serial_number()`.
- On **Write**, Ori's parser never looks for `"s"` — an incoming write that
  includes it is a no-op for that key (§4's "unknown keys ignored" rule),
  giving read-only semantics for free.
- Orion surfaces it in its **Ori Info modal** (`pc-app.md`), deriving a
  displayed manufacture date from the serial's own leading `DDMMYY` digits
  rather than reading a second field for it — alongside a piggybacked field,
  **live signal bars** (`"r"`) — Ori's own RSSI to Orion, sampled fresh on
  every read. Different motivation (Windows can't read a connected
  peripheral's RSSI locally, so Ori reports its own back) but the same "no
  new characteristic for one more small field" reasoning.

## 5. What this does NOT cover

- **Authenticity.** Nothing proves a serial is genuine — same caveat as
  `ota.md`'s firmware-integrity-vs-authenticity note. If ever needed:
  ESP32-S3 Secure Boot v2 + Flash Encryption at provisioning time (`ota.md`'s
  deferred-to-M8 plan), which would also encrypt the factory partition at
  rest. Not needed for this threat model today.
- **Hardware revision / calibration data.** Out of scope for this pass — the
  16 KB factory partition has headroom if wanted later, but nothing beyond
  `"sn"` is defined yet.
