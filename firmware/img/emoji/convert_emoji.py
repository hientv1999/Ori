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
MAX_EMOJI  = 250                     # hard cap (flash budget)
RAW        = "https://raw.githubusercontent.com/microsoft/fluentui-emoji/main/assets/"

HERE       = os.path.dirname(os.path.abspath(__file__))
FW         = os.path.abspath(os.path.join(HERE, "..", ".."))
OUT_C      = os.path.join(FW, "src", "assets", "emoji_assets.c")
OUT_H      = os.path.join(FW, "include", "assets", "emoji_assets.h")

# Curated folders (Fluent CLDR names). Over-listed slightly; capped at MAX_EMOJI.
FOLDERS = [
    # hearts / love
    "Red heart","Orange heart","Yellow heart","Green heart","Blue heart",
    "Purple heart","Black heart","Broken heart","Two hearts","Sparkling heart",
    "Growing heart","Beating heart","Revolving hearts","Heart with arrow",
    # hands / gestures
    "Thumbs up","Thumbs down","Clapping hands","Raising hands","Folded hands",
    "Waving hand","OK hand","Victory hand","Crossed fingers","Flexed biceps",
    "Handshake","Raised fist","Call me hand","Love-you gesture",
    "Backhand index pointing right","Backhand index pointing left",
    # smileys
    "Grinning face","Grinning face with big eyes","Beaming face with smiling eyes",
    "Grinning squinting face","Face with tears of joy","Rolling on the floor laughing",
    "Slightly smiling face","Smiling face with smiling eyes",
    "Smiling face with heart-eyes","Smiling face with sunglasses","Star-struck",
    "Face blowing a kiss","Winking face","Face savoring food","Thinking face",
    "Zany face","Face with raised eyebrow","Neutral face","Unamused face",
    "Face with rolling eyes","Smirking face","Pensive face","Confused face",
    "Worried face","Crying face","Loudly crying face","Face screaming in fear",
    "Angry face","Pouting face","Exploding head","Hot face","Cold face",
    "Flushed face","Pleading face","Sleeping face","Face with medical mask",
    "Nauseated face","Woozy face","Partying face","Smiling face with halo",
    "Cowboy hat face","Shushing face","Face with hand over mouth","Grimacing face",
    "Winking face with tongue","Zipper-mouth face","Sleepy face","Yawning face",
    # objects / symbols
    "Fire","Party popper","Confetti ball","Sparkles","Star","Glowing star",
    "Hundred points","Collision","Balloon","Birthday cake","Wrapped gift",
    "Trophy","Crown","Rocket","Check mark button","Cross mark","Warning",
    "Red exclamation mark","Red question mark","Eyes","Skull","Ghost","Robot",
    "Alien","Pile of poo","Clinking beer mugs","Clinking glasses","Hot beverage",
    "Musical notes","High voltage","Sun","Rainbow","Snowflake","Light bulb",
    "Bell","Locked","Key","Money bag","Dollar banknote","Bomb","Alarm clock",
    "Hourglass done",
    # --- extended set (net ~150 total) ---
    # more smileys
    "Smiling face","Kissing face with closed eyes","Face with tongue",
    "Hugging face","Nerd face","Smiling face with horns","Clown face","Lying face",
    "Sneezing face","Face vomiting","Astonished face","Fearful face","Weary face",
    "Tired face","Disappointed face","Downcast face with sweat",
    "Anxious face with sweat","Face with steam from nose","Relieved face",
    "Persevering face","Money-mouth face","Saluting face","Smiling face with tear",
    # more hands / body
    "Raised back of hand","Vulcan salute","Sign of the horns","Index pointing up",
    "Writing hand","Brain","Eye","Pinched fingers","Left-facing fist","Right-facing fist",
    # animals / nature / plants
    "Dog face","Cat face","Unicorn","Butterfly","Four leaf clover","Rose",
    "Bouquet","Sunflower","Cherry blossom","Christmas tree","Maple leaf","Mushroom",
    "Seedling",
    # weather
    "Sun behind cloud","Cloud with rain","Snowman","Droplet","Tornado","Comet",
    # food / drink
    "Pizza","Hamburger","Doughnut","Cookie","Ice cream","Red apple","Banana",
    "Watermelon","Strawberry","Grapes","Peach","Avocado","Wine glass",
    "Cocktail glass","Beer mug","Bottle with popping cork","Hot beverage",
    # activities / objects
    "Soccer ball","Basketball","Video game","Game die","Direct hit","Guitar",
    "Microphone","Camera","Movie camera","Clapper board","Books","Open book",
    "Memo","Pencil","Pushpin","Round pushpin","Paperclip","Scissors","Envelope",
    "Package","Calendar","Watch","Stopwatch","Mobile phone","Laptop","Battery",
    "Flashlight","Gear","Wrench","Hammer","Shield","Toolbox","Link",
    "Magnifying glass tilted left","1st place medal","Sports medal",
    # symbols / hearts
    "Speech balloon","Thought balloon","Right arrow","Play button","Repeat button",
    "No entry","Prohibited","Recycling symbol","Anger symbol","Sparkle",
    "Double exclamation mark","Exclamation question mark","Check mark",
    "Pink heart","White heart","Brown heart","Heart on fire","Mending heart",
    "Heart exclamation","Kiss mark",
    # --- second extension (net ~250 total) ---
    # animals
    "Dog","Cat","Horse face","Cow face","Pig face","Mouse face","Hamster",
    "Rabbit face","Bear","Panda","Koala","Tiger face","Lion","Frog",
    "Monkey face","See-no-evil monkey","Hear-no-evil monkey","Speak-no-evil monkey",
    "Chicken","Penguin","Bird","Baby chick","Duck","Eagle","Owl","Wolf","Fox",
    "Horse","Honeybee","Bug","Lady beetle","Ant","Spider","Turtle","Snake",
    "T-Rex","Dragon","Whale","Dolphin","Fish","Tropical fish","Shark","Octopus",
    "Snail","Crab","Paw prints",
    # food / drink
    "Green apple","Pear","Tangerine","Lemon","Mango","Pineapple","Coconut",
    "Tomato","Eggplant","Broccoli","Carrot","Ear of corn","Hot pepper",
    "Mushroom","Bread","Croissant","Pretzel","Bagel","Pancakes","Waffle",
    "Cheese wedge","Poultry leg","Cut of meat","Bacon","French fries",
    "Hot dog","Sandwich","Taco","Burrito","Egg","Green salad","Popcorn",
    "Rice ball","Cooked rice","Spaghetti","Sushi","Dumpling","Fortune cookie",
    "Shaved ice","Shortcake","Pie","Chocolate bar","Candy","Lollipop",
    "Honey pot","Milk glass","Bubble tea","Teacup without handle","Tumbler glass",
    # travel / places
    "Automobile","Taxi","Bus","Fire engine","Police car","Airplane","Rocket",
    "Bicycle","Motorcycle","Ship","Sailboat","Helicopter","Train",
    "House","Office building","Hospital","School","Mountain","Volcano",
    "Beach with umbrella","Desert island","Sunrise","National park","Tent",
    "Globe showing Europe-Africa","World map","Compass",
    # objects
    "Candle","Nut and bolt","Chains","Ribbon","Teddy bear","Kite","Crystal ball",
    "Telescope","Microscope","Test tube","Dna","Pill","Syringe","Adhesive bandage",
    "Stethoscope","Broom","Basket","Soap","Sponge","Bucket","Toothbrush",
    "Lipstick","Ring","Gem stone","Handbag","Backpack","Briefcase","Eyeglasses",
    "Sunglasses","Necktie","T-shirt","Jeans","Dress","Running shoe","Top hat",
    "Graduation cap",
    # arrows / symbols
    "Up arrow","Down arrow","Left arrow","Left-right arrow","Up-down arrow",
    "Right arrow curving left","Left arrow curving right","Counterclockwise arrows button",
    "Clockwise vertical arrows","Fast-forward button","Pause button","Stop button",
    "Record button","Next track button","Play or pause button","Shuffle tracks button",
    "Plus","Minus","Multiply","Divide","Heavy dollar sign","Trade mark",
    "Cross mark button","White question mark","White exclamation mark",
    "Peace symbol","Atom symbol","Radioactive","Biohazard","No entry",
    "Anatomical heart","Heart decoration",
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
