"""
ANCS icon build pipeline — crop to 60×60 then convert to LVGL C arrays.

Usage:
    python convert_icons.py
    python convert_icons.py --lvgl-script path/to/LVGLImage.py

LVGLImage.py is auto-detected from the PlatformIO libdeps folder if not specified.

Dependencies:
    pip install pillow pypng lz4
"""

import os
import sys
import subprocess
import argparse
import shutil
import tempfile
from PIL import Image

# ── Tunable parameters ────────────────────────────────────────────────────────
SIZE        = (60, 60)          # target icon dimensions
COLOR_FMT   = "ARGB8888"        # RGB565 | ARGB8888 | RGB888
COMPRESS    = "NONE"            # NONE | RLE | LZ4
ALIGN       = 1                 # stride alignment in bytes
DITHER      = False             # RGB565 dithering (not needed for ARGB8888)
# ─────────────────────────────────────────────────────────────────────────────

# Map image filename stem -> firmware token when they differ.
RENAME_MAP = {
    "X": "twitter",
}

ICON_DIR    = os.path.dirname(os.path.abspath(__file__))
OUTPUT_DIR  = os.path.abspath(os.path.join(ICON_DIR, "..", "src", "assets"))
CROPPED_DIR = os.path.join(ICON_DIR, "cropped")
EXTS        = (".png", ".jpg", ".jpeg", ".webp")

# Default LVGLImage.py location inside PlatformIO libdeps.
DEFAULT_LVGL_SCRIPT = os.path.abspath(
    os.path.join(ICON_DIR, "..", ".pio", "libdeps", "ori", "lvgl", "scripts", "LVGLImage.py")
)


def crop(src: str) -> str:
    """Resize to SIZE preserving transparency. Saves to CROPPED_DIR as PNG. Returns dest path."""
    img = Image.open(src).convert("RGBA")
    img = img.resize(SIZE, Image.LANCZOS)
    stem = os.path.splitext(os.path.basename(src))[0]
    dest = os.path.join(CROPPED_DIR, stem + ".png")
    img.save(dest)
    print(f"  cropped   {os.path.basename(src)} -> cropped/{stem}.png ({SIZE[0]}x{SIZE[1]})")
    return dest


def convert(src: str, token: str, lvgl_script: str):
    # src is always a PNG from the crop step.
    tmp = tempfile.mkdtemp()
    try:
        input_path = src

        cmd = [
            sys.executable, lvgl_script,
            input_path,
            "--ofmt", "C",
            "--cf",   COLOR_FMT,
            "--compress", COMPRESS,
            "--align", str(ALIGN),
            "--name", token,
            "-o", tmp,
        ]
        if DITHER:
            cmd.append("--rgb565dither")

        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            print(f"  ERROR    {os.path.basename(src)}:\n{result.stderr.strip()}")
            return

        # LVGLImage.py may name the output after the input stem or after --name.
        # Find whichever .c file appeared in tmp.
        c_files = [f for f in os.listdir(tmp) if f.endswith(".c")]
        if not c_files:
            print(f"  ERROR    no .c output produced for {os.path.basename(src)}")
            return

        dest = os.path.join(OUTPUT_DIR, f"ancs_{token}.c")
        shutil.move(os.path.join(tmp, c_files[0]), dest)
        print(f"  converted {os.path.basename(src)} -> ancs_{token}.c  ({COLOR_FMT}, compress={COMPRESS})")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(description="Crop + convert ANCS icons for LVGL firmware.")
    parser.add_argument("--lvgl-script", default=DEFAULT_LVGL_SCRIPT,
                        help="Path to LVGLImage.py (auto-detected from libdeps if omitted).")
    args = parser.parse_args()

    lvgl_script = os.path.abspath(args.lvgl_script)
    if not os.path.isfile(lvgl_script):
        print(f"ERROR: LVGLImage.py not found at:\n  {lvgl_script}")
        print("Pass --lvgl-script <path> or install LVGL via PlatformIO first.")
        sys.exit(1)

    os.makedirs(OUTPUT_DIR, exist_ok=True)
    os.makedirs(CROPPED_DIR, exist_ok=True)

    images = sorted([
        os.path.join(ICON_DIR, f)
        for f in os.listdir(ICON_DIR)
        if os.path.splitext(f)[1].lower() in EXTS
    ])

    if not images:
        print("No PNG/JPG files found in ANCS_icons/")
        sys.exit(0)

    print(f"LVGLImage.py : {lvgl_script}")
    print(f"Output       : {OUTPUT_DIR}")
    print(f"Format       : {COLOR_FMT}, compress={COMPRESS}, align={ALIGN}")
    print(f"Processing {len(images)} icon(s)...\n")

    for path in images:
        stem    = os.path.splitext(os.path.basename(path))[0]
        token   = RENAME_MAP.get(stem, stem.lower())
        cropped = crop(path)
        convert(cropped, token, lvgl_script)

    print("\nDone.")


if __name__ == "__main__":
    main()
