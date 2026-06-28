#include "ui_helpers.h"

#include <stddef.h>
#include <stdint.h>

#include "theme.h"
#include "widgets/widget_status_bar.h"


namespace ui {

namespace {

// Decode one UTF-8 codepoint at `s`. Sets *adv to its byte length (1..4).
// Invalid lead/continuation bytes → adv=1, returns 0 (caller drops it).
static uint32_t utf8_next(const uint8_t* s, size_t* adv) {
    uint8_t c = s[0];
    if (c < 0x80) { *adv = 1; return c; }
    if ((c >> 5) == 0x6 && (s[1] & 0xC0) == 0x80) {
        *adv = 2; return ((uint32_t)(c & 0x1F) << 6) | (s[1] & 0x3F);
    }
    if ((c >> 4) == 0xE && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        *adv = 3; return ((uint32_t)(c & 0x0F) << 12) |
                         ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    }
    if ((c >> 3) == 0x1E && (s[1] & 0xC0) == 0x80 &&
        (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        *adv = 4; return ((uint32_t)(c & 0x07) << 18) |
                         ((uint32_t)(s[1] & 0x3F) << 12) |
                         ((uint32_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
    }
    *adv = 1; return 0;  // invalid byte
}

// True if the UI font has a glyph for this codepoint. ASCII printables are
// always present in the Hanken subset; for the rest, ask the font directly.
static bool font_has_glyph(uint32_t cp) {
    if (cp >= 0x20 && cp < 0x7F) return true;
    lv_font_glyph_dsc_t dsc;
    return lv_font_get_glyph_dsc(theme::font_meta(), &dsc, cp, 0);
}

} // namespace

const char* sanitize_text(const char* in, char* out, size_t out_sz) {
    if (!out || out_sz == 0) return out;
    if (!in) { out[0] = '\0'; return out; }

    const uint8_t* s = reinterpret_cast<const uint8_t*>(in);
    size_t o = 0;
    bool pending_space = false;   // a space we'll emit only if a glyph follows
    bool at_line_start = true;    // start of string or just after a newline

    while (*s) {
        size_t adv = 1;
        uint32_t cp = utf8_next(s, &adv);

        if (cp == '\n') {
            pending_space = false;             // trim trailing space before break
            if (o < out_sz - 1) out[o++] = '\n';
            at_line_start = true;
            s += adv;
            continue;
        }
        if (cp == ' ' || cp == '\t') {
            if (!at_line_start) pending_space = true;  // collapse + trim leading
            s += adv;
            continue;
        }
        // Printable candidate — drop if the font can't render it.
        if (cp == 0 || !font_has_glyph(cp)) { s += adv; continue; }

        if (pending_space && o < out_sz - 1) out[o++] = ' ';
        pending_space = false;
        at_line_start = false;
        for (size_t k = 0; k < adv && o < out_sz - 1; ++k) out[o++] = s[k];
        s += adv;
    }

    out[o] = '\0';
    return out;
}

lv_obj_t* make_screen_body(lv_obj_t* screen) {
    lv_obj_t* body = lv_obj_create(screen);
    lv_obj_set_size(body, 800, 480 - widget_status_bar::HEIGHT);
    lv_obj_set_pos(body, 0, widget_status_bar::HEIGHT);
    lv_obj_set_style_bg_color(body, theme::color(theme::COLOR_BG), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    // LVGL's default pad_column on lv_obj_create() is non-zero, which
    // adds ~11 px between panels and shoves the profile card off screen.
    lv_obj_set_style_pad_row(body, 0, 0);
    lv_obj_set_style_pad_column(body, 0, 0);
    return body;
}

lv_obj_t* make_panel_divider(lv_obj_t* parent) {
    lv_obj_t* div = lv_obj_create(parent);
    lv_obj_set_size(div, 5, lv_pct(100));
    lv_obj_set_style_bg_color(div, theme::color(theme::COLOR_DIVIDER), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(div, 0, 0);
    lv_obj_set_style_pad_all(div, 0, 0);
    lv_obj_clear_flag(div, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(div, LV_OBJ_FLAG_CLICKABLE);
    return div;
}

ModalLayout make_modal_layout(lv_obj_t* base_screen, lv_coord_t card_w, lv_coord_t card_h) {
    ModalLayout layout{};

    layout.scrim = lv_obj_create(base_screen);
    lv_obj_set_size(layout.scrim, 800, 480);
    lv_obj_set_pos(layout.scrim, 0, 0);
    lv_obj_set_style_bg_color(layout.scrim, theme::color(theme::COLOR_SCRIM), 0);
    lv_obj_set_style_bg_opa(layout.scrim, theme::SCRIM_OPA, 0);
    lv_obj_set_style_border_width(layout.scrim, 0, 0);
    lv_obj_set_style_pad_all(layout.scrim, 0, 0);
    lv_obj_clear_flag(layout.scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(layout.scrim, LV_OBJ_FLAG_CLICKABLE);

    layout.card = lv_obj_create(layout.scrim);
    lv_obj_set_size(layout.card, card_w, card_h);
    lv_obj_center(layout.card);
    lv_obj_set_style_bg_color(layout.card, theme::color(theme::COLOR_CARD), 0);
    lv_obj_set_style_bg_opa(layout.card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(layout.card, theme::color(theme::COLOR_DIVIDER_STRONG), 0);
    lv_obj_set_style_border_width(layout.card, 1, 0);
    lv_obj_set_style_radius(layout.card, 18, 0);
    lv_obj_set_style_shadow_color(layout.card, theme::color(0x000000), 0);
    lv_obj_set_style_shadow_width(layout.card, 30, 0);
    lv_obj_set_style_shadow_opa(layout.card, LV_OPA_70, 0);
    lv_obj_set_style_pad_left(layout.card, 32, 0);
    lv_obj_set_style_pad_right(layout.card, 32, 0);
    lv_obj_set_style_pad_top(layout.card, 32, 0);
    lv_obj_set_style_pad_bottom(layout.card, 32, 0);
    lv_obj_clear_flag(layout.card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(layout.card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(layout.card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    layout.scroll_area = lv_obj_create(layout.card);
    lv_obj_set_width(layout.scroll_area, lv_pct(100));
    lv_obj_set_flex_grow(layout.scroll_area, 1);
    lv_obj_set_style_bg_opa(layout.scroll_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(layout.scroll_area, 0, 0);
    lv_obj_set_style_pad_all(layout.scroll_area, 0, 0);
    lv_obj_add_flag(layout.scroll_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(layout.scroll_area, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scroll_dir(layout.scroll_area, LV_DIR_VER);
    lv_obj_set_flex_flow(layout.scroll_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(layout.scroll_area, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    layout.actions = lv_obj_create(layout.card);
    lv_obj_set_size(layout.actions, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(layout.actions, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(layout.actions, 0, 0);
    // actions is LV_SIZE_CONTENT, so its own padding sets the box size the
    // buttons get clipped to. Each button's breathing glow extends
    // shadow_width/2 + shadow_spread = font_h/2 + font_h/4 beyond its border
    // box (make_btn) — for font_meta (line_height 32) that's 24 px. 8 px of
    // padding clipped the outer buttons' glow on the left/right; 26 px gives
    // it room on every side. LV_OBJ_FLAG_OVERFLOW_VISIBLE is kept as a
    // backstop (same fix as the brand-mark glow bleed in screen_setup.cpp)
    // but the padding is what actually guarantees the box is big enough.
    lv_obj_set_style_pad_left(layout.actions, 26, 0);
    lv_obj_set_style_pad_right(layout.actions, 26, 0);
    lv_obj_set_style_pad_bottom(layout.actions, 26, 0);
    lv_obj_set_style_pad_top(layout.actions, 26, 0);
    lv_obj_set_style_pad_column(layout.actions, 16, 0);
    lv_obj_clear_flag(layout.actions, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(layout.actions, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(layout.actions, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_flex_flow(layout.actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(layout.actions, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    return layout;
}

lv_obj_t* add_close_x(lv_obj_t* card, lv_event_cb_t cb, void* user) {
    lv_obj_t* btn = lv_obj_create(card);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_IGNORE_LAYOUT);   // not part of the card's flex flow
    lv_obj_set_size(btn, 52, 52);                      // 40 px +30%
    lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, 20, -20);    // nudged toward the card corner
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, theme::color(theme::COLOR_ELEV), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(btn, LV_OPA_60, LV_STATE_PRESSED);

    // "X" from two crossing lines — no dependency on a symbol font glyph.
    // Glyph + stroke scaled with the button (14 px → 18 px, 3 px → 4 px).
    static const lv_point_precise_t seg_a[] = {{0, 0}, {18, 18}};
    static const lv_point_precise_t seg_b[] = {{0, 18}, {18, 0}};
    const lv_point_precise_t* segs[2] = { seg_a, seg_b };
    for (int i = 0; i < 2; ++i) {
        lv_obj_t* ln = lv_line_create(btn);
        lv_line_set_points(ln, segs[i], 2);
        lv_obj_center(ln);
        lv_obj_set_style_line_width(ln, 4, 0);
        lv_obj_set_style_line_color(ln, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
        lv_obj_set_style_line_rounded(ln, true, 0);
        lv_obj_clear_flag(ln, LV_OBJ_FLAG_CLICKABLE);
    }

    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user);
    return btn;
}

static void btn_glow_anim_cb(void* obj, int32_t v) {
    lv_obj_set_style_shadow_opa(static_cast<lv_obj_t*>(obj), (lv_opa_t)v, 0);
}

lv_obj_t* make_btn(lv_obj_t* parent, const char* text,
                   BtnStyle style,
                   lv_event_cb_t cb, void* user,
                   int16_t pad_v, int16_t pad_h,
                   const lv_font_t* font) {
    const lv_font_t* active_font = font ? font : theme::font_meta();
    const lv_coord_t font_h = lv_font_get_line_height(active_font);
    const lv_coord_t glow_w = font_h;
    const lv_coord_t glow_spread = font_h/4;

    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_set_size(btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_left(btn, pad_h, 0);
    lv_obj_set_style_pad_right(btn, pad_h, 0);
    lv_obj_set_style_pad_top(btn, pad_v, 0);
    lv_obj_set_style_pad_bottom(btn, pad_v, 0);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);

    // Shared ghost-button emphasis so every button inherits a visible outline.
    lv_obj_set_style_border_width(btn, glow_spread/4, 0);
    lv_obj_set_style_shadow_width(btn, glow_w, 0);
    lv_obj_set_style_shadow_spread(btn, glow_spread, 0);
    // Danger glow profile is the baseline for non-tertiary buttons.
    lv_obj_set_style_shadow_opa(btn, LV_OPA_40, 0);
    lv_obj_set_style_shadow_ofs_x(btn, 0, 0);
    lv_obj_set_style_shadow_ofs_y(btn, 0, 0);
    lv_obj_set_style_opa(btn, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_set_style_border_opa(btn, LV_OPA_90, 0);
    lv_color_t text_color = theme::color(theme::COLOR_TEXT_SECONDARY);

    switch (style) {
        case BtnStyle::Primary:
            text_color = theme::color(theme::COLOR_ACCENT);
            break;

        case BtnStyle::Danger:
            text_color = theme::color(theme::COLOR_DANGER);
            break;

        case BtnStyle::Tertiary:
            // Tertiary has no glow.
            lv_obj_set_style_shadow_width(btn, 0, 0);
            lv_obj_set_style_shadow_spread(btn, 0, 0);
            break;
    }

    lv_obj_set_style_border_color(btn, text_color, 0);
    lv_obj_set_style_shadow_color(btn, text_color, 0);

    // Breathing glow for all styles except Tertiary — pulses shadow_opa via
    // lv_anim (not LVGL style transitions, which cause tearing on the
    // bounce-buffer display). lv_anim fires inside lv_task_handler() so one
    // invalidation per flush; no horizontal tear bands.
    if (style != BtnStyle::Tertiary) {
        const lv_opa_t glow_min = LV_OPA_40;
        const lv_opa_t glow_max = LV_OPA_80;
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, btn);
        lv_anim_set_exec_cb(&a, btn_glow_anim_cb);
        lv_anim_set_values(&a, (int32_t)glow_min, (int32_t)glow_max);
        lv_anim_set_time(&a, 1800);
        lv_anim_set_playback_time(&a, 1800);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_start(&a);
        lv_obj_add_event_cb(btn, [](lv_event_t* e) {
            lv_anim_delete(lv_event_get_current_target(e), btn_glow_anim_cb);
        }, LV_EVENT_DELETE, nullptr);
    }

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, text_color, 0);
    lv_obj_set_style_text_font(lbl, active_font, 0);
    if (style == BtnStyle::Primary) {
        lv_obj_set_style_text_letter_space(lbl, 4, 0);
    }
    lv_obj_center(lbl);

    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user);
    }
    return btn;
}

} // namespace ui
