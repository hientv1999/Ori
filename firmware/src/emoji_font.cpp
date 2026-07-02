#include "emoji_font.h"

#include "assets/emoji_assets.h"

// Height (px) the emoji images were baked at — keep in sync with EMOJI_PX in
// img/emoji/convert_emoji.py.
static constexpr uint16_t EMOJI_PX = 28;

// Small downward nudge so the glyph box sits closer to the text baseline rather
// than riding high above it. Tuned by eye against font_meta (24 px); adjust if
// emoji sit too high/low next to text.
static constexpr int32_t EMOJI_OFFSET_Y = 2;

namespace {

// g_ori_emoji is sorted by codepoint (see convert_emoji.py) → binary search.
const lv_image_dsc_t* lookup(uint32_t cp) {
    int lo = 0, hi = (int)g_ori_emoji_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t c = g_ori_emoji[mid].cp;
        if (c == cp) return g_ori_emoji[mid].img;
        if (c < cp)  lo = mid + 1;
        else         hi = mid - 1;
    }
    return nullptr;
}

const void* emoji_path_cb(const lv_font_t* /*font*/, uint32_t unicode,
                          uint32_t /*unicode_next*/, int32_t* offset_y, void* /*ud*/) {
    const lv_image_dsc_t* d = lookup(unicode);
    if (!d) return nullptr;                 // not in our set → let LVGL drop it
    if (offset_y) *offset_y = EMOJI_OFFSET_Y;
    return d;                               // imgfont accepts an lv_image_dsc_t* as the src
}

lv_font_t* g_emoji_font = nullptr;

}  // namespace

namespace emoji_font {

const lv_font_t* get() {
    if (!g_emoji_font && g_ori_emoji_count > 0) {
        g_emoji_font = lv_imgfont_create(EMOJI_PX, emoji_path_cb, nullptr);
    }
    return g_emoji_font;
}

}  // namespace emoji_font
