#!/usr/bin/env python
"""
Download a curated set of Microsoft Fluent Emoji (MIT-licensed) 3D glyphs and
bake them into LVGL RGB565+A8 image descriptors compiled into flash. These feed
an LVGL imgfont registered as a .fallback on the UI text fonts, so emoji that
appear in ANCS notification text render inline instead of being dropped by
ui::sanitize_text(). See emoji_font.cpp / theme.cpp for the runtime wiring.

Source: https://github.com/microsoft/fluentui-emoji  (MIT License)
  Non-skin-tone emoji:  assets/<Folder>/3D/<slug>_3d.png
  Skin-tone-capable:    assets/<Folder>/Default/3D/<slug>_3d_default.png
Codepoint comes from each folder's metadata.json ("unicode" field); the first
codepoint (the base) is the registry key (VS16/ZWJ tails are ignored — the base
alone maps to the image, matching how sanitize_text keeps the base + drops
modifiers).

Outputs (overwrites):
  src/assets/emoji_assets.c
  include/assets/emoji_assets.h

Requires: Pillow  (pip install pillow).  Run from firmware/:
  python img/emoji/convert_emoji.py
Then rebuild:  pio run -e ori
"""
import os, sys, urllib.request, urllib.parse, json, io
from PIL import Image

EMOJI_PX   = 28                      # rendered glyph height (px)
MAX_EMOJI  = 100                     # hard cap (flash budget) — see FOLDERS below
RAW        = "https://raw.githubusercontent.com/microsoft/fluentui-emoji/main/assets/"

HERE       = os.path.dirname(os.path.abspath(__file__))
FW         = os.path.abspath(os.path.join(HERE, "..", ".."))
OUT_C      = os.path.join(FW, "src", "assets", "emoji_assets.c")
OUT_H      = os.path.join(FW, "include", "assets", "emoji_assets.h")

# Curated folders (Fluent CLDR names), in real-world usage-frequency order
# (Unicode Consortium median frequency data, cross-referenced 2026-07 against
# this repo's own metadata.json for each name to confirm the codepoint match
# — Fluent's folder names don't always match the raw Unicode 1.0 character
# name, e.g. U+1F44C is "Ok hand" here, not "OK hand"). Previously an
# ~250-entry category-broad list; replaced with the top 100 emoji by actual
# usage, which covers ~80% of real top-100 usage on its own (the old list
# only reached that by accident — most of its 250 slots went to long-tail
# individual food/animal/travel icons that barely register in real usage).
# Order matters if MAX_EMOJI is ever lowered further: earlier entries are
# more frequently used and should be kept first.
FOLDERS = [
    "Face with tears of joy", "Red heart", "Smiling face with heart-eyes", "Rolling on the floor laughing",
    "Smiling face with smiling eyes", "Folded hands", "Two hearts", "Loudly crying face",
    "Face blowing a kiss", "Thumbs up", "Grinning face with sweat", "Clapping hands",
    "Beaming face with smiling eyes", "Heart suit", "Fire", "Broken heart",
    "Sparkling heart", "Blue heart", "Crying face", "Thinking face",
    "Grinning squinting face", "Face with rolling eyes", "Flexed biceps", "Winking face",
    "Smiling face", "Ok hand", "Hugging face", "Purple heart",
    "Pensive face", "Smiling face with sunglasses", "Smiling face with halo", "Rose",
    "Person facepalming", "Party popper", "Double exclamation mark", "Revolving hearts",
    "Victory hand", "Sparkles", "Person shrugging", "Face screaming in fear",
    "Relieved face", "Cherry blossom", "Raising hands", "Face savoring food",
    "Growing heart", "Green heart", "Smirking face", "Yellow heart",
    "Slightly smiling face", "Beating heart", "Star-struck", "Grinning face with smiling eyes",
    "Grinning face", "Black heart", "Grinning face with big eyes", "Hundred points",
    "See-no-evil monkey", "Backhand index pointing down", "Musical notes", "Unamused face",
    "Face with hand over mouth", "Heart exclamation", "Red exclamation mark", "Winking face with tongue",
    "Kiss mark", "Eyes", "Sleepy face", "Expressionless face",
    "Collision", "Person raising hand", "Disappointed face", "Weary face",
    "Pouting face", "Zany face", "Oncoming fist", "Sun",
    "Sad but relieved face", "Drooling face", "Backhand index pointing right", "Woman dancing",
    "Flushed face", "Raised hand", "Kissing face with closed eyes", "Squinting face with tongue",
    "Sleeping face", "Glowing star", "Grimacing face", "Upside-down face",
    "Four leaf clover", "Tulip", "Smiling cat with heart-eyes", "Downcast face with sweat",
    "Star", "Check mark button", "Rainbow", "Smiling face with horns",
    "Sign of the horns", "Sweat droplets", "Check mark", "Persevering face",
]

def fetch(path):
    url = RAW + urllib.parse.quote(path)
    try:
        with urllib.request.urlopen(url, timeout=30) as r:
            return r.read()
    except Exception:
        return None

def slug_variants(cldr):
    s = cldr.strip().lower()
    out = []
    for v in (s.replace(" ", "_"),
              s.replace(" ", "_").replace("-", "_"),
              s.replace(" ", "_").replace("-", "")):
        if v not in out:
            out.append(v)
    return out

def find_png(folder, cldr):
    for s in slug_variants(cldr):
        for path in (f"{folder}/3D/{s}_3d.png",
                     f"{folder}/Default/3D/{s}_3d_default.png"):
            data = fetch(path)
            if data:
                return data
    return None

def to_rgb565a8(img):
    img = img.convert("RGBA").resize((EMOJI_PX, EMOJI_PX), Image.LANCZOS)
    px = list(img.getdata())
    color = bytearray()
    alpha = bytearray()
    for (r, g, b, a) in px:
        v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        color += bytes((v & 0xFF, (v >> 8) & 0xFF))   # little-endian RGB565
        alpha.append(a)
    return bytes(color) + bytes(alpha)               # color plane then alpha plane

def main():
    seen_cp, entries, total = set(), [], 0
    for folder in FOLDERS:
        if len(entries) >= MAX_EMOJI:
            break
        meta = fetch(f"{folder}/metadata.json")
        if not meta:
            print(f"  skip (no metadata): {folder}"); continue
        m = json.loads(meta)
        cps = [int(x, 16) for x in m["unicode"].split()]
        cp = cps[0]
        cldr = m.get("cldr", folder.lower())
        if cp < 0x2000:                # never shadow ASCII / Latin Hanken glyphs
            print(f"  skip (base cp too low U+{cp:04X}): {folder}"); continue
        if cp in seen_cp:
            continue
        png = find_png(folder, cldr)
        if not png:
            print(f"  skip (no 3D png): {folder}"); continue
        try:
            data = to_rgb565a8(Image.open(io.BytesIO(png)))
        except Exception as e:
            print(f"  skip (decode {e}): {folder}"); continue
        seen_cp.add(cp)
        entries.append((cp, cldr, data))
        total += len(data)
        print(f"  U+{cp:05X}  {folder}  ({len(data)} B)")

    entries.sort(key=lambda e: e[0])
    guard = "map"
    lines = ['#if defined(LV_LVGL_H_INCLUDE_SIMPLE)', '#include "lvgl.h"',
             '#else', '#include <lvgl.h>', '#endif',
             '#include "assets/emoji_assets.h"', '',
             '#ifndef LV_ATTRIBUTE_MEM_ALIGN', '#define LV_ATTRIBUTE_MEM_ALIGN', '#endif', '']
    for cp, cldr, data in entries:
        sym = f"emoji_{cp:05x}"
        body = ",".join(str(b) for b in data)
        lines.append(f"// U+{cp:05X} {cldr}")
        lines.append(f"static const LV_ATTRIBUTE_MEM_ALIGN uint8_t {sym}_map[] = {{{body}}};")
        lines.append(f"static const lv_image_dsc_t {sym} = {{")
        lines.append(f"  .header = {{ .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565A8,")
        lines.append(f"    .flags = 0, .w = {EMOJI_PX}, .h = {EMOJI_PX}, .stride = {EMOJI_PX*2}, .reserved_2 = 0 }},")
        lines.append(f"  .data_size = sizeof({sym}_map), .data = {sym}_map, .reserved = NULL,")
        lines.append("};")
        lines.append("")
    lines.append("const ori_emoji_entry_t g_ori_emoji[] = {")
    for cp, cldr, _ in entries:
        lines.append(f"  {{ 0x{cp:05X}, &emoji_{cp:05x} }},")
    lines.append("};")
    lines.append(f"const uint32_t g_ori_emoji_count = {len(entries)};")
    with open(OUT_C, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")

    with open(OUT_H, "w", encoding="utf-8") as f:
        f.write("#pragma once\n#include <lvgl.h>\n#include <stdint.h>\n\n"
                "// Codepoint -> compiled-in Fluent emoji image (sorted by cp).\n"
                "typedef struct { uint32_t cp; const lv_image_dsc_t* img; } ori_emoji_entry_t;\n"
                "extern const ori_emoji_entry_t g_ori_emoji[];\n"
                "extern const uint32_t g_ori_emoji_count;\n")

    print(f"\n{len(entries)} emoji, {total/1024:.1f} KB pixel data -> {OUT_C}")

if __name__ == "__main__":
    main()
