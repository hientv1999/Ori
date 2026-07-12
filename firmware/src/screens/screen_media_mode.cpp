#include "screens/screen_media_mode.h"

#include <lvgl.h>
#include <cstdlib>
#include <cstring>

#include "assets/shortcut_icons.h"
#include "ble/gatt_server.h"
#include "app_state.h"
#include "ori_log.h"
#include "photo_cache.h"
#include "theme.h"
#include "ui_helpers.h"
#include "widgets/widget_profile_card.h"
#include "widgets/widget_status_bar.h"

#include <esp_heap_caps.h>

// Declared in gatt_server.cpp — sets the vol-swipe override flag.
extern "C" void gatt_server_set_vol_swipe_active(bool active);

namespace {

constexpr int16_t LEFT_PANEL_WIDTH  = 528;
// Art: 484 × 216 — full usable panel width (528 − 22 pad each side), height unchanged.
constexpr int16_t ART_W             = 484;
constexpr int16_t ART_H             = 216;

// Gesture thresholds — mirror the HTML prototype values.
constexpr int16_t TAP_MAX           = 20;   // px in either axis = tap
constexpr int16_t H_SWIPE_MIN       = 50;   // px horizontal to count as swipe
constexpr int16_t V_SWIPE_ENGAGE    = 25;   // px vertical to engage volume HUD
// Sensitivity: ~400 px of swipe = full 0..100 range (factor = 100/400 = 0.25).
// 50% less sensitive than before (was 200 px / factor 0.5) — needs 2x the
// swipe distance for the same volume change.
constexpr int     V_SENS_NUM        = 1;
constexpr int     V_SENS_DEN        = 4;

// Play-triangle overlay — the paused-state indicator drawn on top of the
// album art. The HTML demo uses an inline SVG (#i-play, a solid filled
// triangle). LVGL's LV_SYMBOL_PLAY (U+F04B in FontAwesome) is not present
// in our custom Ori Montserrat fonts (we only ship ASCII + a handful of
// punctuation glyphs), and adding FontAwesome would balloon the font asset.
// In LVGL 9 we use lv_draw_triangle() via a LV_EVENT_DRAW_MAIN callback —
// no canvas buffer needed, no chroma-key required.
constexpr int PLAY_ICON_SIZE = 64;

static void fmt_time(char* buf, size_t sz, uint32_t seconds) {
    lv_snprintf(buf, sz, "%u:%02u", seconds / 60, seconds % 60);
}

// Timeline overlay geometry — shared between make_art_block() and on_seek_gesture().
constexpr int16_t TL_BAR_H     = 3;
constexpr int16_t TL_BAR_PAD   = 12;
constexpr int16_t TL_OVERLAY_H = 46;
constexpr int16_t TL_THUMB_SZ  = 8;

// Timeline auto-hide: hidden by default, revealed by touching the art,
// hidden again after this many ms of no further touch — see ArtState's
// tl_user_visible/tl_hide_timer doc comment.
constexpr uint32_t TL_AUTO_HIDE_MS = 5000;

struct ArtState {
    lv_obj_t* wrap;          // gesture surface / animation target for the whole art block
    lv_obj_t* art;           // gradient fallback — always visible behind art_img
    lv_obj_t* art_img;       // lv_image showing decoded JPEG; hidden until art arrives
    lv_obj_t* shortcuts_row; // row of 3 shortcut buttons — updated by update_shortcuts()
    lv_obj_t* paused_overlay;
    lv_obj_t* hud;             // volume HUD (initially hidden)
    lv_obj_t* hud_fill;        // accent-fill bar inside HUD
    lv_obj_t* hud_pct_label;   // "NN%" text
    lv_obj_t* title_label;
    lv_obj_t* artist_label;
    // Timeline seek widgets — always created, shown/hidden by update_seek().
    lv_obj_t* tl_overlay;      // semi-transparent bar at art bottom — hidden until can_seek+dur arrive
    lv_obj_t* tl_fill;         // accent fill bar — width tracks playhead
    lv_obj_t* tl_thumb;        // playhead dot — x tracks playhead
    lv_obj_t* tl_cur_label;    // current-time text — updated live during seek
    uint32_t  tl_dur_s;        // cached track duration for seek position math
    // Timeline visibility — hidden by default, revealed by touching the art
    // (tap/swipe/seek-drag), auto-hidden again after TL_AUTO_HIDE_MS idle.
    // Actual shown-ness is tl_seek_eligible && tl_user_visible (see
    // apply_timeline_visibility()) — either one alone isn't enough: no point
    // revealing a bar for a non-seekable/no-media track, and eligibility
    // alone shouldn't force it on-screen without the user asking to see it.
    bool        tl_seek_eligible;  // duration_s > 0 && can_seek (update_seek())
    bool        tl_user_visible;   // user tapped to reveal; clears after idle timeout
    lv_timer_t* tl_hide_timer;     // one-shot-per-reveal 5 s countdown; paused when idle
    // Captured at the start of each press (before reveal_timeline() runs) —
    // whether the bar was ALREADY showing coming into this touch. A tap only
    // toggles play/pause when this is true; the first tap on a hidden bar
    // (when eligible) is consumed entirely by revealing it instead. See
    // on_art_gesture's RELEASED tap-case.
    bool        tl_was_visible_before_press;
    // Drag tracking
    int       start_x;
    int       start_y;
    int       start_volume;
    bool      tracking;
    bool      vertical_engaged;
};

// Live pointer to the ArtState of the currently active media screen.
// Accessed by the public update_* functions in namespace screen_media_mode
// below. Anonymous-namespace scope keeps it TU-local (same as g_active_card
// in widget_profile_card.cpp).
ArtState* g_active_art = nullptr;

// 1-second dead-reckoning timer — advances position_s while playing so the
// scrubber moves smoothly between BLE position pushes from Orion.
// Created in create(), destroyed in the screen's LV_EVENT_DELETE handler.
static lv_timer_t* g_pos_timer = nullptr;

// RAM-only PSRAM cache for the current album art (not persisted to LittleFS).
// Cleared when a new track arrives or the media screen is rebuilt.
static uint16_t*      g_art_buf = nullptr;
static lv_image_dsc_t g_art_dsc = {};

// Last title pushed to the labels — used to detect track changes so album art
// is cleared immediately when a new track arrives (before its art lands).
static char g_displayed_title[193] = {};

// Build the paused-state play-triangle widget on top of the album art.
// Matches the HTML #i-play SVG path `M7 5v14l12-7z` at viewBox 24x24,
// scaled to PLAY_ICON_SIZE x PLAY_ICON_SIZE (~×2.67).
// LVGL 9: drawn via lv_draw_triangle() in a LV_EVENT_DRAW_MAIN callback
// rather than a canvas buffer — no chroma key, no BSS allocation.
lv_obj_t* make_play_triangle(lv_obj_t* parent) {
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_set_size(obj, PLAY_ICON_SIZE, PLAY_ICON_SIZE);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(obj);
    // Triangle vertices: HTML (7,5)/(7,19)/(19,12) ×2.67 → (19,13)/(19,51)/(51,32).
    lv_obj_add_event_cb(obj, [](lv_event_t* e) {
        lv_layer_t* layer = lv_event_get_layer(e);
        lv_obj_t*   o     = (lv_obj_t*)lv_event_get_target(e);
        lv_area_t   coords;
        lv_obj_get_coords(o, &coords);
        lv_draw_triangle_dsc_t dsc;
        lv_draw_triangle_dsc_init(&dsc);
        dsc.p[0].x = coords.x1 + 19; dsc.p[0].y = coords.y1 + 13;
        dsc.p[1].x = coords.x1 + 19; dsc.p[1].y = coords.y1 + 51;
        dsc.p[2].x = coords.x1 + 51; dsc.p[2].y = coords.y1 + 32;
        dsc.color = lv_color_white();
        dsc.opa   = LV_OPA_COVER;
        lv_draw_triangle(layer, &dsc);
    }, LV_EVENT_DRAW_MAIN, nullptr);
    return obj;
}

void apply_paused_visual(ArtState* s, bool paused) {
    if (!s || !s->art) return;
    // Dim when paused. Two separate objects need the same opacity: s->art is
    // the gradient fallback (always present, shown before real art arrives or
    // when there's none), and s->art_img is the decoded-JPEG overlay that
    // sits on top of it once real album art loads (photo_cache::
    // decode_to_psram / set_album_art()) — s->art_img is drawn at
    // LV_OPA_COVER over s->art, so dimming only s->art had no visible effect
    // once a real photo was showing (the fade only ever worked pre-art, on
    // the gradient placeholder). Both must move together.
    const lv_opa_t opa = paused ? LV_OPA_50 : LV_OPA_COVER;
    lv_obj_set_style_opa(s->art, opa, LV_PART_MAIN);
    if (s->art_img) lv_obj_set_style_opa(s->art_img, opa, LV_PART_MAIN);
    // Show / hide the centred play-triangle overlay.
    // paused_overlay is a child of wrap (not s->art) so it is unaffected by the opa change.
    if (paused) lv_obj_clear_flag(s->paused_overlay, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_add_flag(s->paused_overlay,   LV_OBJ_FLAG_HIDDEN);
}

void set_volume_visual(ArtState* s, int volume) {
    if (!s) return;
    if (volume < 0)   volume = 0;
    if (volume > 100) volume = 100;
    app_state::set_media_volume(volume);
    // HUD bar fill — height is a fraction of the bar container.
    if (s->hud_fill) lv_obj_set_height(s->hud_fill, lv_pct(volume));
    if (s->hud_pct_label) {
        char buf[8];
        lv_snprintf(buf, sizeof(buf), "%d%%", volume);
        lv_label_set_text(s->hud_pct_label, buf);
    }
}

void show_hud(ArtState* s, bool show) {
    if (!s || !s->hud) return;
    if (show) lv_obj_clear_flag(s->hud, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_add_flag(s->hud,   LV_OBJ_FLAG_HIDDEN);
}

// Single source of truth for tl_overlay's actual shown/hidden state — always
// call this instead of touching the HIDDEN flag directly, so the two inputs
// (is it even seekable right now, has the user asked to see it) can never
// drift out of sync with what's on screen.
void apply_timeline_visibility(ArtState* s) {
    if (!s || !s->tl_overlay) return;
    const bool visible = s->tl_seek_eligible && s->tl_user_visible;
    if (visible) lv_obj_clear_flag(s->tl_overlay, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag(s->tl_overlay,   LV_OBJ_FLAG_HIDDEN);
}

constexpr uint32_t TL_FADE_OUT_MS = 400;

void tl_overlay_opa_cb(void* obj, int32_t v) {
    lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), (lv_opa_t)v, 0);
}

// Runs once the auto-hide fade finishes: actually hides the overlay (so it
// stops intercepting seek touches once invisible) and restores full opacity
// for next time — reveal_timeline() always shows the bar instantly, never
// fading in, so it must not still be transparent on its next reveal.
void tl_fade_out_done(lv_anim_t* a) {
    lv_obj_t* overlay = static_cast<lv_obj_t*>(a->var);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(overlay, LV_OPA_COVER, 0);
}

// Auto-hide timeout ONLY — fades the bar out over TL_FADE_OUT_MS instead of
// snapping it away instantly. Every other hide path (track becomes
// non-seekable, etc.) still goes through apply_timeline_visibility() for an
// instant hide; only the idle-timeout dismissal should feel gradual.
void fade_out_timeline(ArtState* s) {
    if (!s || !s->tl_overlay) return;
    s->tl_user_visible = false;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s->tl_overlay);
    lv_anim_set_exec_cb(&a, tl_overlay_opa_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&a, TL_FADE_OUT_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&a, tl_fade_out_done);
    lv_anim_start(&a);
}

// Called on any touch of the art surface (tap, swipe, or a press on the
// timeline overlay itself) — reveals the bar and (re)starts the 5 s idle
// countdown. Always instant, never a fade-in — including cancelling and
// snapping back from a fade-out that was already in progress, so a touch
// that lands mid-fade doesn't leave the bar to keep dimming (or get hidden
// by the fade's own completion callback) right after the user asked to see
// it again. A no-op when nothing is currently seekable: there's nothing to
// reveal, and leaving tl_user_visible false in that case means a later
// track that DOES become seekable doesn't spuriously start out shown from a
// stale flag.
void reveal_timeline(ArtState* s) {
    if (!s || !s->tl_seek_eligible) return;
    if (s->tl_overlay) {
        lv_anim_delete(s->tl_overlay, tl_overlay_opa_cb);
        lv_obj_set_style_opa(s->tl_overlay, LV_OPA_COVER, 0);
    }
    s->tl_user_visible = true;
    apply_timeline_visibility(s);
    if (s->tl_hide_timer) {
        lv_timer_reset(s->tl_hide_timer);
        lv_timer_resume(s->tl_hide_timer);
    }
}

// Seek gesture — bound to the timeline overlay so touches in the bottom 46 px
// of the art are handled here exclusively and do NOT bubble to on_art_gesture.
// Drags update the fill/thumb/label live; release emits
// KeyboardCommand{op:"seek", arg:new_pos}.
void on_seek_gesture(lv_event_t* e) {
    auto* s = static_cast<ArtState*>(lv_event_get_user_data(e));
    if (!s || !s->tl_fill) return;
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t* indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    auto* overlay = static_cast<lv_obj_t*>(lv_event_get_current_target(e));

    // Convert absolute touch x to bar-relative x (clamped to bar bounds).
    lv_area_t ov_area;
    lv_obj_get_coords(overlay, &ov_area);
    const int16_t bar_w = ART_W - TL_BAR_PAD * 2;
    int16_t rel_x = (int16_t)((int)p.x - (int)ov_area.x1) - TL_BAR_PAD;
    if (rel_x < 0)     rel_x = 0;
    if (rel_x > bar_w) rel_x = bar_w;

    const uint32_t dur     = s->tl_dur_s > 0 ? s->tl_dur_s : 1;
    const uint32_t new_pos = (uint32_t)((int32_t)rel_x * (int32_t)dur / (int32_t)bar_w);

    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        // Touching the bar itself counts as interacting with it — reveal (a
        // no-op if already visible) and keep resetting the idle countdown on
        // every PRESSING tick so a drag longer than TL_AUTO_HIDE_MS can't get
        // hidden out from under the user's finger.
        reveal_timeline(s);
        lv_obj_set_width(s->tl_fill, rel_x);
        lv_obj_set_pos(s->tl_thumb,
            TL_BAR_PAD + rel_x - TL_THUMB_SZ / 2,
            8 - (TL_THUMB_SZ - TL_BAR_H) / 2);
        char buf[8];
        fmt_time(buf, sizeof(buf), new_pos);
        lv_label_set_text(s->tl_cur_label, buf);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        // Finalise: emit KeyboardCommand{op:"seek", arg:new_pos} over BLE.
        char buf[8];
        fmt_time(buf, sizeof(buf), new_pos);
        lv_label_set_text(s->tl_cur_label, buf);
        gatt_server::notify_keyboard_command("seek", new_pos);
    }
}

// Animation callback for the horizontal-swipe slide + snap-back effect.
// Must be a plain function pointer — lv_anim_exec_xcb_t is not compatible with lambdas.
static void art_translate_x_cb(void* obj, int32_t val) {
    lv_obj_set_style_translate_x(static_cast<lv_obj_t*>(obj), (int16_t)val, 0);
}

// Slide the art block ~40 px in the swipe direction then snap it back to centre.
// Matches media-mode.md: "Art shifts right/left ~40 px then snaps back".
static void animate_art_swipe(lv_obj_t* wrap, int direction /* +1=right, -1=left */) {
    constexpr uint32_t SLIDE_MS    = 120;
    constexpr uint32_t SNAPBACK_MS = 200;
    constexpr int16_t  SLIDE_PX    = 40;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, wrap);
    lv_anim_set_exec_cb(&a, art_translate_x_cb);
    lv_anim_set_values(&a, 0, direction * SLIDE_PX);
    lv_anim_set_duration(&a, SLIDE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    lv_anim_init(&a);
    lv_anim_set_var(&a, wrap);
    lv_anim_set_exec_cb(&a, art_translate_x_cb);
    lv_anim_set_values(&a, direction * SLIDE_PX, 0);
    lv_anim_set_duration(&a, SNAPBACK_MS);
    lv_anim_set_delay(&a, SLIDE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_start(&a);
}

// PRESSED — begin tracking a new touch: reveal the timeline bar and record
// the press origin + current volume as the baseline for whatever this touch
// turns out to be (tap, h-swipe, or v-swipe).
void on_art_pressed(ArtState* s, lv_point_t p) {
    // Capture BEFORE reveal_timeline() below changes it — see
    // tl_was_visible_before_press's doc comment. Not eligible at all
    // (nothing to reveal) counts as "already visible" so a tap on a
    // non-seekable track still toggles play/pause normally, same as
    // before this bar existed.
    s->tl_was_visible_before_press = !s->tl_seek_eligible || s->tl_user_visible;
    // Any touch on the art — tap, swipe, or the start of a volume drag —
    // reveals the timeline bar and (re)starts its 5 s idle countdown.
    reveal_timeline(s);
    s->start_x = p.x;
    s->start_y = p.y;
    s->start_volume = app_state::media().volume;
    s->tracking = true;
    s->vertical_engaged = false;
}

// PRESSING — live drag tracking. Latches vertical_engaged the first tick the
// drag crosses the vertical-swipe threshold (and shows the volume HUD +
// engages the drag-wins BLE override right then), then keeps the HUD synced
// to the drag on every subsequent tick while engaged.
void on_art_pressing(ArtState* s, lv_point_t p) {
    if (!s->tracking) return;
    int dx = p.x - s->start_x;
    int dy = p.y - s->start_y;
    if (!s->vertical_engaged && abs(dy) > V_SWIPE_ENGAGE && abs(dy) > abs(dx)) {
        s->vertical_engaged = true;
        show_hud(s, true);
        // Set drag-wins override: ignore incoming HostVolumeState pushes.
        gatt_server_set_vol_swipe_active(true);
    }
    if (s->vertical_engaged) {
        // Negative dy (swipe up) = louder; positive dy = quieter.
        int new_vol = s->start_volume + (-dy) * V_SENS_NUM / V_SENS_DEN;
        set_volume_visual(s, new_vol);
    }
}

enum class ReleaseGesture { kNone, kTap, kSwipeH };

// Pure decision, no state mutation and no BLE calls: given the total
// displacement of a finished touch that is ALREADY known not to be a
// vertical volume swipe (caller checks s->vertical_engaged first — see
// on_art_released), classify it as a tap, a horizontal swipe, or neither.
// Mirrors the original inline if/else-if precedence exactly: tap is checked
// before h-swipe.
ReleaseGesture classify_release_gesture(int abs_dx, int abs_dy) {
    if (abs_dx < TAP_MAX && abs_dy < TAP_MAX) return ReleaseGesture::kTap;
    if (abs_dx > H_SWIPE_MIN && abs_dx > abs_dy) return ReleaseGesture::kSwipeH;
    return ReleaseGesture::kNone;
}

// RELEASED / PRESS_LOST — finalize whatever this touch turned out to be. A
// vertical swipe latched during PRESSING is finished here directly (it
// bypasses classify_release_gesture entirely, same as the original code);
// otherwise the total displacement is classified as a tap or h-swipe and
// dispatched to its side effects.
void on_art_released(ArtState* s, lv_point_t p) {
    if (!s->tracking) return;
    s->tracking = false;
    int dx = p.x - s->start_x;
    int dy = p.y - s->start_y;

    if (s->vertical_engaged) {
        // Release vertical swipe — emit KeyboardCommand{op:"vol_set", arg:N}.
        int new_vol = s->start_volume + (-dy) * V_SENS_NUM / V_SENS_DEN;
        if (new_vol < 0)   new_vol = 0;
        if (new_vol > 100) new_vol = 100;
        set_volume_visual(s, new_vol);
        show_hud(s, false);
        s->vertical_engaged = false;
        // Clear the swipe-override so incoming HostVolumeState pushes resume
        // after the 800 ms linger (ble-protocol.md §12 drag-wins rule).
        gatt_server_set_vol_swipe_active(false);
        gatt_server::notify_keyboard_command("vol_set", (uint32_t)new_vol);
        return;
    }

    switch (classify_release_gesture(abs(dx), abs(dy))) {
        case ReleaseGesture::kTap:
            // Tap = play/pause toggle, but ONLY once the timeline bar is
            // already visible. The first tap on a hidden-and-eligible bar is
            // consumed entirely by revealing it (already done on PRESSED,
            // above) — the user taps again, now that they can see the bar,
            // to actually toggle playback. Tracks that aren't seek-eligible
            // at all skip this gate (tl_was_visible_before_press is forced
            // true for them) and toggle on every tap same as always.
            if (s->tl_was_visible_before_press) {
                bool playing = !app_state::media_playing();
                app_state::set_media_playing(playing);
                apply_paused_visual(s, !playing);
                gatt_server::notify_keyboard_command("play_pause", 0);
            }
            break;
        case ReleaseGesture::kSwipeH: {
            // Horizontal swipe — slide art ~40 px in the swipe direction then
            // snap back, and emit the next/prev KeyboardCommand.
            const int dir = (dx > 0) ? 1 : -1;
            animate_art_swipe(s->wrap, dir);
            gatt_server::notify_keyboard_command(dx > 0 ? "next" : "prev", 0);
            break;
        }
        case ReleaseGesture::kNone:
            break;
    }
}

void on_art_gesture(lv_event_t* e) {
    auto* s = static_cast<ArtState*>(lv_event_get_user_data(e));
    if (!s) return;
    lv_indev_t* indev = lv_indev_get_act();
    if (!indev) return;
    lv_event_code_t code = lv_event_get_code(e);
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    if (code == LV_EVENT_PRESSED) {
        on_art_pressed(s, p);
    } else if (code == LV_EVENT_PRESSING) {
        on_art_pressing(s, p);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        on_art_released(s, p);
    }
}

// ===== make_art_block() helpers — each builds one section of the art
// block, in the exact order make_art_block() previously built it inline.
// Split for readability only; no widget, style, or callback wiring changes
// vs. the prior single-function version. =====

// Outer wrapper — fixed size, used as the gesture surface and animation target.
static lv_obj_t* make_art_wrap(lv_obj_t* parent, ArtState* s) {
    lv_obj_t* wrap = lv_obj_create(parent);
    s->wrap = wrap;
    lv_obj_set_size(wrap, ART_W, ART_H);
    lv_obj_set_style_pad_all(wrap, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(wrap, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(wrap, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_radius(wrap, 14, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(wrap, true, LV_PART_MAIN);
    lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(wrap, LV_OBJ_FLAG_CLICKABLE);
    return wrap;
}

// Album art gradient fallback — approximates the HTML prototype's mock
// gradient. Always visible behind s->art_img (below), which overlays the
// real decoded JPEG from the Media Album Art characteristic once it arrives.
static void make_art_gradient(lv_obj_t* wrap, ArtState* s) {
    s->art = lv_obj_create(wrap);
    lv_obj_set_size(s->art, ART_W, ART_H);
    lv_obj_align(s->art, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(s->art, 14, LV_PART_MAIN);
    lv_obj_set_style_border_width(s->art, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s->art, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s->art, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(s->art, LV_GRAD_DIR_VER, LV_PART_MAIN);
    // Vibrant gradient always — 2-stop vertical approximation of the HTML prototype's
    // 4-stop 135° gradient: #2c1a4c → #6e2456 → #c44b3d → #f5b042. Shown regardless
    // of BLE/media state; the brand mark overlays it in the empty (no-art) state.
    lv_obj_set_style_bg_color(s->art, theme::color(0x2C1A4C), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(s->art, theme::color(0xF5B042), LV_PART_MAIN);
    lv_obj_set_style_shadow_color(s->art, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s->art, 32, LV_PART_MAIN);
    lv_obj_set_style_shadow_ofs_y(s->art, 12, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(s->art, LV_OPA_60, LV_PART_MAIN);
    lv_obj_clear_flag(s->art, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s->art, LV_OBJ_FLAG_CLICKABLE);
}

// lv_image overlay — sits on top of the gradient and shows the decoded
// JPEG once set_album_art() delivers it. Hidden until then so the
// gradient fallback shows through.
static void make_art_image(lv_obj_t* wrap, ArtState* s) {
    s->art_img = lv_image_create(wrap);
    lv_obj_set_size(s->art_img, ART_W, ART_H);
    lv_obj_align(s->art_img, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(s->art_img, 14, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(s->art_img, true, LV_PART_MAIN);
    lv_obj_clear_flag(s->art_img, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s->art_img, LV_OBJ_FLAG_CLICKABLE);
    // Restore from cache if art was received before the screen was created
    // (e.g. Controls mode entered after a sync that already pushed art).
    if (g_art_buf) {
        lv_image_set_src(s->art_img, &g_art_dsc);
        lv_obj_clear_flag(s->art_img, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s->art_img, LV_OBJ_FLAG_HIDDEN);
    }
}

// Volume HUD — vertical fill bar + percentage text, on the right edge.
static void make_volume_hud(lv_obj_t* wrap, ArtState* s) {
    s->hud = lv_obj_create(wrap);
    lv_obj_set_size(s->hud, 60, 180);
    lv_obj_align(s->hud, LV_ALIGN_RIGHT_MID, -16, 0);
    ui::clear_container(s->hud);
    lv_obj_add_flag(s->hud, LV_OBJ_FLAG_HIDDEN);  // hidden until vertical drag engages

    // HUD bar background.
    lv_obj_t* bar_bg = lv_obj_create(s->hud);
    lv_obj_set_size(bar_bg, 10, 160);
    lv_obj_align(bar_bg, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(bar_bg, 5, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_bg, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar_bg, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar_bg, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar_bg, 0, LV_PART_MAIN);
    lv_obj_clear_flag(bar_bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(bar_bg, LV_OBJ_FLAG_CLICKABLE);

    // HUD bar fill (anchored to bottom, height = volume %).
    s->hud_fill = lv_obj_create(bar_bg);
    lv_obj_set_size(s->hud_fill, lv_pct(100), lv_pct(app_state::media().volume));
    lv_obj_align(s->hud_fill, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(s->hud_fill, 5, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s->hud_fill, theme::color(theme::COLOR_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s->hud_fill, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s->hud_fill, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s->hud_fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s->hud_fill, LV_OBJ_FLAG_CLICKABLE);

    // HUD percentage label.
    s->hud_pct_label = lv_label_create(s->hud);
    {
        char buf[8];
        lv_snprintf(buf, sizeof(buf), "%d%%", app_state::media().volume);
        lv_label_set_text(s->hud_pct_label, buf);
    }
    lv_obj_set_style_text_color(s->hud_pct_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s->hud_pct_label, theme::font_meta(), 0);
    lv_obj_align(s->hud_pct_label, LV_ALIGN_TOP_MID, 0, -28);
}

// Timeline bar — always created so update_seek() can show it later when
// Orion pushes can_seek=true + position/duration. This avoids the "built
// before media arrived" problem where tl_fill was null and update_seek
// was a no-op. Always starts HIDDEN regardless of eligibility — the bar
// is tap-to-reveal (media-mode.md), not shown-whenever-seekable; the
// user must touch the art at least once before it appears.
static void make_timeline_overlay(lv_obj_t* wrap, ArtState* s) {
    // Re-read current media state — same value make_art_block() read
    // earlier in the original single-function version; nothing mutates
    // app_state between then and now within this synchronous UI build.
    const auto& m        = app_state::media();
    const bool has_media = m.has_media;

    const uint32_t pos_s  = m.position_s;
    const uint32_t dur_s  = m.duration_s > 0 ? m.duration_s : 1;
    const int16_t  bar_w  = ART_W - TL_BAR_PAD * 2;
    const int16_t  fill_w = (int16_t)((int32_t)bar_w * (int32_t)pos_s / (int32_t)dur_s);

    s->tl_seek_eligible = (has_media && m.can_seek && m.duration_s > 0);
    s->tl_user_visible  = false;

    lv_obj_t* overlay = lv_obj_create(wrap);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);  // tl_user_visible starts false
    s->tl_overlay = overlay;
    lv_obj_set_size(overlay, ART_W, TL_OVERLAY_H);
    lv_obj_align(overlay, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(overlay, 0, LV_PART_MAIN);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    // overlay is intentionally clickable — seek events land here so
    // they never reach wrap's on_art_gesture handler.

    // Track.
    lv_obj_t* track = lv_obj_create(overlay);
    lv_obj_set_size(track, bar_w, TL_BAR_H);
    lv_obj_set_pos(track, TL_BAR_PAD, 8);
    lv_obj_set_style_radius(track, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(track, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(track, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_border_width(track, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(track, 0, LV_PART_MAIN);
    lv_obj_clear_flag(track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(track, LV_OBJ_FLAG_CLICKABLE);

    // Accent fill — width updated live during seek via s->tl_fill.
    lv_obj_t* fill = lv_obj_create(track);
    s->tl_fill = fill;
    lv_obj_set_size(fill, fill_w, LV_PCT(100));
    lv_obj_align(fill, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(fill, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(fill, theme::color(theme::COLOR_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(fill, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(fill, 0, LV_PART_MAIN);
    lv_obj_clear_flag(fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(fill, LV_OBJ_FLAG_CLICKABLE);

    // Thumb dot at the playhead — position updated live during seek via s->tl_thumb.
    lv_obj_t* thumb = lv_obj_create(overlay);
    s->tl_thumb = thumb;
    lv_obj_set_size(thumb, TL_THUMB_SZ, TL_THUMB_SZ);
    lv_obj_set_pos(thumb,
        TL_BAR_PAD + fill_w - TL_THUMB_SZ / 2,
        8 - (TL_THUMB_SZ - TL_BAR_H) / 2);
    lv_obj_set_style_radius(thumb, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(thumb, theme::color(theme::COLOR_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(thumb, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(thumb, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(thumb, theme::color(theme::COLOR_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(thumb, 6, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(thumb, LV_OPA_50, LV_PART_MAIN);
    lv_obj_clear_flag(thumb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(thumb, LV_OBJ_FLAG_CLICKABLE);

    // Timestamps.
    char cur_buf[8], dur_buf[8];
    fmt_time(cur_buf, sizeof(cur_buf), pos_s);
    fmt_time(dur_buf, sizeof(dur_buf), dur_s);

    s->tl_dur_s = dur_s;

    auto make_time_label = [&](const char* text, lv_align_t align, int16_t x_ofs) -> lv_obj_t* {
        lv_obj_t* lbl = lv_label_create(overlay);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_font(lbl, theme::font_body(), 0);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_set_style_text_opa(lbl, LV_OPA_80, 0);
        lv_obj_align(lbl, align, x_ofs, -4);
        return lbl;
    };
    s->tl_cur_label = make_time_label(cur_buf, LV_ALIGN_BOTTOM_LEFT,  TL_BAR_PAD);
                      make_time_label(dur_buf, LV_ALIGN_BOTTOM_RIGHT, -TL_BAR_PAD);

    // Seek events land on the overlay so they do not reach wrap's on_art_gesture.
    lv_obj_add_event_cb(overlay, on_seek_gesture, LV_EVENT_PRESSED,    s);
    lv_obj_add_event_cb(overlay, on_seek_gesture, LV_EVENT_PRESSING,   s);
    lv_obj_add_event_cb(overlay, on_seek_gesture, LV_EVENT_RELEASED,   s);
    lv_obj_add_event_cb(overlay, on_seek_gesture, LV_EVENT_PRESS_LOST, s);

    // Auto-hide countdown — created paused; reveal_timeline() resets +
    // resumes it on every touch, the callback re-pauses itself after
    // firing so it sits idle (not ticking) between reveals.
    s->tl_hide_timer = lv_timer_create([](lv_timer_t* t) {
        auto* st = static_cast<ArtState*>(lv_timer_get_user_data(t));
        fade_out_timeline(st);
        lv_timer_pause(t);
    }, TL_AUTO_HIDE_MS, s);
    lv_timer_pause(s->tl_hide_timer);
}

lv_obj_t* make_art_block(lv_obj_t* parent, ArtState* s) {
    lv_obj_t* wrap = make_art_wrap(parent, s);

    make_art_gradient(wrap, s);
    make_art_image(wrap, s);

    // Centred play-triangle overlay shown while paused. Drawn via lv_canvas
    // because LV_SYMBOL_PLAY (FontAwesome U+F04B) is not present in our
    // custom Montserrat fonts — see make_play_triangle() for the why.
    s->paused_overlay = make_play_triangle(wrap);

    make_volume_hud(wrap, s);
    make_timeline_overlay(wrap, s);

    // Gesture handlers — bound to the wrapper so the art + overlays move
    // as one when LVGL dispatches events.
    lv_obj_add_event_cb(wrap, on_art_gesture, LV_EVENT_PRESSED,     s);
    lv_obj_add_event_cb(wrap, on_art_gesture, LV_EVENT_PRESSING,    s);
    lv_obj_add_event_cb(wrap, on_art_gesture, LV_EVENT_RELEASED,    s);
    lv_obj_add_event_cb(wrap, on_art_gesture, LV_EVENT_PRESS_LOST,  s);

    // Apply initial paused-state visual.
    apply_paused_visual(s, !app_state::media_playing());
    return wrap;
}

lv_obj_t* make_meta_block(lv_obj_t* parent, ArtState* s) {
    // Fixed-width block so LV_LABEL_LONG_DOT has something to truncate
    // against — `lv_pct(100)` inside a flex-column parent that's wider
    // than the panel was causing the title to wrap instead of ellipsise.
    constexpr int16_t META_W = LEFT_PANEL_WIDTH - 60;
    lv_obj_t* meta = lv_obj_create(parent);
    lv_obj_set_size(meta, META_W, LV_SIZE_CONTENT);
    ui::clear_container(meta);
    lv_obj_set_style_pad_top(meta, 10, LV_PART_MAIN);
    lv_obj_set_flex_flow(meta, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(meta, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    const auto& m = app_state::media();
    const bool has        = m.has_media && m.title[0];
    const bool has_artist = has && m.artist[0];

    // LV_LABEL_LONG_DOT in LVGL 8.x truncates only when the label's HEIGHT
    // is constrained to a single line — otherwise the label grows to a
    // second line and the text wraps. Set both width AND a one-line height
    // for the title/artist to get the desired ellipsis behaviour.
    // Media title — font_title() (24 px) to match the meeting-list title size.
    // Box height matches the font's real line_height (34) so descenders
    // can't overflow into the artist label below.
    s->title_label = lv_label_create(meta);
    lv_obj_set_size(s->title_label, META_W, 34);
    lv_label_set_long_mode(s->title_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(s->title_label, has ? m.title : "Nothing playing");
    lv_obj_set_style_text_color(s->title_label,
        theme::color(has ? theme::COLOR_TEXT_PRIMARY : theme::COLOR_TEXT_TERTIARY), 0);
    lv_obj_set_style_text_font(s->title_label, theme::font_title(), 0);
    lv_obj_set_style_text_align(s->title_label, LV_TEXT_ALIGN_CENTER, 0);

    // Artist — font_meta() (22 px) to match meeting-location and profile-title.
    // Box height matches the font's real line_height (32), same reasoning
    // as the title box above. pad_top is the actual (flex-layout) gap from
    // the title now — no more negative translate_y faking a tighter gap.
    // Checked independently of `has` (title's own presence) — a track with a
    // title but no artist tag must still fall back to "No artist" rather than
    // render blank; same independent check update_meta() already applies.
    s->artist_label = lv_label_create(meta);
    lv_obj_set_size(s->artist_label, META_W, 32);
    lv_label_set_long_mode(s->artist_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(s->artist_label, has_artist ? m.artist : "No artist");
    lv_obj_set_style_text_color(s->artist_label, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(s->artist_label, theme::font_meta(), 0);
    lv_obj_set_style_text_align(s->artist_label, LV_TEXT_ALIGN_CENTER, 0);
    // No extra pad_top — the title/artist box heights already match their
    // fonts' real line_height (34/32), so the boxes alone give the two
    // lines clean, non-overlapping room without any added gap.

    return meta;
}

// Sets one shortcut button's icon for `token` — shared by initial construction
// (make_shortcuts_row) and update_shortcuts()'s live refresh after a Device
// Settings write. Unrecognized tokens hide the whole button entirely
// (media-mode.md); recognised ones centre the compiled-in icon image and
// make sure the button is visible (a no-op on a freshly-created button,
// which isn't hidden yet; on a live refresh it undoes a prior bad token's
// hide).
void apply_shortcut_icon(lv_obj_t* btn, const char* token) {
    const lv_image_dsc_t* img = shortcut_icons::image(token);
    if (img) {
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t* img_obj = lv_image_create(btn);
        lv_image_set_src(img_obj, img);
        lv_obj_center(img_obj);
        lv_obj_clear_flag(img_obj, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
    }
}

// User-assignable shortcut buttons — tap emits KeyboardCommand{op:"shortcut", arg:N}.
lv_obj_t* make_shortcuts_row(lv_obj_t* parent, ArtState* s) {
    lv_obj_t* row = lv_obj_create(parent);
    // Row box height = button height (72) + internal pad_top (8). Button
    // height was 82 — shrunk to make room for the title/artist box-height
    // fix above (see make_meta_block() and the vertical-budget comment in
    // create()).
    lv_obj_set_size(row, lv_pct(100), 80);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_top(row, 8, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 14, 0);

    const auto* slots = app_state::shortcuts();
    for (size_t i = 0; i < app_state::SHORTCUT_COUNT; ++i) {
        lv_obj_t* btn = lv_obj_create(row);
        // Width: (484px available − 2×14px gap) ÷ 3 = 152px fills the row.
        // Height: 72 (was 82) — see make_shortcuts_row()'s row-height comment.
        lv_obj_set_size(btn, 152, 72);
        lv_obj_set_style_radius(btn, 14, LV_PART_MAIN);
        lv_obj_set_style_bg_color(btn, theme::color(theme::COLOR_CARD), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(btn, theme::color(theme::COLOR_DIVIDER), LV_PART_MAIN);
        lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
        lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(btn, theme::color(theme::COLOR_ACCENT_SOFT), LV_STATE_PRESSED);
        lv_obj_set_style_border_color(btn, theme::color(theme::COLOR_ACCENT_LINE), LV_STATE_PRESSED);
        lv_obj_set_style_opa(btn, LV_OPA_80, LV_STATE_PRESSED);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

        // slot is 1-indexed per ble-protocol.md. Store as user_data.
        lv_obj_set_user_data(btn, reinterpret_cast<void*>((uintptr_t)(i + 1)));
        lv_obj_add_event_cb(btn, [](lv_event_t* e) {
            if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
            uintptr_t slot = (uintptr_t)lv_obj_get_user_data(
                static_cast<lv_obj_t*>(lv_event_get_current_target(e)));
            gatt_server::notify_keyboard_command("shortcut", (uint32_t)slot);
        }, LV_EVENT_CLICKED, nullptr);

        // Unrecognized token (no compiled-in icon for it) hides the slot
        // entirely rather than showing a placeholder. Flex layout (CENTER
        // main-axis) re-centers the remaining visible slots automatically.
        apply_shortcut_icon(btn, slots[i].icon_token);
    }
    s->shortcuts_row = row;
    return row;
}

} // namespace

namespace screen_media_mode {

lv_obj_t* create() {
    lv_obj_t* screen = lv_obj_create(nullptr);
    theme::apply_to_screen(screen);

    // Status bar.
    lv_obj_t* bar = widget_status_bar::create(screen);
    lv_obj_set_pos(bar, 0, 0);
    widget_status_bar::set_mode(bar, widget_status_bar::Mode::Keyboard);

    lv_obj_t* body = ui::make_screen_body(screen);

    // Left panel — vertical flex: art / meta / shortcuts.
    lv_obj_t* left = lv_obj_create(body);
    lv_obj_set_size(left, LEFT_PANEL_WIDTH, lv_pct(100));
    lv_obj_set_style_bg_opa(left, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(left, 0, 0);
    lv_obj_set_style_pad_left(left, 22, 0);
    lv_obj_set_style_pad_right(left, 22, 0);
    // Body height is 396 (480 - 84 status bar). Vertical budget:
    //   10 (pad_top)  +  216 (art h)  +  10 (meta pad_top)  +  34 (title box)  +
    //   32 (artist box, directly below — no added pad_top)  +
    //   8 (shortcuts pad_top)  +  72 (shortcuts)  +  10 (pad_bottom)  =  392
    //   ✓ (4 px headroom)
    // Title/artist box heights match their fonts' real line_height (34/32)
    // so descenders can't overflow into the next label — that alone gives
    // clean separation, no extra pad_top needed between them. Shortcut
    // button height dropped 82 → 72 to make room for that line_height-
    // matched sizing — see make_meta_block() and make_shortcuts_row().
    // Art is 484 × 216 — fills the full usable panel width (528 − 22 pad each side).
    // The seek overlay is inside the art (zero extra vertical cost).
    // pad_row is forced to 0 — LVGL's default theme adds inter-child spacing that
    // would push the shortcut buttons past the bottom of the screen.
    lv_obj_set_style_pad_top(left, 10, 0);
    lv_obj_set_style_pad_bottom(left, 10, 0);
    lv_obj_set_style_pad_row(left, 0, 0);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto* state = new ArtState{};
    state->tracking = false;
    state->vertical_engaged = false;

    make_art_block(left, state);
    make_meta_block(left, state);
    make_shortcuts_row(left, state);

    ui::make_panel_divider(body);

    // Right profile card.
    widget_profile_card::create(body);

    // Register as the active media screen so update_* functions can reach live widgets.
    g_active_art = state;

    // Dead-reckoning position timer: fires every 1 s and advances position_s
    // by 1 while the track is playing — keeps the scrubber moving between
    // Orion's BLE position pushes. Orion resets the anchor on track change or
    // seek; the firmware advances from there on its own.
    // Guard against a stale timer from a previous screen (shouldn't happen
    // with auto_del screens, but be safe).
    if (g_pos_timer) { lv_timer_delete(g_pos_timer); g_pos_timer = nullptr; }
    g_pos_timer = lv_timer_create([](lv_timer_t*) {
        if (!app_state::media_playing()) return;
        const auto& m = app_state::media();
        if (m.duration_s == 0) return;
        uint32_t new_pos = m.position_s + 1;
        if (new_pos > m.duration_s) new_pos = m.duration_s;
        screen_media_mode::update_seek(new_pos, m.duration_s);
    }, 1000, nullptr);

    // Store the ArtState pointer and free it when the screen is destroyed.
    // load_screen() uses lv_scr_load_anim(..., auto_del=true), which defers
    // the OLD screen's deletion until after the NEW screen is already built
    // and has set g_active_art to itself. Only clear g_active_art if it
    // still points to THIS (the one being deleted) — otherwise this stale
    // callback would null out a newer screen's already-correct pointer.
    lv_obj_set_user_data(screen, state);
    lv_obj_add_event_cb(screen, [](lv_event_t* e) {
        ArtState* mine = static_cast<ArtState*>(lv_event_get_user_data(e));
        if (g_active_art == mine) g_active_art = nullptr;
        if (g_pos_timer) { lv_timer_delete(g_pos_timer); g_pos_timer = nullptr; }
        // Per-instance timer (unlike g_pos_timer above) — LVGL timers are a
        // separate registry from the lv_obj tree, so deleting `mine` below
        // would otherwise leak this one.
        if (mine->tl_hide_timer) lv_timer_delete(mine->tl_hide_timer);
        delete mine;
    }, LV_EVENT_DELETE, state);
    return screen;
}

void update_meta(const char* title, const char* artist) {
    app_state::set_media_meta(title, artist, app_state::media().can_seek);
    if (!g_active_art) return;
    const bool has = title && title[0];

    // When the track changes (or playback stops), clear the previous album art
    // immediately so the default gradient shows while waiting for new art to
    // arrive. Play/pause updates share the same metadata path but keep the same
    // title, so comparing titles limits the clear to real track changes.
    const char* new_title = has ? title : "";
    if (strcmp(new_title, g_displayed_title) != 0) {
        clear_album_art();
        strncpy(g_displayed_title, new_title, sizeof(g_displayed_title) - 1);
        g_displayed_title[sizeof(g_displayed_title) - 1] = '\0';
    }

    lv_label_set_text(g_active_art->title_label, has ? title : "Nothing playing");
    lv_obj_set_style_text_color(g_active_art->title_label,
        theme::color(has ? theme::COLOR_TEXT_PRIMARY : theme::COLOR_TEXT_TERTIARY), 0);
    lv_label_set_text(g_active_art->artist_label,
        (has && artist && artist[0]) ? artist : "No artist");
    // Force-hide timeline when nothing is playing — update_seek() re-shows it
    // when valid position+duration arrive for a new track.
    if (!has && g_active_art->tl_overlay)
        lv_obj_add_flag(g_active_art->tl_overlay, LV_OBJ_FLAG_HIDDEN);
}

void update_playing(bool playing) {
    app_state::set_media_playing(playing);
    if (!g_active_art) return;
    apply_paused_visual(g_active_art, !playing);
}

void update_shortcuts() {
    if (!g_active_art || !g_active_art->shortcuts_row) return;
    const auto* slots = app_state::shortcuts();
    lv_obj_t* row = g_active_art->shortcuts_row;
    for (size_t i = 0; i < app_state::SHORTCUT_COUNT; ++i) {
        lv_obj_t* btn = lv_obj_get_child(row, (int32_t)i);
        if (!btn) continue;
        // Remove the existing icon/label child and recreate from the updated token.
        lv_obj_t* old_child = lv_obj_get_child(btn, 0);
        if (old_child) lv_obj_del(old_child);
        apply_shortcut_icon(btn, slots[i].icon_token);
    }
}

void set_album_art(uint8_t* jpeg_buf, size_t len) {
    if (!jpeg_buf || len == 0) {
        if (jpeg_buf) heap_caps_free(jpeg_buf);
        clear_album_art();
        return;
    }

    uint16_t* new_buf = photo_cache::decode_to_psram(jpeg_buf, len, ART_W, ART_H);
    heap_caps_free(jpeg_buf);

    if (!new_buf) {
        LOG("[media] set_album_art: decode failed (%u bytes)\n", (unsigned)len);
        return;
    }

    // Swap in new buffer; free the old one.
    if (g_art_buf) heap_caps_free(g_art_buf);
    g_art_buf = new_buf;
    photo_cache::fill_image_dsc(&g_art_dsc, g_art_buf, ART_W, ART_H);

    if (g_active_art && g_active_art->art_img) {
        lv_image_set_src(g_active_art->art_img, &g_art_dsc);
        lv_obj_clear_flag(g_active_art->art_img, LV_OBJ_FLAG_HIDDEN);
    }
    LOG("[media] set_album_art: %ux%u displayed (%u bytes)\n",
        (unsigned)ART_W, (unsigned)ART_H, (unsigned)len);
}

void clear_album_art() {
    if (g_art_buf) {
        heap_caps_free(g_art_buf);
        g_art_buf = nullptr;
        g_art_dsc = {};
    }
    if (g_active_art && g_active_art->art_img)
        lv_obj_add_flag(g_active_art->art_img, LV_OBJ_FLAG_HIDDEN);
}

void update_seek(uint32_t position_s, uint32_t duration_s) {
    app_state::set_media_seek(position_s, duration_s);
    if (!g_active_art) return;
    // Eligible = we have a valid duration AND the OS supports seeking (no
    // media, or a non-seekable stream, makes the bar meaningless regardless
    // of whether the user wants to see it). This does NOT by itself make the
    // bar visible — see ArtState's tl_user_visible doc comment; visibility is
    // tap-to-reveal, apply_timeline_visibility() combines both.
    const bool eligible = (duration_s > 0 && app_state::media().can_seek);
    if (eligible != g_active_art->tl_seek_eligible) {
        g_active_art->tl_seek_eligible = eligible;
        if (!eligible) {
            // No longer anything to show — drop any pending reveal rather
            // than let a later track that becomes eligible again inherit a
            // stale "user asked to see it" flag from an unrelated track.
            g_active_art->tl_user_visible = false;
            if (g_active_art->tl_hide_timer) lv_timer_pause(g_active_art->tl_hide_timer);
        }
        apply_timeline_visibility(g_active_art);
    }
    if (!g_active_art->tl_fill || !eligible) return;
    const uint32_t dur    = duration_s > 0 ? duration_s : 1;
    const int16_t  bar_w  = ART_W - TL_BAR_PAD * 2;
    const int16_t  fill_w = (int16_t)((int32_t)bar_w * (int32_t)position_s / (int32_t)dur);
    lv_obj_set_width(g_active_art->tl_fill, fill_w);
    if (g_active_art->tl_thumb) {
        lv_obj_set_pos(g_active_art->tl_thumb,
            TL_BAR_PAD + fill_w - TL_THUMB_SZ / 2,
            8 - (TL_THUMB_SZ - TL_BAR_H) / 2);
    }
    if (g_active_art->tl_cur_label) {
        char buf[8];
        fmt_time(buf, sizeof(buf), position_s);
        lv_label_set_text(g_active_art->tl_cur_label, buf);
    }
    g_active_art->tl_dur_s = dur;
}

} // namespace screen_media_mode
