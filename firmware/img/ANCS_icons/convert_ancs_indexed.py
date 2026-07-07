#!/usr/bin/env python
"""
Re-encode Ori's ANCS icon C arrays from raw ARGB8888 to LVGL indexed
(I1/I2/I4/I8) format to cut flash usage. Reads pixel data directly out of the
currently-compiled ancs_*.c files (not the original PNGs) so output is
guaranteed visually equivalent (or quantized-equivalent) to what's shipping,
regardless of whether a same-named PNG source exists for every token.

LVGL 9.5 binary layout (verified against lv_bin_decoder.c / lv_color.h):
  lv_image_dsc_t.data = palette[2**bpp] (lv_color32_t = B,G,R,A bytes each)
                         + indexed pixel data (bpp bits/px, MSB-first packing,
                           row stride = ceil(w*bpp/8) bytes)
  header.stride = that row stride (post-palette)
  LV_IMAGE_HEADER_MAGIC = 0x19

Picks the smallest format that's lossless (<=2/4/16/256 unique BGRA colors
-> I1/I2/I4/I8 exact palette); falls back to Pillow FASTOCTREE quantization
to fit I8 (256 colors, full per-entry alpha preserved) for busier icons.

Usage (run after convert_icons.py, or any time to re-tighten existing icons):
    python convert_ancs_indexed.py
    python convert_ancs_indexed.py path/to/firmware/src/assets

Dependencies:
    pip install pillow
"""
import os
import re
import sys
from PIL import Image

_ICON_DIR = os.path.dirname(os.path.abspath(__file__))
_DEFAULT_ASSETS_DIR = os.path.abspath(
    os.path.join(_ICON_DIR, "..", "..", "src", "assets"))
ASSETS_DIR = sys.argv[1] if len(sys.argv) > 1 else _DEFAULT_ASSETS_DIR

# token -> (.c filename, array symbol prefix) for everything currently linked
# into ancs_icons.cpp (48 brand icons + 9 category fallbacks + unknown_app).
TOKENS = [
    "amazon","apple_calendar","apple_findmy","apple_mail","apple_maps",
    "apple_music","apple_reminders","apple_wallet","chatgpt","claude",
    "discord","facebook","facetime","github","gmail","google_authenticator",
    "google_map","google_meet","google_photos","health","instagram","line",
    "linkedin","messenger","microsoft_authenticator","notion","outlook",
    "paypal","phone","reddit","skype","slack","sms","snapchat","spotify",
    "teams","telegram","threads","tiktok","twitch","twitter","uber","venmo",
    "viber","wechat","whatsapp","youtube","youtube_music","zoom",
    # category fallbacks + generic
    "cat_call","cat_health","email","entertainment","finance","location",
    "news","schedule","social","unknown",
]

# filename stem differs from the C symbol for these (cat_* fallbacks share a
# bare ancs_<category>.c filename but a cat_<category> symbol; unknown_app
# lives in ancs_unknown.c).
FILE_OVERRIDE = {
    "email": "ancs_email.c", "entertainment": "ancs_entertainment.c",
    "finance": "ancs_finance.c", "location": "ancs_location.c",
    "news": "ancs_news.c", "schedule": "ancs_schedule.c",
    "social": "ancs_social.c",
}
SYMBOL_OVERRIDE = {
    "email": "cat_email", "entertainment": "cat_entertainment",
    "finance": "cat_finance", "location": "cat_location",
    "news": "cat_news", "schedule": "cat_schedule", "social": "cat_social",
    "unknown": "unknown_app", "cat_call": "cat_call", "cat_health": "cat_health",
}

HEADER_TMPL = '''
#if defined(LV_LVGL_H_INCLUDE_SIMPLE)
#include "lvgl.h"
#elif defined(LV_LVGL_H_INCLUDE_SYSTEM)
#include <lvgl.h>
#elif defined(LV_BUILD_TEST)
#include "../lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif

#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

#ifndef LV_ATTRIBUTE_{macro}
#define LV_ATTRIBUTE_{macro}
#endif

// Re-encoded from ARGB8888 to {cf_name} to cut flash usage
// (tools/convert_ancs_indexed.py). {note}
static const
LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST LV_ATTRIBUTE_{macro}
uint8_t {symbol}_map[] = {{
{bytes_block}
}};

const lv_image_dsc_t {symbol} = {{
  .header = {{
    .magic = LV_IMAGE_HEADER_MAGIC,
    .cf = {cf_name},
    .flags = 0,
    .w = {w},
    .h = {h},
    .stride = {stride},
    .reserved_2 = 0,
  }},
  .data_size = sizeof({symbol}_map),
  .data = {symbol}_map,
  .reserved = NULL,
}};
'''

CF_FOR_BPP = {1: "LV_COLOR_FORMAT_I1", 2: "LV_COLOR_FORMAT_I2",
              4: "LV_COLOR_FORMAT_I4", 8: "LV_COLOR_FORMAT_I8"}


def parse_existing(path, symbol):
    text = open(path, "r", errors="ignore").read()
    m = re.search(r"\.w\s*=\s*(\d+)", text)
    w = int(m.group(1))
    m = re.search(r"\.h\s*=\s*(\d+)", text)
    h = int(m.group(1))
    assert "ARGB8888" in text, f"{path} is not ARGB8888 (already converted?)"
    m = re.search(re.escape(symbol) + r"_map\[\] = \{(.*?)\n\};", text, re.S)
    body = m.group(1)
    data = bytes(int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]{2}", body))
    assert len(data) == w * h * 4, f"{path}: {len(data)} != {w*h*4}"
    return w, h, data


def bgra_bytes_to_image(w, h, data):
    return Image.frombytes("RGBA", (w, h), data, "raw", "BGRA")


def build_palette_and_indices(img, w, h):
    """Return (bpp, palette[(B,G,R,A)...], indices[w*h])."""
    pixels = list(img.getdata())  # RGBA tuples
    uniq = []
    seen = {}
    indices = []
    lossless_ok = True
    for px in pixels:
        if px not in seen:
            if len(uniq) >= 256:
                lossless_ok = False
                break
            seen[px] = len(uniq)
            uniq.append(px)
        indices.append(seen[px])

    if lossless_ok:
        n = len(uniq)
        bpp = 1 if n <= 2 else 2 if n <= 4 else 4 if n <= 16 else 8
        pad = (1 << bpp) - n
        palette_rgba = uniq + [(0, 0, 0, 0)] * pad
        palette_bgra = [(r, g, b, a) for (r, g, b, a) in
                         [(p[0], p[1], p[2], p[3]) for p in palette_rgba]]
        # convert RGBA -> BGRA byte order for the palette table
        palette_bgra = [(b, g, r, a) for (r, g, b, a) in palette_rgba]
        return bpp, palette_bgra, indices, "lossless palette"

    # Busy icon (>256 unique colors): quantize with Pillow, preserving
    # per-entry alpha (FASTOCTREE keeps alpha varying per palette slot).
    q = img.quantize(colors=256, method=Image.Quantize.FASTOCTREE)
    pal_rgba_flat = q.getpalette("RGBA")
    palette_rgba = [tuple(pal_rgba_flat[i:i + 4]) for i in range(0, 1024, 4)]
    palette_bgra = [(b, g, r, a) for (r, g, b, a) in palette_rgba]
    indices = list(q.getdata())
    return 8, palette_bgra, indices, "quantized (FASTOCTREE, 256 colors)"


def pack_indices(bpp, indices, w, h):
    if bpp == 8:
        return bytes(indices), w
    px_per_byte = 8 // bpp
    stride = (w * bpp + 7) // 8
    out = bytearray(stride * h)
    for y in range(h):
        row = indices[y * w:(y + 1) * w]
        for x, idx in enumerate(row):
            byte_i = y * stride + x // px_per_byte
            shift = 8 - bpp - bpp * (x % px_per_byte)
            out[byte_i] |= (idx & ((1 << bpp) - 1)) << shift
    return bytes(out), stride


def format_bytes_block(data, per_line=16):
    lines = []
    for i in range(0, len(data), per_line):
        chunk = data[i:i + per_line]
        lines.append("    " + ",".join(f"0x{b:02x}" for b in chunk) + ",")
    return "\n".join(lines)


def main():
    total_before = 0
    total_after = 0
    results = []
    for token in TOKENS:
        fname = FILE_OVERRIDE.get(token, f"ancs_{token}.c")
        symbol = SYMBOL_OVERRIDE.get(token, token)
        path = os.path.join(ASSETS_DIR, fname)
        if not os.path.isfile(path):
            print(f"SKIP (missing): {path}")
            continue
        if "ARGB8888" not in open(path, "r", errors="ignore").read():
            print(f"SKIP (already indexed): {path}")
            continue
        w, h, data = parse_existing(path, symbol)
        before = w * h * 4 + 28  # header is small/fixed; ignore for delta calc, just track data
        img = bgra_bytes_to_image(w, h, data)
        bpp, palette, indices, note = build_palette_and_indices(img, w, h)
        packed, stride = pack_indices(bpp, indices, w, h)
        palette_bytes = b"".join(bytes(p) for p in palette)
        full_data = palette_bytes + packed
        after = len(full_data)
        total_before += w * h * 4
        total_after += after

        macro = symbol.upper()
        cf_name = CF_FOR_BPP[bpp]
        content = HEADER_TMPL.format(
            macro=macro, cf_name=cf_name, w=w, h=h, stride=stride,
            symbol=symbol, bytes_block=format_bytes_block(full_data),
            note=note,
        )
        with open(path, "w") as f:
            f.write(content)
        results.append((token, w * h * 4, after, bpp, note))
        print(f"{token:28s} {w*h*4:7d} -> {after:6d} bytes  "
              f"(I{bpp}, {note})")

    print()
    print(f"TOTAL raw pixel data: {total_before:,} -> {total_after:,} bytes "
          f"({100*(1-total_after/total_before):.1f}% reduction)")


if __name__ == "__main__":
    main()
