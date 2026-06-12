#!/usr/bin/env bash
#
# Regenerate the Hanken Grotesk LVGL font files at every size the firmware uses.
#
# WHY: the fonts are subset bitmaps baked into flash. The default build only
# included a partial range; this script regenerates them with the font's FULL
# glyph repertoire (lv_font_conv emits only glyphs the .ttf actually has, so the
# generous range below resolves to Hanken's ~550 Latin glyphs — Latin +
# Latin Extended A/B, Vietnamese, combining diacritics, punctuation, currency,
# math/arrow/geometric symbols, and fi/fl ligatures). Characters outside that
# set (emoji, CJK, Cyrillic, Arabic, …) are not in the font and cannot be added;
# they are dropped at runtime by ui::sanitize_text().
#
# REQUIRES: lv_font_conv (Node).  Install once with:  npm i -g lv_font_conv
# RUN from the firmware/ directory:  bash tools/regen_fonts.sh
#
# After running, rebuild:  pio run -e ori
set -euo pipefail

FONT="src/fonts/HankenGrotesk-Medium.ttf"
OUTDIR="src/fonts"
SIZES=(20 24 26 28 30 42 48)

# Hanken's full Latin repertoire. lv_font_conv silently skips codepoints the
# font lacks, so over-specifying the range is safe and future-proof.
RANGE="0x20-0x3FF,0x1E00-0x1EFF,0x2000-0x20CF,0x2100-0x21FF,0x2200-0x22FF,0x25A0-0x25FF,0xFB00-0xFB02"

if ! command -v lv_font_conv >/dev/null 2>&1; then
  echo "error: lv_font_conv not found. Install with: npm i -g lv_font_conv" >&2
  exit 1
fi
if [[ ! -f "$FONT" ]]; then
  echo "error: $FONT not found — run this from the firmware/ directory." >&2
  exit 1
fi

for sz in "${SIZES[@]}"; do
  out="$OUTDIR/ori_font_hanken_${sz}.c"
  echo "  generating $out (size ${sz}px, bpp 4)"
  lv_font_conv \
    --no-compress --no-prefilter --bpp 4 --size "$sz" \
    --font "$FONT" \
    -r "$RANGE" \
    --format lvgl -o "$out"
done

echo "done. Rebuild with: pio run -e ori"
