#include "screens/modal_profile.h"

#include <lvgl.h>

#include "photo_cache.h"
#include "theme.h"
#include "ui_helpers.h"
#include "widgets/widget_profile_card.h"

// ── Hand-drawn detail icons ──────────────────────────────────────────────────
// The UI fonts (Hanken Grotesk) carry no symbol glyphs and every Montserrat /
// FontAwesome built-in is disabled (lv_conf.h), so icons are composed from
// LVGL primitives — the same approach as make_cal_glyph()/add_close_x(). Each
// glyph is centred inside a 44 px rounded "tile".
namespace {

constexpr lv_coord_t TILE_SZ = 44;

// Filled decorative shape (non-interactive).
lv_obj_t* shape(lv_obj_t* parent, lv_coord_t w, lv_coord_t h,
                uint32_t col, lv_coord_t radius) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, theme::color(col), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

// Hollow (outlined) rounded rect (non-interactive).
lv_obj_t* outline(lv_obj_t* parent, lv_coord_t w, lv_coord_t h,
                  uint32_t col, lv_coord_t bw, lv_coord_t radius) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(o, theme::color(col), 0);
    lv_obj_set_style_border_width(o, bw, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

lv_obj_t* make_icon_tile(lv_obj_t* parent) {
    lv_obj_t* tile = lv_obj_create(parent);
    lv_obj_set_size(tile, TILE_SZ, TILE_SZ);
    lv_obj_set_style_radius(tile, 12, 0);
    lv_obj_set_style_bg_color(tile, theme::color(theme::COLOR_ELEV), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tile, 0, 0);
    lv_obj_set_style_pad_all(tile, 0, 0);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    return tile;
}

// Person/avatar: head circle over a rounded "shoulders" bar.
void draw_person(lv_obj_t* tile, uint32_t col) {
    lv_obj_align(shape(tile, 13, 13, col, LV_RADIUS_CIRCLE), LV_ALIGN_CENTER, 0, -7);
    lv_obj_align(shape(tile, 24, 14, col, 7),                LV_ALIGN_CENTER, 0, 9);
}

// Envelope: outlined body + a "V" flap line.
void draw_envelope(lv_obj_t* tile, uint32_t col) {
    lv_obj_center(outline(tile, 28, 19, col, 2, 4));
    static const lv_point_precise_t flap[] = {{0, 0}, {12, 8}, {24, 0}};
    lv_obj_t* ln = lv_line_create(tile);
    lv_line_set_points(ln, flap, 3);
    lv_obj_set_style_line_width(ln, 2, 0);
    lv_obj_set_style_line_color(ln, theme::color(col), 0);
    lv_obj_set_style_line_rounded(ln, true, 0);
    lv_obj_clear_flag(ln, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(ln, LV_ALIGN_CENTER, 0, -3);
}

// Smartphone: outlined body + speaker slit + home dot.
void draw_phone(lv_obj_t* tile, uint32_t col) {
    lv_obj_center(outline(tile, 18, 28, col, 2, 5));
    lv_obj_align(shape(tile, 6, 2, col, 1),                LV_ALIGN_CENTER, 0, -9);
    lv_obj_align(shape(tile, 3, 3, col, LV_RADIUS_CIRCLE), LV_ALIGN_CENTER, 0, 9);
}

} // namespace

namespace modal_profile {

lv_obj_t* create(lv_obj_t* base_screen, lv_obj_t* ref_photo) {
    const char* p_name  = widget_profile_card::get_profile_name();
    if (!p_name  || !p_name[0])  p_name  = "No name";
    const char* p_title = widget_profile_card::get_profile_title();
    if (!p_title || !p_title[0]) p_title = "No position";
    const char* p_email = widget_profile_card::get_profile_email();
    const char* p_phone = widget_profile_card::get_profile_phone();

    widget_profile_card::Presence pres = widget_profile_card::get_default_presence();
    uint32_t pres_color;
    switch (pres) {
        case widget_profile_card::Presence::Available: pres_color = theme::COLOR_PRESENCE_AVAILABLE; break;
        case widget_profile_card::Presence::Busy:       pres_color = theme::COLOR_PRESENCE_BUSY;      break;
        case widget_profile_card::Presence::Away:       pres_color = theme::COLOR_PRESENCE_AWAY;      break;
        default:                                        pres_color = theme::COLOR_PRESENCE_OFFLINE;   break;
    }

    // Independent screen — pure black background, no status bar.
    // Tap the profile photo to return to base_screen.
    lv_obj_t* screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen, theme::color(0x000000), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    // Outer box — full 800×480 screen, no padding (this is a standalone screen
    // with no status bar). body_row fills all 480 px so LV_FLEX_ALIGN_CENTER
    // in left_col centres the text block exactly at screen midpoint (y=240).
    lv_obj_t* box = lv_obj_create(screen);
    lv_obj_set_size(box, 800, 480);
    lv_obj_set_pos(box, 0, 0);
    ui::clear_container(box);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(box, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    lv_obj_t* body_row = lv_obj_create(box);
    lv_obj_set_width(body_row, lv_pct(100));
    lv_obj_set_flex_grow(body_row, 1);
    ui::clear_container(body_row);
    lv_obj_set_flex_flow(body_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(body_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    // OVERFLOW_VISIBLE so the presence ring's glow (below) isn't clipped to
    // this row's box — same reasoning as widget_profile_card.cpp's card.
    lv_obj_add_flag(body_row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    // ── Left column ───────────────────────────────────────────────────────────
    // Explicit height = full screen height so LVGL doesn't need to resolve it
    // through two layers of flex growth. LV_FLEX_ALIGN_CENTER on the main axis
    // centres the content block at y=240 (screen midpoint).
    lv_obj_t* left_col = lv_obj_create(body_row);
    lv_obj_set_flex_grow(left_col, 1);
    lv_obj_set_height(left_col, 480);
    lv_obj_set_style_bg_opa(left_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(left_col, 0, 0);
    lv_obj_set_style_pad_left(left_col, 40, 0);
    lv_obj_set_style_pad_right(left_col, 20, 0);
    lv_obj_set_style_pad_top(left_col, 0, 0);
    lv_obj_set_style_pad_bottom(left_col, 0, 0);
    lv_obj_clear_flag(left_col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(left_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(left_col, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // ── Name — the headline ───────────────────────────────────────────────────
    lv_obj_t* name_lbl = lv_label_create(left_col);
    lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(name_lbl, p_name);
    lv_obj_set_width(name_lbl, lv_pct(100));
    lv_obj_set_style_text_font(name_lbl, theme::font_time(), 0);  // 30px (font_h2 28 + 2)
    lv_obj_set_style_text_color(name_lbl, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_align(name_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_bottom(name_lbl, 16, 0);

    // Short accent divider under the name.
    shape(left_col, 132, 2, theme::COLOR_ACCENT_LINE, 1);

    // ── Detail rows — icon tile + label ───────────────────────────────────────
    lv_obj_t* info = lv_obj_create(left_col);
    lv_obj_set_width(info, 360);
    lv_obj_set_height(info, LV_SIZE_CONTENT);
    ui::clear_container(info);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(info, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(info, 14, 0);
    lv_obj_set_style_pad_top(info, 22, 0);

    // Build a row "[icon tile] [label]" and return the (empty, styled) label.
    // `draw` paints the icon into the tile; pass nullptr to leave it for the
    // caller (the status row paints a presence dot itself). `out_tile` returns
    // the tile when the caller needs it.
    auto add_row = [&](void (*draw)(lv_obj_t*, uint32_t), uint32_t icon_col,
                       uint32_t text_col, lv_obj_t** out_tile) -> lv_obj_t* {
        lv_obj_t* row = lv_obj_create(info);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        ui::clear_container(row);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 16, 0);

        lv_obj_t* tile = make_icon_tile(row);
        if (draw) draw(tile, icon_col);
        if (out_tile) *out_tile = tile;

        lv_obj_t* lbl = lv_label_create(row);
        lv_obj_set_flex_grow(lbl, 1);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(lbl, theme::font_title(), 0);  // 26px (font_meta 24 + 2)
        lv_obj_set_style_text_color(lbl, theme::color(text_col), 0);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_LEFT, 0);
        return lbl;
    };

    // Job title — person icon.
    lv_obj_t* title_lbl = add_row(draw_person, theme::COLOR_ACCENT,
                                  theme::COLOR_TEXT_SECONDARY, nullptr);
    lv_label_set_text(title_lbl, p_title);

    // Status — presence dot (colour + glow track presence; registered for live
    // updates so a Presence Status write recolours it while the modal is open).
    lv_obj_t* status_tile = nullptr;
    lv_obj_t* status_lbl  = add_row(nullptr, 0, pres_color, &status_tile);
    lv_obj_t* status_dot  = shape(status_tile, 18, 18, pres_color, LV_RADIUS_CIRCLE);
    lv_obj_center(status_dot);
    lv_obj_set_style_shadow_width(status_dot, 14, 0);
    lv_obj_set_style_shadow_spread(status_dot, 2, 0);
    lv_obj_set_style_shadow_color(status_dot, theme::color(pres_color), 0);
    // Offline gets no glow — matches the photo ring's offline treatment.
    lv_obj_set_style_shadow_opa(status_dot,
        pres == widget_profile_card::Presence::Offline ? LV_OPA_TRANSP : LV_OPA_50, 0);
    {
        const char* status_str;
        switch (pres) {
            case widget_profile_card::Presence::Available: status_str = "Available"; break;
            case widget_profile_card::Presence::Busy:      status_str = "Busy";      break;
            case widget_profile_card::Presence::Away:      status_str = "Away";       break;
            default:                                       status_str = "Offline";    break;
        }
        lv_label_set_text(status_lbl, status_str);
    }

    // Email — envelope icon.
    lv_obj_t* email_lbl = add_row(draw_envelope, theme::COLOR_ACCENT,
                                  theme::COLOR_TEXT_SECONDARY, nullptr);
    lv_label_set_text(email_lbl, (p_email && p_email[0]) ? p_email : "No email");

    // Phone — smartphone icon.
    lv_obj_t* phone_lbl = add_row(draw_phone, theme::COLOR_ACCENT,
                                  theme::COLOR_TEXT_SECONDARY, nullptr);
    lv_label_set_text(phone_lbl, (p_phone && p_phone[0]) ? p_phone : "No phone number");

    // ── Right column — photo pinned to the same absolute Y as in calendar mode ─
    // body_row now starts at y=0 (box has no pad_top), so photo_pad_top = coords.y1
    // places the ring at the same absolute screen position as the live card.
    lv_coord_t photo_pad_top = 0;
    if (ref_photo) {
        lv_area_t coords;
        lv_obj_get_coords(ref_photo, &coords);
        if (coords.y1 > 0) photo_pad_top = coords.y1;
    }

    lv_obj_t* right_col = lv_obj_create(body_row);
    lv_obj_set_size(right_col, widget_profile_card::WIDTH, lv_pct(100));
    ui::clear_container(right_col);
    lv_obj_set_style_pad_left(right_col, 11, 0);
    lv_obj_set_style_pad_right(right_col, 5, 0);
    lv_obj_set_style_pad_top(right_col, photo_pad_top, 0);
    lv_obj_set_style_pad_bottom(right_col, 0, 0);
    lv_obj_set_flex_flow(right_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(right_col, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    // OVERFLOW_VISIBLE so the presence ring's glow isn't clipped to this
    // column's box — same reasoning as widget_profile_card.cpp's card.
    lv_obj_add_flag(right_col, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    lv_obj_t* photo_ring = lv_obj_create(right_col);
    lv_obj_set_size(photo_ring,
        widget_profile_card::PHOTO_SIZE + 12,
        widget_profile_card::PHOTO_SIZE + 12);
    lv_obj_set_style_radius(photo_ring, LV_RADIUS_CIRCLE, 0);
    // Solid flat fill (not a gradient) — see widget_profile_card.cpp's ring
    // comment for why: a directional gradient here would make the glow below
    // read as growing bottom-to-top instead of evenly outward from the photo.
    lv_obj_set_style_bg_color(photo_ring, theme::color(pres_color), 0);
    lv_obj_set_style_bg_opa(photo_ring, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(photo_ring, 0, 0);
    lv_obj_set_style_pad_all(photo_ring, 0, 0);
    // Static presence glow — matches widget_profile_card.cpp's ring, no anim.
    lv_obj_set_style_shadow_color(photo_ring, theme::color(pres_color), 0);
    lv_obj_set_style_shadow_width(photo_ring, 40, 0);
    lv_obj_set_style_shadow_spread(photo_ring, 8, 0);
    // Offline gets no glow — see widget_profile_card.cpp's shadow_opa_for_presence().
    lv_obj_set_style_shadow_opa(photo_ring,
        pres == widget_profile_card::Presence::Offline ? LV_OPA_TRANSP : LV_OPA_50, 0);
    lv_obj_set_style_shadow_ofs_x(photo_ring, 0, 0);
    lv_obj_set_style_shadow_ofs_y(photo_ring, 0, 0);
    lv_obj_clear_flag(photo_ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(photo_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(photo_ring, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_add_event_cb(photo_ring, [](lv_event_t* e) {
        lv_obj_t* prev = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
        lv_scr_load_anim(prev, LV_SCR_LOAD_ANIM_NONE, 0, 0, /*auto_del=*/true);
    }, LV_EVENT_CLICKED, base_screen);
    widget_profile_card::register_modal_photo(photo_ring);

    lv_obj_t* photo = lv_obj_create(photo_ring);
    lv_obj_set_size(photo, widget_profile_card::PHOTO_SIZE, widget_profile_card::PHOTO_SIZE);
    lv_obj_center(photo);
    lv_obj_set_style_radius(photo, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(photo, theme::color(0x2A3140), 0);
    lv_obj_set_style_bg_grad_color(photo, theme::color(0x1A1F28), 0);
    lv_obj_set_style_bg_grad_dir(photo, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(photo, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(photo, 0, 0);
    lv_obj_set_style_pad_all(photo, 0, 0);
    lv_obj_clear_flag(photo, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(photo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_clip_corner(photo, true, 0);

    widget_profile_card::ModalLabels modal_labels = {};
    modal_labels.status_lbl = status_lbl;
    modal_labels.name_lbl   = name_lbl;
    modal_labels.title_lbl  = title_lbl;
    modal_labels.email_lbl  = email_lbl;
    modal_labels.phone_lbl  = phone_lbl;
    modal_labels.status_dot = status_dot;
    widget_profile_card::register_modal_labels(modal_labels);

    // Always create the image (even with no photo yet) and register it so
    // set_photo() can update it live when a photo arrives over BLE while the
    // modal is open. Falls back to the placeholder, matching the base card.
    lv_obj_t* img = lv_image_create(photo);
    lv_obj_set_size(img, widget_profile_card::PHOTO_SIZE, widget_profile_card::PHOTO_SIZE);
    lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);
    const lv_image_dsc_t* photo_dsc = photo_cache::get();
    if (!photo_dsc) photo_dsc = photo_cache::get_profile_placeholder();
    if (photo_dsc) {
        lv_image_set_src(img, photo_dsc);
    } else {
        lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    }
    widget_profile_card::register_modal_photo_img(img);

    lv_obj_add_event_cb(screen, [](lv_event_t*) {
        widget_profile_card::unregister_modal_photo();
        widget_profile_card::unregister_modal_photo_img();
        widget_profile_card::unregister_modal_labels();
    }, LV_EVENT_DELETE, nullptr);

    // Load as independent screen; keep base_screen alive for the go-back tap.
    lv_scr_load_anim(screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, /*auto_del=*/false);

    return screen;
}

} // namespace modal_profile
