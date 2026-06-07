#include "widgets/widget_profile_card.h"

#include <cstring>
#include "screens/modal_factory_reset.h"
#include "screens/modal_profile.h"
#include "theme.h"

// LVGL has no native multi-stop gradient; bg_grad_dir = VER with two colour
// stops approximates the prototype's vertical linear-gradient closely enough.

namespace {

struct CardState {
    lv_obj_t* photo_ring;   // gradient ring — PHOTO_SIZE+12 circle, bg is the gradient border
    lv_obj_t* photo;        // inner photo circle — PHOTO_SIZE, clip_corner, holds photo_img
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

// Weak reference to the most-recently-created card, used by set_photo() and
// set_profile() to update the live screen without a screen rebuild.
// Cleared when the card is deleted (LV_EVENT_DELETE handler).
lv_obj_t* g_active_card = nullptr;

// Optional live photo object registered by modal_profile. Receives border-colour
// updates alongside g_active_card so presence toggles reflect immediately inside
// the overlay without closing and reopening it.
lv_obj_t* g_modal_photo = nullptr;

// Live text-label handles for the modal_profile overlay. Populated by
// register_modal_labels(); zeroed on unregister_modal_labels().
widget_profile_card::ModalLabels g_modal_labels = {};

// Cached name, title, email, phone — populated from NVS at boot and updated on
// every BLE ProfileInfo write. create() reads these directly so the
// real synced values appear after the first setup sync.
char g_name[65]   = {};
char g_title[65]  = {};
char g_email[129] = {};
char g_phone[33]  = {};

// Returns g_name/g_title if set, otherwise an em-dash placeholder so the
// profile card never renders blank before a first sync.
static const char* display_name()  { return g_name[0]  ? g_name  : "\xe2\x80\x94"; }
static const char* display_title() { return g_title[0] ? g_title : "\xe2\x80\x94"; }

uint32_t color_for_presence(widget_profile_card::Presence p) {
    switch (p) {
        case widget_profile_card::Presence::Available: return theme::COLOR_PRESENCE_AVAILABLE;
        case widget_profile_card::Presence::Busy:      return theme::COLOR_PRESENCE_BUSY;
        case widget_profile_card::Presence::Away:      return theme::COLOR_PRESENCE_AWAY;
        case widget_profile_card::Presence::Offline:
        default:                                       return theme::COLOR_PRESENCE_OFFLINE;
    }
}

// Top gradient stop — near-white pastel tint of the presence colour.
uint32_t color_for_presence_light(widget_profile_card::Presence p) {
    switch (p) {
        case widget_profile_card::Presence::Available: return theme::COLOR_PRESENCE_AVAILABLE_LIGHT;
        case widget_profile_card::Presence::Busy:      return theme::COLOR_PRESENCE_BUSY_LIGHT;
        case widget_profile_card::Presence::Away:      return theme::COLOR_PRESENCE_AWAY_LIGHT;
        case widget_profile_card::Presence::Offline:
        default:                                       return theme::COLOR_PRESENCE_OFFLINE_LIGHT;
    }
}

// Bottom gradient stop — deep dark shade of the presence colour.
uint32_t color_for_presence_dark(widget_profile_card::Presence p) {
    switch (p) {
        case widget_profile_card::Presence::Available: return theme::COLOR_PRESENCE_AVAILABLE_DARK;
        case widget_profile_card::Presence::Busy:      return theme::COLOR_PRESENCE_BUSY_DARK;
        case widget_profile_card::Presence::Away:      return theme::COLOR_PRESENCE_AWAY_DARK;
        case widget_profile_card::Presence::Offline:
        default:                                       return theme::COLOR_PRESENCE_OFFLINE_DARK;
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
    // LV_OBJ_FLAG_OVERFLOW_VISIBLE is intentionally NOT set here — the solid
    // border stays within the card bounds and does not need to bleed outward.

    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto* s = new CardState();
    s->photo_img = nullptr;

    // Track the live card so set_photo() can update it without a screen rebuild.
    g_active_card = card;

    // ── Gradient presence ring ────────────────────────────────────────────────
    // A circle 12 px larger than the photo (PHOTO_SIZE + 12 = 6 px each side).
    // Its gradient background IS the "border" — top = presence colour,
    // bottom = darker shade. Replaces the old solid border_color approach so
    // the ring can be updated with a single bg_color / bg_grad_color call.
    // Tap / long-press events are on the inner photo; the ring is non-clickable.
    s->photo_ring = lv_obj_create(card);
    lv_obj_set_size(s->photo_ring, PHOTO_SIZE + 12, PHOTO_SIZE + 12);
    lv_obj_set_style_radius(s->photo_ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s->photo_ring,
        theme::color(color_for_presence_light(g_default_presence)), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(s->photo_ring,
        theme::color(color_for_presence_dark(g_default_presence)), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(s->photo_ring, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s->photo_ring, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s->photo_ring, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s->photo_ring, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s->photo_ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s->photo_ring, LV_OBJ_FLAG_CLICKABLE);

    // ── Inner photo circle ────────────────────────────────────────────────────
    // The presence ring peeks 6 px around all edges of this circle.
    // Tap opens the profile detail overlay; long-press (3 s) opens factory reset.
    s->photo = lv_obj_create(s->photo_ring);
    lv_obj_set_size(s->photo, PHOTO_SIZE, PHOTO_SIZE);
    lv_obj_center(s->photo);
    lv_obj_set_style_radius(s->photo, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s->photo, theme::color(0x2A3140), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(s->photo, theme::color(0x1A1F28), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(s->photo, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s->photo, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s->photo, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s->photo, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s->photo, LV_OBJ_FLAG_SCROLLABLE);
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
        // Pass photo_ring so modal_profile aligns its ring to the same position.
        modal_profile::create(lv_screen_active(), s ? s->photo_ring : nullptr);
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(s->photo, [](lv_event_t* e) {
        auto* s = static_cast<CardState*>(lv_obj_get_user_data((lv_obj_t*)lv_event_get_target(e)));
        if (s) s->suppress_click = true;
        modal_factory_reset::create(lv_screen_active());
    }, LV_EVENT_LONG_PRESSED, nullptr);

    // Clip photo image to the circular boundary.
    lv_obj_set_style_clip_corner(s->photo, true, LV_PART_MAIN);

    // Photo image — hidden initially; shown when a decoded JPEG is available.
    // Sized to fill the container so the circle clip masks it correctly.
    s->photo_img = lv_image_create(s->photo);
    lv_obj_set_size(s->photo_img, PHOTO_SIZE, PHOTO_SIZE);
    lv_obj_align(s->photo_img, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s->photo_img, LV_OBJ_FLAG_HIDDEN);

    // Apply a cached photo immediately if one was already received before
    // this card was created (e.g. boot with NVS photo, or screen transition
    // after photo arrived).
    if (g_default_photo) {
        lv_image_set_src(s->photo_img, g_default_photo);
        lv_obj_clear_flag(s->photo_img, LV_OBJ_FLAG_HIDDEN);
    }

    // Name. Uses font_time() (30 px). Single line with ellipsis per
    // screen-layout.md ("Full name — single line, ellipsis on overflow").
    // Orion enforces name ≤ 24 chars at input so truncation is rare.
    s->name_label = lv_label_create(card);
    lv_label_set_text(s->name_label, display_name());
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
    lv_label_set_text(s->title_label, display_title());
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
    return s ? s->photo_ring : nullptr;
}

void set_presence(lv_obj_t* card, Presence p) {
    auto* s = static_cast<CardState*>(lv_obj_get_user_data(card));
    if (!s || !s->photo_ring) return;
    lv_obj_set_style_bg_color(s->photo_ring,
        theme::color(color_for_presence_light(p)), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(s->photo_ring,
        theme::color(color_for_presence_dark(p)), LV_PART_MAIN);
}

void set_default_presence(Presence p) {
    g_default_presence = p;
    if (g_active_card) set_presence(g_active_card, p);
    if (g_modal_photo) {
        lv_obj_set_style_bg_color(g_modal_photo,
            theme::color(color_for_presence_light(p)), LV_PART_MAIN);
        lv_obj_set_style_bg_grad_color(g_modal_photo,
            theme::color(color_for_presence_dark(p)), LV_PART_MAIN);
    }
    if (g_modal_labels.status_lbl) {
        const char* status_str;
        switch (p) {
            case Presence::Available: status_str = "Status: Available"; break;
            case Presence::Busy:      status_str = "Status: Busy";      break;
            case Presence::Away:      status_str = "Status: Away";      break;
            default:                  status_str = "Status: Offline";   break;
        }
        lv_label_set_text(g_modal_labels.status_lbl, status_str);
        lv_obj_set_style_text_color(g_modal_labels.status_lbl,
            theme::color(color_for_presence(p)), LV_PART_MAIN);
    }
}

void register_modal_photo(lv_obj_t* photo_obj) { g_modal_photo = photo_obj; }
void unregister_modal_photo()                   { g_modal_photo = nullptr; }

void register_modal_labels(const ModalLabels& labels) { g_modal_labels = labels; }
void unregister_modal_labels()                         { g_modal_labels = {}; }

Presence get_default_presence() {
    return g_default_presence;
}

void set_photo(const lv_image_dsc_t* img_dsc) {
    // Store as the new default so future screen creates start with the photo.
    g_default_photo = img_dsc;

    // Update the currently visible card if one exists.
    if (!g_active_card) return;
    auto* s = static_cast<CardState*>(lv_obj_get_user_data(g_active_card));
    if (!s || !s->photo_img) return;

    if (img_dsc) {
        lv_image_set_src(s->photo_img, img_dsc);
        lv_obj_clear_flag(s->photo_img, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s->photo_img, LV_OBJ_FLAG_HIDDEN);
    }
}

const lv_image_dsc_t* get_photo() {
    return g_default_photo;
}

void set_profile(const char* name, const char* title,
                 const char* email, const char* phone) {
    if (name)  strncpy(g_name,  name,  sizeof(g_name)  - 1);
    if (title) strncpy(g_title, title, sizeof(g_title) - 1);
    if (email) strncpy(g_email, email, sizeof(g_email) - 1);
    if (phone) strncpy(g_phone, phone, sizeof(g_phone) - 1);

    // Update the live card labels if a card is currently on screen.
    if (g_active_card) {
        auto* s = static_cast<CardState*>(lv_obj_get_user_data(g_active_card));
        if (s) {
            if (s->name_label)  lv_label_set_text(s->name_label,  display_name());
            if (s->title_label) lv_label_set_text(s->title_label, display_title());
        }
    }

    // Update the modal profile labels if the overlay is open.
    if (g_modal_labels.name_lbl)
        lv_label_set_text(g_modal_labels.name_lbl,  g_name[0]  ? g_name  : "\xe2\x80\x94");
    if (g_modal_labels.title_lbl)
        lv_label_set_text(g_modal_labels.title_lbl, g_title[0] ? g_title : "\xe2\x80\x94");
    if (g_modal_labels.email_lbl)
        lv_label_set_text(g_modal_labels.email_lbl, g_email);
    if (g_modal_labels.phone_lbl)
        lv_label_set_text(g_modal_labels.phone_lbl, g_phone);
}

const char* get_profile_name()  { return g_name; }
const char* get_profile_title() { return g_title; }
const char* get_profile_email() { return g_email; }
const char* get_profile_phone() { return g_phone; }

} // namespace widget_profile_card
