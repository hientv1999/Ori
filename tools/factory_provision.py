#!/usr/bin/env python3
"""
factory_provision.py — Mass-production identity provisioning for the whole
Orinari ecosystem: Ori, Origale and Orimat (provisioning.md).

Serial number format:  DDMMYYNNNNCC   (12 digits, no prefix, no separators)
    DD    day of the month the unit was programmed
    MM    month
    YY    2-digit year
    NNNN  0-based index of this unit among those programmed that same day,
          for this same product
    CC    device type — 01 Ori, 02 Origale, 03 Orimat. The same value the
          IDENTITY frame carries as `device_type`, which is why no product
          prefix is needed: the serial already says what it is, in a form
          software compares directly.

    e.g. the first Ori programmed on 2026-07-26 is 260726000001.

There is no check digit. The previous format had one so a human could catch a
mis-typed serial unaided; it is gone because the serial is now machine-checked
where it matters — Orion compares it against the IDENTITY frame before a
firmware update, and a wrong digit fails that outright.

How the identity is written differs by product, because the silicon does:

    ori               a separate "factory" NVS partition (firmware/
                      partitions.csv), built with ESP-IDF's own
                      nvs_partition_gen.py and flashed with esptool. Holds the
                      serial only — the manufacture date is not stored
                      on-device anywhere; it's the serial's own leading
                      DDMMYY digits, and any consumer derives it from that
                      rather than being given a second, redundant copy.
    origale, orimat   a magic-delimited constant patched straight into the
                      built .bin (--image), because a CH32V003 has no NVS and
                      its firmware deliberately has no ability to write its own
                      flash. Serial only, same as ori.

Either way the value is written ONCE, at manufacturing time, by this script and
nothing else: no firmware anywhere in the ecosystem has a setter, and Ori's
partition is additionally outside nvs::factory_reset()'s blast radius
(firmware/src/factory_reset.cpp).

Uniqueness ledger:
    Every issued serial is appended to --ledger (default
    tools/factory_serials.csv, one row per unit: serial, mfg_date, note).
    The constraint is (date, device_type, index) — which is just "the serial is
    unique", since those three ARE the serial. Re-running always continues past
    the highest index already recorded for that date and product, so multiple
    sessions and batches can't collide. Rows in the old ORI-YYMM-NNNNNN-C
    format are retained as history and skipped when computing the next index.
    Keep this file (back it up / commit it somewhere durable) — it is the only
    record of what has already been issued.

Requirements:
    pip install esptool
    A working `nvs_partition_gen.py` on PATH — ships inside ESP-IDF at
    components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py, or
    install the standalone package: pip install esp-idf-nvs-partition-gen
    (provides the same tool as a console script). Either way this script
    just shells out to whatever `nvs_partition_gen.py` it finds on PATH —
    run `nvs_partition_gen.py --help` yourself first to confirm one is
    reachable before provisioning a real batch.

Usage:
    # Generate + flash identity for one unit, connected over USB:
    python tools/factory_provision.py --port COM9

    # Generate N images for an offline batch (e.g. a separate flashing
    # station), without touching any port:
    python tools/factory_provision.py --count 50 --out-dir batch017/

    # Explicit manufacture date (defaults to today):
    python tools/factory_provision.py --port COM9 --mfg-date 2026-07-12

Partition target (firmware/partitions.csv):
    name=factory  type=data  subtype=nvs  offset=0xFEC000  size=0x4000

This tool WRITES TO PHYSICAL FLASH when --port is given. It never touches
any partition other than "factory" — the bootloader, app slots, and the
main "nvs"/bonds partitions are untouched, so running it against an
already-flashed unit is safe (it does not erase pairing state, profile,
etc.). There is no duplicate/re-run guard, though: running it twice against
the same physical board just issues that board a SECOND serial number and
overwrites the first in flash — the ledger will then have two rows for one
unit. Nothing stops that automatically; keep track of which boards you've
already run it against yourself (e.g. don't leave the same board plugged in
across two invocations).
"""

import argparse
import csv
import datetime
import os
import shutil
import subprocess
import sys
import tempfile

FACTORY_PARTITION_OFFSET = "0xFEC000"
FACTORY_PARTITION_SIZE = "0x4000"          # must match firmware/partitions.csv
FACTORY_PARTITION_SIZE_BYTES = 0x4000
NVS_NAMESPACE = "factory"                   # must match firmware/src/factory_info.cpp

# Serial is DDMMYYNNNNCC (provisioning.md §2). CC is the IDENTITY frame's
# device_type, so these two tables are the same fact and must not drift —
# see Orion's .claude/rules/pc-app-usb-serial.md.
DEVICE_TYPES = {"ori": 1, "origale": 2, "orimat": 3}

# Origale/Orimat carry their serial in a magic-delimited constant that this
# script patches directly in the .bin — there is no NVS on a CH32V003 and no
# runtime flash write anywhere in that firmware. Must match
# ../Origale/firmware/src/serial_id.c.
SERIAL_BLOCK_MAGIC = b"ORISER01"
ESP32S3_USB_VID = "303A"                    # native USB — same VID mock_orion_ota.py looks for


def load_ledger(path):
    """Returns (existing_serials, next_index_by_scope).

    A serial's index is scoped to (programming date, device type) — that pair
    plus the index IS the serial, so the ledger's uniqueness constraint and the
    format are the same statement. `next_index_by_scope` is keyed on the
    DDMMYY+CC digits, i.e. everything in the serial except NNNN.

    Rows in the OLD `ORI-YYMM-NNNNNN-C` format are kept (they are the issued
    history and deleting them would let a number be reissued) but skipped for
    index purposes — they cannot collide with the new format, which has no
    hyphens.
    """
    if not os.path.exists(path):
        return set(), {}
    serials = set()
    next_index = {}
    with open(path, newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            serial = row["serial"]
            serials.add(serial)
            if len(serial) != 12 or not serial.isdigit():
                continue                       # legacy row — history only
            scope = serial[:6] + serial[10:]   # DDMMYY + CC
            nxt = int(serial[6:10]) + 1
            if nxt > next_index.get(scope, 0):
                next_index[scope] = nxt
    return serials, next_index


def append_ledger(path, serial, mfg_date, note):
    is_new = not os.path.exists(path)
    with open(path, "a", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        if is_new:
            w.writerow(["serial", "mfg_date", "note", "issued_at"])
        w.writerow([serial, mfg_date, note, datetime.datetime.now().isoformat(timespec="seconds")])


def next_serial(next_index, ddmmyy, cc, existing_serials):
    """The next DDMMYYNNNNCC for this (date, device type), skipping anything the
    ledger already has — a gap or a hand-added row must never be reissued."""
    idx = next_index.get(ddmmyy + cc, 0)
    while True:
        if idx > 9999:
            raise SystemExit(
                f"ERROR: all 10000 indices for {ddmmyy}/{cc} are used. The format "
                f"allows 10,000 units of one product per day; a run this large "
                f"needs a format revision, not a rollover (provisioning.md §2)."
            )
        serial = f"{ddmmyy}{idx:04d}{cc}"
        if serial not in existing_serials:
            next_index[ddmmyy + cc] = idx + 1
            return serial
        idx += 1


def patch_serial_into_image(src_bin, out_bin, serial):
    """Stamp `serial` into a CH32V003 image (Origale/Orimat).

    Finds SERIAL_BLOCK_MAGIC and overwrites the 8 bytes after it with the serial
    as a little-endian u64 — the layout serial_id.c declares. Patching a built
    image rather than rebuilding means every unit in a batch shares one compile,
    and firmware never needs the ability to write its own flash.

    Requires exactly one occurrence: zero means the firmware predates
    serial_id.c (or was built with it optimised away, which the `volatile`
    there exists to prevent), and more than one means the magic string appears
    somewhere it shouldn't and patching would be a guess.
    """
    image = bytearray(open(src_bin, "rb").read())
    hits = []
    start = image.find(SERIAL_BLOCK_MAGIC)
    while start != -1:
        hits.append(start)
        start = image.find(SERIAL_BLOCK_MAGIC, start + 1)

    if len(hits) != 1:
        raise SystemExit(
            f"ERROR: expected exactly one {SERIAL_BLOCK_MAGIC.decode()} block in "
            f"{src_bin}, found {len(hits)}. "
            + ("Is this firmware built with serial_id.c?" if not hits
               else "The magic is ambiguous; refusing to guess which to patch.")
        )

    at = hits[0] + len(SERIAL_BLOCK_MAGIC)
    image[at:at + 8] = int(serial).to_bytes(8, "little")
    with open(out_bin, "wb") as f:
        f.write(image)


def find_nvs_partition_gen():
    exe = shutil.which("nvs_partition_gen.py") or shutil.which("nvs_partition_gen")
    if exe:
        return [exe]
    # Fall back to `python -m` in case it's installed as a module without a
    # console-script shim on this platform.
    try:
        subprocess.run(
            [sys.executable, "-m", "esp_idf_nvs_partition_gen", "--help"],
            capture_output=True, check=True,
        )
        return [sys.executable, "-m", "esp_idf_nvs_partition_gen"]
    except Exception:
        return None


def build_factory_image(serial, out_bin):
    """Writes the one-row NVS CSV and shells out to nvs_partition_gen.py to
    produce the binary image nvs_partition_gen expects: a CSV with a leading
    "key,type,encoding,value" header, one "namespace" row, then one row per
    key.

    Only "sn" is written — the manufacture date is NOT stored on-device. It's
    the serial's own leading DDMMYY digits (provisioning.md §2), so a second
    on-device copy would just be the same fact twice with a chance to drift.
    `mfg_date` is still recorded in the ledger (append_ledger) purely as
    off-device factory bookkeeping, which is a different concern.
    """
    tool = find_nvs_partition_gen()
    if not tool:
        print(
            "ERROR: nvs_partition_gen.py not found on PATH.\n"
            "  Install via a full ESP-IDF checkout (components/nvs_flash/\n"
            "  nvs_partition_generator/nvs_partition_gen.py) or:\n"
            "    pip install esp-idf-nvs-partition-gen\n",
            file=sys.stderr,
        )
        sys.exit(1)

    with tempfile.NamedTemporaryFile(
        mode="w", suffix=".csv", delete=False, newline="", encoding="utf-8"
    ) as f:
        w = csv.writer(f)
        w.writerow(["key", "type", "encoding", "value"])
        w.writerow([NVS_NAMESPACE, "namespace", "", ""])
        w.writerow(["sn", "data", "string", serial])
        csv_path = f.name

    try:
        subprocess.run(
            tool + ["generate", csv_path, out_bin, str(FACTORY_PARTITION_SIZE_BYTES)],
            check=True,
        )
    finally:
        os.unlink(csv_path)


def flash_factory_image(port, bin_path):
    subprocess.run(
        [
            sys.executable, "-m", "esptool",
            "--chip", "esp32s3", "--port", port,
            "write_flash", FACTORY_PARTITION_OFFSET, bin_path,
        ],
        check=True,
    )


def find_esp32s3_port():
    """Best-effort auto-detect, same VID mock_orion_ota.py filters on. Returns
    None (never guesses) if zero or more than one candidate is found — the
    caller falls back to asking for --port explicitly.
    """
    try:
        from serial.tools import list_ports
    except ImportError:
        return None
    candidates = [p.device for p in list_ports.comports() if (p.vid and f"{p.vid:04X}" == ESP32S3_USB_VID)]
    return candidates[0] if len(candidates) == 1 else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--device", choices=sorted(DEVICE_TYPES), default="ori",
                     help="Which product is being provisioned. Decides the serial's CC digits and "
                          "how the identity is written: NVS partition for ori, .bin patch for the rest.")
    ap.add_argument("--image", help="For origale/orimat: the built firmware .bin to stamp the serial into.")
    ap.add_argument("--port", help="Serial port to flash one unit now (e.g. COM9). Omit for --count batch mode.")
    ap.add_argument("--count", type=int, default=1, help="Number of identities to generate (batch mode, no --port).")
    ap.add_argument("--out-dir", default=None, help="Directory to write generated .bin images (batch mode).")
    ap.add_argument("--mfg-date", default=None, help="ISO-8601 date, e.g. 2026-07-12. Defaults to today.")
    ap.add_argument("--ledger", default=os.path.join(os.path.dirname(__file__), "factory_serials.csv"),
                     help="CSV tracking every serial ever issued (default: tools/factory_serials.csv).")
    ap.add_argument("--note", default="", help="Free-text note stored alongside the serial in the ledger (e.g. batch/order id).")
    args = ap.parse_args()

    mfg_date = args.mfg_date or datetime.date.today().isoformat()
    try:
        datetime.date.fromisoformat(mfg_date)
    except ValueError:
        print(f"ERROR: --mfg-date must be ISO-8601 (YYYY-MM-DD), got {mfg_date!r}", file=sys.stderr)
        sys.exit(1)
    # DDMMYY — day first, unlike the ISO input and unlike the old YYMM scheme.
    ddmmyy = mfg_date[8:10] + mfg_date[5:7] + mfg_date[2:4]
    cc = f"{DEVICE_TYPES[args.device]:02d}"

    is_esp = args.device == "ori"
    if not is_esp:
        if not args.image:
            print(f"ERROR: --image is required for {args.device} (the .bin to stamp).", file=sys.stderr)
            sys.exit(1)
        if args.port:
            print(f"ERROR: --port flashing isn't implemented for {args.device}; generate images "
                  f"with --count/--out-dir and flash them with the WCH ISP tool.", file=sys.stderr)
            sys.exit(1)

    existing_serials, next_index = load_ledger(args.ledger)

    if args.port:
        count = 1
    else:
        count = args.count
        if not args.out_dir:
            print("ERROR: --out-dir is required in batch mode (no --port).", file=sys.stderr)
            sys.exit(1)
        os.makedirs(args.out_dir, exist_ok=True)

    port = args.port

    for i in range(count):
        serial = next_serial(next_index, ddmmyy, cc, existing_serials)
        existing_serials.add(serial)

        if args.port:
            bin_path = os.path.join(tempfile.gettempdir(), f"{serial}.bin")
        else:
            bin_path = os.path.join(args.out_dir, f"{serial}.bin")

        print(f"[{i + 1}/{count}] {serial}  {args.device}  mfg={mfg_date}")
        if is_esp:
            build_factory_image(serial, bin_path)
        else:
            # One compile, N stamped copies.
            patch_serial_into_image(args.image, bin_path, serial)

        if args.port:
            if port is None:
                port = find_esp32s3_port()
            if not port:
                print("ERROR: no --port given and couldn't auto-detect a single ESP32-S3 USB port.", file=sys.stderr)
                sys.exit(1)
            flash_factory_image(port, bin_path)
            os.unlink(bin_path)
            print(f"  → flashed to {port} at {FACTORY_PARTITION_OFFSET}")

        append_ledger(args.ledger, serial, mfg_date, args.note)

    print(f"\nDone. Ledger: {args.ledger}")


if __name__ == "__main__":
    main()
