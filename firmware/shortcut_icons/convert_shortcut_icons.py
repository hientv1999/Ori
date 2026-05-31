"""
Shortcut icon build pipeline — crop to 40×40 then convert to LVGL C arrays.

Place your PNG/JPG icons here named after their token:
    vol-mute.png
    mic-mute.png
    screenshot.png

Usage:
    python convert_shortcut_icons.py

LVGLImage.py is auto-detected from the PlatformIO libdeps folder.

Dependencies:
    pip install pillow pypng
"""

import os
import sys
import subprocess
import argparse
import shutil
import tempfile
from PIL import Image, ImageOps

# ── Tunable parameters ────────────────────────────────────────────────────────
SIZE        = (40, 40)          # must match .s-btn svg size in HTML (40x40)
COLOR_FMT   = "ARGB8888"        # RGB565 | ARGB8888 | RGB888
COMPRESS    = "NONE"            # NONE | RLE | LZ4
ALIGN       = 1                 # stride alignment in bytes
DITHER      = False
# ─────────────────────────────────────────────────────────────────────────────

ICON_DIR    = os.path.dirname(os.path.abspath(__file__))
OUTPUT_DIR  = os.path.abspath(os.path.join(ICON_DIR, "..", "src", "assets"))
CROPPED_DIR = os.path.join(ICON_DIR, "cropped")   # originals never touched
EXTS        = (".png", ".jpg", ".jpeg", ".webp")

DEFAULT_LVGL_SCRIPT = os.path.abspath(
    os.path.join(ICON_DIR, "..", ".pio", "libdeps", "ori", "lvgl", "scripts", "LVGLImage.py")
)

# Map filename stem -> C variable name (hyphens are invalid in C identifiers).
def token_to_cname(stem):
    return stem.replace("-", "_")


def crop(src):
    """Resize to SIZE, invert RGB (black->white), preserve alpha. Saves to CROPPED_DIR as PNG."""
    img = Image.open(src).convert("RGBA")
    img = img.resize(SIZE, Image.LANCZOS)
    r, g, b, a = img.split()
    r, g, b = ImageOps.invert(r), ImageOps.invert(g), ImageOps.invert(b)
    img = Image.merge("RGBA", (r, g, b, a))
    stem = os.path.splitext(os.path.basename(src))[0]
    dest = os.path.join(CROPPED_DIR, stem + ".png")
    img.save(dest)
    print(f"  cropped   {os.path.basename(src)} -> cropped/{stem}.png ({SIZE[0]}x{SIZE[1]})")
    return dest


def convert(src, token, cname, lvgl_script):
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
            "--name", cname,
            "-o", tmp,
        ]
        if DITHER:
            cmd.append("--rgb565dither")

        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            print(f"  ERROR    {os.path.basename(src)}:\n{result.stderr.strip()}")
            return

        c_files = [f for f in os.listdir(tmp) if f.endswith(".c")]
        if not c_files:
            print(f"  ERROR    no .c output for {os.path.basename(src)}")
            return

        dest = os.path.join(OUTPUT_DIR, f"shortcut_{cname}.c")
        shutil.move(os.path.join(tmp, c_files[0]), dest)
        print(f"  converted {os.path.basename(src)} -> shortcut_{cname}.c  ({COLOR_FMT}, compress={COMPRESS})")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--lvgl-script", default=DEFAULT_LVGL_SCRIPT)
    args = parser.parse_args()

    lvgl_script = os.path.abspath(args.lvgl_script)
    if not os.path.isfile(lvgl_script):
        print(f"ERROR: LVGLImage.py not found at:\n  {lvgl_script}")
        sys.exit(1)

    os.makedirs(OUTPUT_DIR, exist_ok=True)
    os.makedirs(CROPPED_DIR, exist_ok=True)

    images = sorted([
        os.path.join(ICON_DIR, f)
        for f in os.listdir(ICON_DIR)
        if os.path.splitext(f)[1].lower() in EXTS
    ])

    if not images:
        print("No PNG/JPG files found in shortcut_icons/")
        print("Add: vol-mute.png  mic-mute.png  screenshot.png")
        sys.exit(0)

    print(f"LVGLImage.py : {lvgl_script}")
    print(f"Output       : {OUTPUT_DIR}")
    print(f"Format       : {COLOR_FMT}, compress={COMPRESS}, align={ALIGN}")
    print(f"Processing {len(images)} icon(s)...\n")

    for path in images:
        stem    = os.path.splitext(os.path.basename(path))[0]
        cname   = token_to_cname(stem)
        cropped = crop(path)
        convert(cropped, stem, cname, lvgl_script)

    print("\nDone.")


if __name__ == "__main__":
    main()
