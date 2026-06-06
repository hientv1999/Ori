#include "widgets/widget_profile_card.h"

#include "mock_data.h"
#include "screens/modal_factory_reset.h"
#include "screens/modal_profile.h"
#include "theme.h"

// LVGL has no native multi-stop gradient; bg_grad_dir = VER with two colour
// stops approximates the prototype's vertical linear-gradient closely enough.

namespace {

struct CardState {
    lv_obj_t* photo;        // circular container (border / shadow / clip)
    lv_obj_t* initials_lbl; // shown when no photo
    lv_obj_t* photo_img;    // lv_image_t child, hidden when no photo
    lv_obj_t* name_label;
    lv_obj_t* title_label;
    bool suppress_click = false;
};

// Default presence applied to newly-created cards. screen_manager sets
// this before each load() so the profile photo border colour stays consistent.
widget_profile_card::Presence g_default_presence =
    widget_profile_card::Presence::Offline;

// Cached photo descriptor — set by set_photo() so new cards created after a
// photo arrives (e.g. screen transitions) start with the photo already loaded.
const lv_image_dsc_t* g_default_photo = nullptr;

// Weak reference to the most-recently-created card, used by set_photo() to
// update the live screen without a screen rebuild.  Cleared when the card is
// deleted (LV_EVENT_DELETE handler).
lv_obj_t* g_active_card = nullptr;

uint32_t color_for_presence(widget_profile_card::Presence p) {
    switch (p) {
        case widget_profile_card::Presence::Available: return theme::COLOR_PRESENCE_AVAILABLE;
        case widget_profile_card::Presence::Busy:      return theme::COLOR_PRESENCE_BUSY;
        case widget_profile_card::Presence::Away:      return theme::COLOR_PRESENCE_AWAY;
        case widget_profile_card::Presence::Offline:
        default:                                       return theme::COLOR_PRESENCE_OFFLINE;
    }
}

} // namespace

namespace widget_profile_card {

lv_obj_t* create(lv_obj_t* parent) {
    lv_obj_t* card = lv_obj_create(parent);
    // Use lv_pct(100) for height — lv_obj_get_height(parent) returns 0 when
    // called before LVGL has resolved the parent's layout, which collapses
    // the card to zero height (nothing rendered). lv_pct(100) defers to
    // the parent's content area at draw time and works correctly inside
    // the body's flex-row layout.
    lv_obj_set_size(card, WIDTH, lv_pct(100));

    // Vertical gradient: COLOR_BG (top) -> 0x0B0E13 (bottom).
    lv_obj_set_style_bg_color(card, theme::color(theme::COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(card, theme::color(0x0B0E13), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(card, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(card, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_right(card, 8, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    // LV_OBJ_FLAG_OVERFLOW_VISIBLE is intentionally NOT set here.
    // The photo's shadow (shadow_width=40, spread=8) extends ~28 px beyond the
    // photo's edge. With the flag set, the shadow bleeds left past the divider
    // into the left panel (photo left edge ≈ x=554, shadow left ≈ x=526, left
    // panel ends x=527). That 2 px overlap means LVGL re-renders the expensive
    // shadow on every scroll frame → drops from 13 fps to 5 fps with any
    // non-Offline presence. Without the flag LVGL clips the shadow to the
    // card's content area (x≥541), keeping it entirely inside the right panel.
    // Cost: the outer ~15 px of lateral blur fringe is clipped — the near-
    // transparent outer edge; the 8 px solid halo stays fully visible.

    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto* s = new CardState();
    s->initials_lbl = nullptr;
    s->photo_img    = nullptr;

    // Track the live card so set_photo() can update it without a screen rebuild.
    g_active_card = card;

    // Photo container. The border colour reflects the user's current
    // Teams presence (set by call to set_presence() once the BLE Presence
    // Status characteristic delivers a value). Initial colour is Offline
    // — same swatch as when the PC link is down — so the device never
    // renders a stale "Available" green before the first push arrives.
    s->photo = lv_obj_create(card);
    lv_obj_set_size(s->photo, PHOTO_SIZE, PHOTO_SIZE);
    lv_obj_set_style_radius(s->photo, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s->photo, theme::color(0x2A3140), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(s->photo, theme::color(0x1A1F28), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(s->photo, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s->photo, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(s->photo, theme::color(color_for_presence(g_default_presence)), LV_PART_MAIN);
    lv_obj_set_style_border_width(s->photo, 6, LV_PART_MAIN);
    // Presence glow — dramatic single-layer approximation of the CSS
    // 4-layer box-shadow. Values tuned so the halo reads clearly on hardware
    // (the IPS panel washes out subtle glows at normal viewing distance).
    //   spread=8  → wide solid halo ring tight against the border
    //   width=40  → long soft blur tail spreading outward
    //   opa=90 %  → near-opaque so the colour punches through the dark bg
    //   Offline  → no glow (CSS: box-shadow: none)
    lv_obj_set_style_shadow_color(s->photo, theme::color(color_for_presence(g_default_presence)), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s->photo, 40, LV_PART_MAIN);
    lv_obj_set_style_shadow_spread(s->photo, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(s->photo,
        g_default_presence == Presence::Offline ? LV_OPA_TRANSP : LV_OPA_90,
        LV_PART_MAIN);
    lv_obj_set_style_pad_all(s->photo, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s->photo, LV_OBJ_FLAG_SCROLLABLE);
    // Tap opens the profile detail overlay; long-press (3 s) opens factory reset.
    lv_obj_add_flag(s->photo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(s->photo, LV_OPA_60, LV_STATE_PRESSED);
    // Store CardState in user data for event handlers
    lv_obj_set_user_data(s->photo, s);
    lv_obj_add_event_cb(s->photo, [](lv_event_t* e) {
        auto* s = static_cast<CardState*>(lv_obj_get_user_data((lv_obj_t*)lv_event_get_target(e)));
        if (s && s->suppress_click) {
            s->suppress_click = false;
            return;
        }
        modal_profile::create(lv_screen_active(), (lv_obj_t*)lv_event_get_target(e));
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(s->photo, [](lv_event_t* e) {
        auto* s = static_cast<CardState*>(lv_obj_get_user_data((lv_obj_t*)lv_event_get_target(e)));
        if (s) s->suppress_click = true;
        modal_factory_reset::create(lv_screen_active());
    }, LV_EVENT_LONG_PRESSED, nullptr);

    // Enable corner clipping on the photo container so an lv_image child is
    // masked to the circle boundary.  The parent radius is already CIRCLE;
    // this flag tells LVGL to clip children against it.
    lv_obj_set_style_clip_corner(s->photo, true, LV_PART_MAIN);

    // Photo image — hidden initially; shown when a decoded JPEG is available.
    // Sized to fill the container so the circle clip masks it correctly.
    s->photo_img = lv_image_create(s->photo);
    lv_obj_set_size(s->photo_img, PHOTO_SIZE, PHOTO_SIZE);
    lv_obj_align(s->photo_img, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s->photo_img, LV_OBJ_FLAG_HIDDEN);

    // Initials, large light-weight glyphs centered in the circle.
    // Stored in CardState so set_photo() can hide/show them.
    s->initials_lbl = lv_label_create(s->photo);
    lv_label_set_text(s->initials_lbl, mock_data::profile().initials);
    lv_obj_set_style_text_color(s->initials_lbl, theme::color(theme::COLOR_ACCENT), 0);
    lv_obj_set_style_text_font(s->initials_lbl, theme::font_large(), 0);
    lv_obj_center(s->initials_lbl);

    // Apply a cached photo immediately if one was already received before
    // this card was created (e.g. boot with NVS photo, or screen transition
    // after photo arrived).
    if (g_default_photo) {
        lv_image_set_src(s->photo_img, g_default_photo);
        lv_obj_clear_flag(s->photo_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s->initials_lbl, LV_OBJ_FLAG_HIDDEN);
    }

    // Name. Uses font_time() (30 px). Single line with ellipsis per
    // screen-layout.md ("Full name — single line, ellipsis on overflow").
    // Orion enforces name ≤ 24 chars at input so truncation is rare.
    s->name_label = lv_label_create(card);
    lv_label_set_text(s->name_label, mock_data::profile().name);
    lv_label_set_long_mode(s->name_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s->name_label, WIDTH - 16);
    lv_obj_set_style_text_color(s->name_label, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(s->name_label, theme::font_time(), 0);
    lv_obj_set_style_text_align(s->name_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(s->name_label, 14, 0);

    // Title. Same single-line / ellipsis treatment; Orion limit ≤40 chars.
    // font_body() (20 px) matches the meeting-location and media-artist
    // sizes — the "secondary descriptor" tier across the device.
    s->title_label = lv_label_create(card);
    lv_label_set_text(s->title_label, mock_data::profile().title);
    lv_label_set_long_mode(s->title_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s->title_label, WIDTH - 16);
    lv_obj_set_style_text_color(s->title_label, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(s->title_label, theme::font_meta(), 0);
    lv_obj_set_style_text_align(s->title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(s->title_label, 6, 0);

    lv_obj_set_user_data(card, s);
    lv_obj_add_event_cb(card, [](lv_event_t* e) {
        lv_obj_t* c = static_cast<lv_obj_t*>(lv_event_get_target(e));
        if (g_active_card == c) g_active_card = nullptr;
        delete static_cast<CardState*>(lv_event_get_user_data(e));
    }, LV_EVENT_DELETE, s);
    return card;
}

lv_obj_t* photo_object(lv_obj_t* card) {
    auto* s = static_cast<CardState*>(lv_obj_get_user_data(card));
    return s ? s->photo : nullptr;
}

void set_presence(lv_obj_t* card, Presence p) {
    auto* s = static_cast<CardState*>(lv_obj_get_user_data(card));
    if (!s || !s->photo) return;
    uint32_t c = color_for_presence(p);
    lv_obj_set_style_border_color(s->photo, theme::color(c), LV_PART_MAIN);
    lv_obj_set_style_shadow_color(s->photo, theme::color(c), LV_PART_MAIN);
    // Offline: CSS uses box-shadow: none — match that by killing the opacity.
    lv_obj_set_style_shadow_opa(s->photo,
        p == Presence::Offline ? LV_OPA_TRANSP : LV_OPA_60, LV_PART_MAIN);
}

void set_default_presence(Presence p) {
    g_default_presence = p;
}

Presence get_default_presence() {
    return g_default_presence;
}

void set_photo(const lv_image_dsc_t* img_dsc) {
    // Store as the new default so future screen creates start with the photo.
    g_default_photo = img_dsc;

    // Update the currently visible card if one exists.
    if (!g_active_card) return;
    auto* s = static_cast<CardState*>(lv_obj_get_user_data(g_active_card));
    if (!s || !s->photo_img || !s->initials_lbl) return;

    if (img_dsc) {
        lv_image_set_src(s->photo_img, img_dsc);
        lv_obj_clear_flag(s->photo_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s->initials_lbl, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s->photo_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s->initials_lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

const lv_image_dsc_t* get_photo() {
    return g_default_photo;
}

} // namespace widget_profile_card
