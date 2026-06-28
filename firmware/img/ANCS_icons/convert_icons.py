"""
ANCS icon build pipeline — resize to 60×60 and convert to LVGL C arrays in one step.

Drop your PNG/JPG/WebP icons into this folder named after their app token:
    gmail.png  slack.png  whatsapp.png  ...

Run:
    python convert_icons.py
    python convert_icons.py --lvgl-script path/to/LVGLImage.py

LVGLImage.py is auto-detected from the PlatformIO libdeps folder if not specified.
Originals are never modified — resized copies go to ANCS_icons/cropped/.
Output C arrays go to firmware/src/assets/ancs_{token}.c.

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
SIZE        = (60, 60)      # status-bar icon tile size
COLOR_FMT   = "ARGB8888"    # RGB565 | ARGB8888 | RGB888
COMPRESS    = "NONE"        # NONE | RLE | LZ4
ALIGN       = 1             # stride alignment in bytes

# Ori's screen background (theme.h COLOR_BG) is pure black, so a black pixel
# inside an icon disappears into it and the icon's round tile edge vanishes.
# Any near-black opaque pixel is remapped to COLOR_ELEV (theme.h) instead.
BLACK_THRESHOLD   = 10               # max R/G/B value still treated as "black"
DARK_REPLACEMENT  = (0x16, 0x1B, 0x23)  # theme.h COLOR_ELEV
# ─────────────────────────────────────────────────────────────────────────────

# Filename stem → firmware token when they differ (e.g. rebranded app names).
RENAME_MAP = {
    "X": "twitter",
}

ICON_DIR    = os.path.dirname(os.path.abspath(__file__))
FIRMWARE_DIR = os.path.abspath(os.path.join(ICON_DIR, "..", ".."))
OUTPUT_DIR  = os.path.join(FIRMWARE_DIR, "src", "assets")
CROPPED_DIR = os.path.join(ICON_DIR, "cropped")
EXTS        = (".png", ".jpg", ".jpeg", ".webp")

DEFAULT_LVGL_SCRIPT = os.path.join(
    FIRMWARE_DIR, ".pio", "libdeps", "ori", "lvgl", "scripts", "LVGLImage.py"
)


def recolor_black(img: Image.Image) -> Image.Image:
    """Remap near-black opaque pixels to DARK_REPLACEMENT so they don't vanish
    into Ori's black screen background. Transparent pixels are left alone."""
    pixels = img.load()
    w, h = img.size
    for y in range(h):
        for x in range(w):
            r, g, b, a = pixels[x, y]
            if a > 0 and r <= BLACK_THRESHOLD and g <= BLACK_THRESHOLD and b <= BLACK_THRESHOLD:
                pixels[x, y] = (*DARK_REPLACEMENT, a)
    return img


def resize(src: str) -> str:
    """Resize to SIZE, preserve transparency, recolor black pixels. Saves to CROPPED_DIR as PNG. Returns dest path."""
    img  = Image.open(src).convert("RGBA")
    img  = img.resize(SIZE, Image.LANCZOS)
    img  = recolor_black(img)
    stem = os.path.splitext(os.path.basename(src))[0]
    dest = os.path.join(CROPPED_DIR, stem + ".png")
    img.save(dest)
    print(f"  resized   {os.path.basename(src)} -> cropped/{stem}.png ({SIZE[0]}x{SIZE[1]})")
    return dest


def convert(src: str, token: str, lvgl_script: str):
    """Convert a cropped PNG to an LVGL C array in OUTPUT_DIR."""
    tmp = tempfile.mkdtemp()
    try:
        cmd = [
            sys.executable, lvgl_script,
            src,
            "--ofmt",     "C",
            "--cf",       COLOR_FMT,
            "--compress", COMPRESS,
            "--align",    str(ALIGN),
            "--name",     token,
            "-o",         tmp,
        ]
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            print(f"  ERROR    {os.path.basename(src)}:\n{result.stderr.strip()}")
            return

        c_files = [f for f in os.listdir(tmp) if f.endswith(".c")]
        if not c_files:
            print(f"  ERROR    no .c output for {os.path.basename(src)}")
            return

        dest = os.path.join(OUTPUT_DIR, f"ancs_{token}.c")
        shutil.move(os.path.join(tmp, c_files[0]), dest)
        print(f"  converted {os.path.basename(src)} -> ancs_{token}.c  ({COLOR_FMT})")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(
        description="Resize ANCS icons to 60×60 and convert to LVGL C arrays."
    )
    parser.add_argument("--lvgl-script", default=DEFAULT_LVGL_SCRIPT,
                        help="Path to LVGLImage.py (auto-detected from libdeps if omitted).")
    args = parser.parse_args()

    lvgl_script = os.path.abspath(args.lvgl_script)
    if not os.path.isfile(lvgl_script):
        print(f"ERROR: LVGLImage.py not found at:\n  {lvgl_script}")
        print("Pass --lvgl-script <path> or run 'pio run' once to install LVGL via PlatformIO.")
        sys.exit(1)

    os.makedirs(OUTPUT_DIR, exist_ok=True)
    os.makedirs(CROPPED_DIR, exist_ok=True)

    images = sorted([
        os.path.join(ICON_DIR, f)
        for f in os.listdir(ICON_DIR)
        if os.path.splitext(f)[1].lower() in EXTS
    ])

    if not images:
        print("No PNG/JPG/WebP files found in ANCS_icons/")
        sys.exit(0)

    print(f"LVGLImage.py : {lvgl_script}")
    print(f"Output       : {OUTPUT_DIR}")
    print(f"Format       : {COLOR_FMT}, compress={COMPRESS}, align={ALIGN}")
    print(f"Processing {len(images)} icon(s)...\n")

    for path in images:
        stem    = os.path.splitext(os.path.basename(path))[0]
        token   = RENAME_MAP.get(stem, stem.lower())
        cropped = resize(path)
        convert(cropped, token, lvgl_script)

    print("\nDone.")


if __name__ == "__main__":
    main()
