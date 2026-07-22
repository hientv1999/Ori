#pragma once

#include <lvgl.h>

// Layout helpers shared across all screen and widget files.
//
// Three patterns repeat identically throughout the codebase and are
// captured here to keep callsites readable:
//
//  ui::clear_container(obj)   — strips transparent-bg, zero-border,
//                               zero-padding, and disables scroll+click
//                               on a layout-only container object.
//
//  ui::make_screen_body(s)    — creates the 800×396 flex-row body
//                               positioned below the status bar, with
//                               COLOR_BG fill and zero row/column gaps.
//                               Used by every two-panel runtime screen.
//
//  ui::make_panel_divider(p)  — creates the 5 px × 100% vertical
//                               divider that separates the left panel
//                               from the profile card.

namespace ui {

struct ModalLayout {
    lv_obj_t* scrim;
    lv_obj_t* card;
    lv_obj_t* scroll_area;
    lv_obj_t* actions;
};

// Shared ghost-button hierarchy used across setup screens and modals.
enum class BtnStyle {
    Primary,    // accent border/text + stronger glow
    Danger,     // danger border/text + danger glow
    Tertiary,   // faint border/text, no glow
};

inline void clear_container(lv_obj_t* obj) {
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
}

lv_obj_t* make_screen_body(lv_obj_t* screen);
lv_obj_t* make_panel_divider(lv_obj_t* parent);

// Zero-size, flex_grow=1 spacer used in pairs (top + bottom) inside a
// vertical-flex scroll area to vertically centre content shorter than the
// area itself — the content sits between two equally-growing spacers.
// Shared by every full-screen detail overlay (meeting detail, Time Off
// detail, ANCS notification detail) and the alert-style confirm modals
// (factory reset, unpair phone).
lv_obj_t* make_flex_spacer(lv_obj_t* parent);

// Full-screen (800x480, pinned at 0,0) scrim: COLOR_SCRIM bg at SCRIM_OPA, no
// border/padding/radius, not scrollable. Shared by every modal/overlay that
// dims the whole screen — countdown, setup passkey/Orioning, meeting detail,
// and make_modal_layout() below.
// absorb_taps: true (default) makes the scrim clickable so taps on empty
// space don't fall through to whatever is behind it — every dismissable
// modal wants this. The non-dismissable Orioning progress overlay is the one
// exception (screen_setup.cpp) and passes false.
lv_obj_t* make_scrim(lv_obj_t* parent, bool absorb_taps = true);

// 96x96 circular alert glyph: COLOR_DANGER_SOFT fill, centered "!" in
// COLOR_DANGER at font_large. Shared by the factory-reset and unpair-phone
// confirmation modals (identical in both).
lv_obj_t* make_alert_glyph_circle(lv_obj_t* parent);

// Classic battery glyph — body outline (border rect) + nub + a live fill bar
// reading `level` (0-100), colored via battery_fill_color() below. Shared by
// modal_iphone_info's iPhone battery row and modal_device_alert's Low
// Battery Alert icon (ported out of modal_iphone_info.cpp, which kept its
// own near-identical copy before this). Returns the built glyph container
// (body+nub) — caller positions/aligns it; this makes no assumption about
// the parent's layout mode (flex row vs. a plain centered circle). `fill_out`,
// if non-null, receives the inner fill-bar object so a caller that mutates it
// live later (modal_iphone_info::set_battery_level()) doesn't have to
// re-derive it from the returned object's children.
lv_obj_t* make_battery_glyph(lv_obj_t* parent, uint8_t level, lv_obj_t** fill_out = nullptr);

// level (0-100) -> COLOR_DANGER at/below the low threshold (20%), else
// COLOR_CONN_CONNECTED — the exact rule make_battery_glyph() applies at
// build time. Exposed so a caller holding a live fill handle
// (modal_iphone_info::set_battery_level()) can recolor on a later update
// using the identical rule.
uint32_t battery_fill_color(uint8_t level);

// Ori wordmark — flanking gradient lines + "ori" label (lowercase, "r" in
// accent gold, theme::font_time()). Used on every setup-flow screen and the
// boot splash; mirrors brandMarkHTML() in the UI prototype.
lv_obj_t* make_brand_mark(lv_obj_t* parent);
ModalLayout make_modal_layout(lv_obj_t* base_screen,
                              lv_coord_t card_w = 520,
                              lv_coord_t card_h = 400);
lv_obj_t* make_btn(lv_obj_t* parent, const char* text,
                   BtnStyle style,
                   lv_event_cb_t cb = nullptr, void* user = nullptr,
                   int16_t pad_v = 14, int16_t pad_h = 28,
                   const lv_font_t* font = nullptr);

// Shared "destructive confirmation" card — alert-glyph circle, heading, body
// copy, and a Danger/Cancel button row (Danger on the left, since users
// instinctively tap the right button, so the destructive action being on the
// left reduces accidental presses). Used identically by the factory-reset
// and unpair-phone modals. Cancel always just deletes the modal's scrim;
// `danger_cb` receives the scrim as its user data (same convention), so it
// can delete it itself once the destructive action is confirmed/deferred.
// Returns the scrim.
lv_obj_t* make_confirm_modal(lv_obj_t* base_screen,
                              const char* heading, const char* body,
                              const char* danger_label, lv_event_cb_t danger_cb);

// Small circular badge overlaid on a corner of `parent` — the ANCS
// notification-count/silent indicator shape shared by the status bar, the
// iPhone Info modal, and the ANCS drill-down list. Handles only the common
// "chrome": full-circle radius, bg/border fill, scrollable/clickable off,
// and IGNORE_LAYOUT + corner alignment (so it overlays instead of
// participating in `parent`'s own layout, matching add_close_x() above).
// `border_color` is typically either the parent's own background colour (a
// "cutout" look against a card) or plain black (on a pure-black background,
// which reads the same way). Caller still sets size and adds content
// (a label or icon, centred) — those genuinely vary per use (fixed 26px
// circle vs. a wider count pill), so they're left to the caller rather than
// baked in here.
lv_obj_t* make_corner_badge(lv_obj_t* parent, lv_color_t bg_color,
                             lv_color_t border_color, int32_t border_width,
                             lv_align_t align, int16_t x_ofs = 0, int16_t y_ofs = 0);

// Circular close affordance pinned to the top-right corner of a modal `card`.
// The "X" is drawn from two crossing lines (no symbol-font dependency). Ignores
// the card's flex layout. `cb` fires on tap (typically deletes the scrim).
// Returns the button. Use instead of a bottom "Close" button on overlays.
lv_obj_t* add_close_x(lv_obj_t* card, lv_event_cb_t cb = nullptr, void* user = nullptr);

// Generic "close this modal" tap handler: deletes the lv_obj_t* passed as the
// event's user data (almost always the modal's own scrim). Register directly
// as an event callback (`lv_obj_add_event_cb(btn, ui::close_scrim_cb,
// LV_EVENT_CLICKED, scrim)`) instead of writing a one-off lambda or named
// wrapper at every plain Close/Cancel button — several modals did exactly
// that independently before this was pulled out.
void close_scrim_cb(lv_event_t* e);

// Generic lv_anim exec callbacks: set the target object's overall style
// opacity / horizontal translation from the animation's current value.
// lv_anim_set_exec_cb() requires a plain function pointer (no lambda
// captures), so every fade/slide animation used to define its OWN
// identically-bodied callback at each call site; these two cover that
// need everywhere. Sharing one function across different target objects is
// safe — lv_anim_delete(var, exec_cb) matches on the (var, exec_cb) pair, so
// animations on different `var`s never cross-cancel just because they share
// an exec_cb.
void anim_set_opa_cb(void* obj, int32_t v);
void anim_set_translate_x_cb(void* obj, int32_t v);

// Copy `in` to `out`, dropping every character the UI font (Hanken) can't
// render — emoji, CJK, and any script outside the font's Latin repertoire —
// and tidying the whitespace the drops leave behind (collapses runs of spaces,
// trims line ends; newlines preserved as LVGL line breaks). Glyph presence is
// queried against the live font, so broadening the font subset automatically
// widens what survives. Always NUL-terminates; safe if in == nullptr (→ "").
// Apply at text ingress (BLE/ANCS handlers) so stored strings are render-clean.
// Returns out.
const char* sanitize_text(const char* in, char* out, size_t out_sz);

// Uppercases an ASCII string in place (a-z -> A-Z only; bytes outside that
// range are untouched). Used to uppercase strftime()'s "%A"/"%B" weekday and
// month output — the ESP32 newlib strftime doesn't support GNU's %^A/%^B
// uppercase modifiers, so both clock faces do it by hand.
void uppercase_ascii(char* s);

} // namespace ui
