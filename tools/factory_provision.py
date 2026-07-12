#!/usr/bin/env python3
"""
factory_provision.py — Mass-production identity provisioning for Ori
(provisioning.md).

Writes exactly two facts into each unit's separate "factory" NVS partition
(firmware/partitions.csv) BEFORE (or after) the firmware itself is flashed:

    sn   serial number,     e.g. "ORI-2607-000123-4"
    mfg  manufacture date,  e.g. "2026-07-12"  (ISO-8601, defaults to today)

That partition is never written by firmware at runtime (factory_info.h has
no setter) and is never touched by nvs::factory_reset() (firmware/src/
factory_reset.cpp) — this script is the ONLY writer, and it only ever runs
once per unit, at manufacturing time.

Serial number format:  ORI-YYMM-NNNNNN-C
    ORI    fixed product code
    YYMM   year+month this batch was provisioned (2-digit year, 2-digit month)
    NNNNNN 6-digit sequence number within the ledger (see below), zero-padded
    C      single check digit (Luhn mod 10 over the preceding digits) — lets
           a human reading a serial off a label/RMA form catch a mis-typed
           digit before it's looked up, without needing a live database.

Uniqueness ledger:
    Every issued serial is appended to --ledger (default
    tools/factory_serials.csv, one row per unit: serial, mfg_date, note).
    This file is the source of truth for "next sequence number" — re-running
    the script always continues from the highest sequence number already in
    the ledger, so it's safe to run in multiple sessions/batches without
    colliding. Keep this file (back it up / commit it somewhere durable) —
    it's the only record of what's already been issued.

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
PRODUCT_CODE = "ORI"
ESP32S3_USB_VID = "303A"                    # native USB — same VID mock_orion_ota.py looks for


def luhn_check_digit(digits: str) -> str:
    """Standard Luhn (mod 10) check digit over an all-numeric string."""
    total = 0
    parity = len(digits) % 2
    for i, ch in enumerate(digits):
        d = int(ch)
        if i % 2 == parity:
            d *= 2
            if d > 9:
                d -= 9
        total += d
    return str((10 - (total % 10)) % 10)


def load_ledger(path):
    """Returns (rows, next_sequence). rows is the raw list of dict rows already
    in the ledger (for --allow-reissue duplicate checks); next_sequence is one
    past the highest NNNNNN seen for THIS YYMM prefix — sequence numbers are
    scoped per year-month batch, not globally, so a busy month doesn't eat
    into headroom future months don't need.
    """
    if not os.path.exists(path):
        return [], {}
    rows = []
    next_seq_by_prefix = {}
    with open(path, newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            rows.append(row)
            serial = row["serial"]
            # ORI-YYMM-NNNNNN-C
            parts = serial.split("-")
            if len(parts) != 4:
                continue
            prefix, seq_str = parts[1], parts[2]
            seq = int(seq_str) + 1
            if seq > next_seq_by_prefix.get(prefix, 1):
                next_seq_by_prefix[prefix] = seq
    return rows, next_seq_by_prefix


def append_ledger(path, serial, mfg_date, note):
    is_new = not os.path.exists(path)
    with open(path, "a", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        if is_new:
            w.writerow(["serial", "mfg_date", "note", "issued_at"])
        w.writerow([serial, mfg_date, note, datetime.datetime.now().isoformat(timespec="seconds")])


def next_serial(next_seq_by_prefix, yymm, existing_serials):
    seq = next_seq_by_prefix.get(yymm, 1)
    while True:
        digits = f"{yymm}{seq:06d}"  # numeric portion only — PRODUCT_CODE ("ORI")
                                      # isn't a digit and Luhn only checksums digits
        check = luhn_check_digit(digits)
        serial = f"{PRODUCT_CODE}-{yymm}-{seq:06d}-{check}"
        if serial not in existing_serials:
            next_seq_by_prefix[yymm] = seq + 1
            return serial
        seq += 1  # ledger had a gap/manual entry — skip past it


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


def build_factory_image(serial, mfg_date, out_bin):
    """Writes the two-row NVS CSV and shells out to nvs_partition_gen.py to
    produce the binary image nvs_partition_gen expects: a CSV with a leading
    "key,type,encoding,value" header, one "namespace" row, then one row per
    key.
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
        w.writerow(["mfg", "data", "string", mfg_date])
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
    yymm = mfg_date[2:4] + mfg_date[5:7]

    rows, next_seq_by_prefix = load_ledger(args.ledger)
    existing_serials = {r["serial"] for r in rows}

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
        serial = next_serial(next_seq_by_prefix, yymm, existing_serials)
        existing_serials.add(serial)

        if args.port:
            bin_path = os.path.join(tempfile.gettempdir(), f"{serial}.bin")
        else:
            bin_path = os.path.join(args.out_dir, f"{serial}.bin")

        print(f"[{i + 1}/{count}] {serial}  mfg={mfg_date}")
        build_factory_image(serial, mfg_date, bin_path)

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
