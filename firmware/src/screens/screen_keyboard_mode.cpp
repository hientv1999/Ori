#include "screens/screen_keyboard_mode.h"

#include <lvgl.h>
#include <cstdlib>

#include "mock_data.h"
#include "theme.h"
#include "ui_helpers.h"
#include "widgets/widget_profile_card.h"
#include "widgets/widget_status_bar.h"

// Controls mode (internal: kbd-mode). BLE commands wired in M5.

namespace {

constexpr int16_t LEFT_PANEL_WIDTH  = 528;
// 16:9 art: 384 × 216. Width fills most of the 484 px usable panel (22 px pad each side).
// Height stays 216 px so the vertical budget is identical to the previous 1:1 layout.
constexpr int16_t ART_W             = 384;
constexpr int16_t ART_H             = 216;

// Gesture thresholds — mirror the HTML prototype values.
constexpr int16_t TAP_MAX           = 20;   // px in either axis = tap
constexpr int16_t H_SWIPE_MIN       = 50;   // px horizontal to count as swipe
constexpr int16_t V_SWIPE_ENGAGE    = 25;   // px vertical to engage volume HUD
// Sensitivity: ~200 px of swipe = full 0..100 range (factor = 100/200 = 0.5).
// Stored as integer scaled by 100 to keep math fixed-point.
constexpr int     V_SENS_NUM        = 1;
constexpr int     V_SENS_DEN        = 2;

// Play-triangle overlay — the paused-state indicator drawn on top of the
// album art. The HTML demo uses an inline SVG (#i-play, a solid filled
// triangle). LVGL's LV_SYMBOL_PLAY (U+F04B in FontAwesome) is not present
// in our custom Ori Montserrat fonts (we only ship ASCII + a handful of
// punctuation glyphs), and adding FontAwesome would balloon the font asset.
// So we draw the triangle ourselves with lv_canvas + a chroma-keyed buffer.
// Memory cost: PLAY_ICON_SIZE^2 * 2 bytes RGB565 = 8 KB in BSS, shared
// across all controls-mode screen instances (only one is ever live).
constexpr int     PLAY_ICON_SIZE    = 64;
static lv_color_t s_play_icon_buf[PLAY_ICON_SIZE * PLAY_ICON_SIZE];

static void fmt_time(char* buf, size_t sz, uint32_t seconds) {
    lv_snprintf(buf, sz, "%u:%02u", seconds / 60, seconds % 60);
}

// Timeline overlay geometry — shared between make_art_block() and on_seek_gesture().
constexpr int16_t TL_BAR_H     = 3;
constexpr int16_t TL_BAR_PAD   = 12;
constexpr int16_t TL_OVERLAY_H = 46;
constexpr int16_t TL_THUMB_SZ  = 8;

struct ArtState {
    lv_obj_t* art;
    lv_obj_t* paused_overlay;
    lv_obj_t* hud;             // volume HUD (initially hidden)
    lv_obj_t* hud_fill;        // accent-fill bar inside HUD
    lv_obj_t* hud_pct_label;   // "NN%" text
    lv_obj_t* title_label;
    lv_obj_t* artist_label;
    // Timeline seek widgets (nullptr when nothing playing)
    lv_obj_t* tl_fill;         // accent fill bar — width tracks playhead
    lv_obj_t* tl_thumb;        // playhead dot — x tracks playhead
    lv_obj_t* tl_cur_label;    // current-time text — updated live during seek
    uint32_t  tl_dur_s;        // cached track duration for seek position math
    // Drag tracking
    int       start_x;
    int       start_y;
    int       start_volume;
    bool      tracking;
    bool      vertical_engaged;
};

// Build the paused-state play-triangle widget on top of the album art.
// Matches the HTML #i-play SVG path `M7 5v14l12-7z` at viewBox 24x24,
// scaled to PLAY_ICON_SIZE x PLAY_ICON_SIZE (~×2.67).
lv_obj_t* make_play_triangle(lv_obj_t* parent) {
    lv_obj_t* canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(canvas, s_play_icon_buf,
                         PLAY_ICON_SIZE, PLAY_ICON_SIZE,
                         LV_IMG_CF_TRUE_COLOR_CHROMA_KEYED);
    // Pure green = the LVGL default chroma key → fully transparent.
    lv_canvas_fill_bg(canvas, lv_color_hex(0x00FF00), LV_OPA_COVER);

    // Triangle vertices: HTML (7,5)/(7,19)/(19,12) ×2.67 → (19,13)/(19,51)/(51,32).
    lv_point_t pts[3] = {
        { 19, 13 },   // top-left corner of the flat back edge
        { 19, 51 },   // bottom-left corner
        { 51, 32 },   // right apex (vertically centred)
    };
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = lv_color_white();
    dsc.bg_opa   = LV_OPA_COVER;
    lv_canvas_draw_polygon(canvas, pts, 3, &dsc);

    lv_obj_center(canvas);
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);
    return canvas;
}

void apply_paused_visual(ArtState* s, bool paused) {
    if (!s || !s->art) return;
    // Dim the entire art object (and its children) when paused.
    // lv_obj_set_style_opa works on any lv_obj; img_recolor only works on lv_img.
    // When M5 replaces s->art with an lv_img, img_recolor can be added back for
    // the colour-desaturation effect; the opa dim still applies on top of that.
    lv_obj_set_style_opa(s->art, paused ? LV_OPA_50 : LV_OPA_COVER, LV_PART_MAIN);
    // Show / hide the centred play-triangle overlay.
    // paused_overlay is a child of wrap (not s->art) so it is unaffected by the opa change.
    if (paused) lv_obj_clear_flag(s->paused_overlay, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_add_flag(s->paused_overlay,   LV_OBJ_FLAG_HIDDEN);
}

void set_volume_visual(ArtState* s, int volume) {
    if (!s) return;
    if (volume < 0)   volume = 0;
    if (volume > 100) volume = 100;
    mock_data::set_media_volume(volume);
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

// Seek gesture — bound to the timeline overlay so touches in the bottom 46 px
// of the art are handled here exclusively and do NOT bubble to on_art_gesture.
// Drags update the fill/thumb/label live; release emits the seek command stub
// (M5 fires KeyboardCommand{op:"seek", arg:new_pos}).
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
        lv_obj_set_width(s->tl_fill, rel_x);
        lv_obj_set_pos(s->tl_thumb,
            TL_BAR_PAD + rel_x - TL_THUMB_SZ / 2,
            8 - (TL_THUMB_SZ - TL_BAR_H) / 2);
        char buf[8];
        fmt_time(buf, sizeof(buf), new_pos);
        lv_label_set_text(s->tl_cur_label, buf);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        // Finalise: M5 emits KeyboardCommand{op:"seek", arg:new_pos} over BLE.
        char buf[8];
        fmt_time(buf, sizeof(buf), new_pos);
        lv_label_set_text(s->tl_cur_label, buf);
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
        s->start_x = p.x;
        s->start_y = p.y;
        s->start_volume = mock_data::media().volume;
        s->tracking = true;
        s->vertical_engaged = false;
    } else if (code == LV_EVENT_PRESSING) {
        if (!s->tracking) return;
        int dx = p.x - s->start_x;
        int dy = p.y - s->start_y;
        if (!s->vertical_engaged && abs(dy) > V_SWIPE_ENGAGE && abs(dy) > abs(dx)) {
            s->vertical_engaged = true;
            show_hud(s, true);
        }
        if (s->vertical_engaged) {
            // Negative dy (swipe up) = louder; positive dy = quieter.
            int new_vol = s->start_volume + (-dy) * V_SENS_NUM / V_SENS_DEN;
            set_volume_visual(s, new_vol);
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (!s->tracking) return;
        s->tracking = false;
        int dx = p.x - s->start_x;
        int dy = p.y - s->start_y;
        int abs_dx = abs(dx);
        int abs_dy = abs(dy);

        if (s->vertical_engaged) {
            // Hide HUD a beat after release. (In M5, fire vol_set command here.)
            // For now hide immediately — M8 adds the fade animation.
            show_hud(s, false);
            s->vertical_engaged = false;
            return;
        }
        if (abs_dx < TAP_MAX && abs_dy < TAP_MAX) {
            // Tap = play/pause toggle.
            bool playing = !mock_data::media_playing();
            mock_data::set_media_playing(playing);
            apply_paused_visual(s, !playing);
        } else if (abs_dx > H_SWIPE_MIN && abs_dx > abs_dy) {
            // Horizontal swipe — prev (left) / next (right). M5 fires KeyboardCommand.
        }
    }
}

lv_obj_t* make_art_block(lv_obj_t* parent, ArtState* s) {
    // Outer wrapper — fixed size, used as the gesture surface.
    lv_obj_t* wrap = lv_obj_create(parent);
    lv_obj_set_size(wrap, ART_W, ART_H);
    lv_obj_set_style_pad_all(wrap, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(wrap, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(wrap, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_radius(wrap, 14, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(wrap, true, LV_PART_MAIN);
    lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(wrap, LV_OBJ_FLAG_CLICKABLE);

    const auto& m        = mock_data::media();
    const bool has_media = m.has_media;

    // Album art — a solid colored object styled with a gradient that
    // approximates the HTML prototype's mock gradient. In M5 this becomes
    // an lv_img driven by the Media Album Art JPEG bytes.
    s->art = lv_obj_create(wrap);
    lv_obj_set_size(s->art, ART_W, ART_H);
    lv_obj_align(s->art, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(s->art, 14, LV_PART_MAIN);
    lv_obj_set_style_border_width(s->art, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s->art, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s->art, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(s->art, LV_GRAD_DIR_VER, LV_PART_MAIN);
    if (has_media) {
        // Mock "vibrant album cover" gradient — sunset palette mirroring
        // the HTML prototype.
        lv_obj_set_style_bg_color(s->art, theme::color(0x6E2456), LV_PART_MAIN);
        lv_obj_set_style_bg_grad_color(s->art, theme::color(0xF5B042), LV_PART_MAIN);
    } else {
        // Empty state — muted dark gradient + brand mark drawn on top.
        lv_obj_set_style_bg_color(s->art, theme::color(0x1A1F28), LV_PART_MAIN);
        lv_obj_set_style_bg_grad_color(s->art, theme::color(0x0C0F14), LV_PART_MAIN);
    }
    lv_obj_set_style_shadow_color(s->art, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s->art, 32, LV_PART_MAIN);
    lv_obj_set_style_shadow_ofs_y(s->art, 12, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(s->art, LV_OPA_60, LV_PART_MAIN);
    lv_obj_clear_flag(s->art, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s->art, LV_OBJ_FLAG_CLICKABLE);

    // Ori brand mark as the "Nothing playing" placeholder (drawn from
    // primitives — outer ring + inner solid disc, accent gold).
    if (!has_media) {
        lv_obj_t* ring = lv_obj_create(s->art);
        lv_obj_set_size(ring, 96, 96);
        lv_obj_center(ring);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_color(ring, theme::color(theme::COLOR_ACCENT), LV_PART_MAIN);
        lv_obj_set_style_border_width(ring, 3, LV_PART_MAIN);
        lv_obj_set_style_pad_all(ring, 0, LV_PART_MAIN);
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* dot = lv_obj_create(s->art);
        lv_obj_set_size(dot, 30, 30);
        lv_obj_center(dot);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(dot, theme::color(theme::COLOR_ACCENT), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    }

    // Centred play-triangle overlay shown while paused. Drawn via lv_canvas
    // because LV_SYMBOL_PLAY (FontAwesome U+F04B) is not present in our
    // custom Montserrat fonts — see make_play_triangle() for the why.
    s->paused_overlay = make_play_triangle(wrap);

    // Volume HUD — vertical fill bar + percentage text, on the right edge.
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
    lv_obj_set_style_bg_opa(bar_bg, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar_bg, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar_bg, 0, LV_PART_MAIN);
    lv_obj_clear_flag(bar_bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(bar_bg, LV_OBJ_FLAG_CLICKABLE);

    // HUD bar fill (anchored to bottom, height = volume %).
    s->hud_fill = lv_obj_create(bar_bg);
    lv_obj_set_size(s->hud_fill, lv_pct(100), lv_pct(mock_data::media().volume));
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
        lv_snprintf(buf, sizeof(buf), "%d%%", mock_data::media().volume);
        lv_label_set_text(s->hud_pct_label, buf);
    }
    lv_obj_set_style_text_color(s->hud_pct_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s->hud_pct_label, theme::font_meta(), 0);
    lv_obj_align(s->hud_pct_label, LV_ALIGN_TOP_MID, 0, -28);

    // Timeline bar — sits on top of everything in wrap (last child = highest
    // z-order) so it stays at full brightness even when the art dims on pause.
    // Hidden when nothing is playing OR the active app doesn't support seeking
    // (MediaMetadata.can_seek = false). tl_fill/tl_thumb/tl_cur_label stay
    // null in that case so on_seek_gesture's early-out guard is always correct.
    if (has_media && m.can_seek) {
        const uint32_t pos_s  = m.position_s;
        const uint32_t dur_s  = m.duration_s > 0 ? m.duration_s : 1;
        const int16_t  bar_w  = ART_W - TL_BAR_PAD * 2;
        const int16_t  fill_w = (int16_t)((int32_t)bar_w * (int32_t)pos_s / (int32_t)dur_s);

        lv_obj_t* overlay = lv_obj_create(wrap);
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
    }

    // Gesture handlers — bound to the wrapper so the art + overlays move
    // as one when LVGL dispatches events.
    lv_obj_add_event_cb(wrap, on_art_gesture, LV_EVENT_PRESSED,     s);
    lv_obj_add_event_cb(wrap, on_art_gesture, LV_EVENT_PRESSING,    s);
    lv_obj_add_event_cb(wrap, on_art_gesture, LV_EVENT_RELEASED,    s);
    lv_obj_add_event_cb(wrap, on_art_gesture, LV_EVENT_PRESS_LOST,  s);

    // Apply initial paused-state visual.
    apply_paused_visual(s, !mock_data::media_playing());
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

    const auto& m = mock_data::media();
    const bool has = m.has_media;

    // LV_LABEL_LONG_DOT in LVGL 8.x truncates only when the label's HEIGHT
    // is constrained to a single line — otherwise the label grows to a
    // second line and the text wraps. Set both width AND a one-line height
    // for the title/artist to get the desired ellipsis behaviour.
    // Media title — font_title() (24 px) to match the meeting-list title size.
    s->title_label = lv_label_create(meta);
    lv_obj_set_size(s->title_label, META_W, 28);   // font_title = 24 px → one-line box (tight)
    lv_label_set_long_mode(s->title_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(s->title_label, has ? m.title : "Nothing playing");
    lv_obj_set_style_text_color(s->title_label,
        theme::color(has ? theme::COLOR_TEXT_PRIMARY : theme::COLOR_TEXT_TERTIARY), 0);
    lv_obj_set_style_text_font(s->title_label, theme::font_title(), 0);
    lv_obj_set_style_text_align(s->title_label, LV_TEXT_ALIGN_CENTER, 0);

    // Artist — font_meta() (22 px) to match meeting-location and profile-title.
    s->artist_label = lv_label_create(meta);
    lv_obj_set_size(s->artist_label, META_W, 26);  // font_meta = 22 px → one-line box
    lv_label_set_long_mode(s->artist_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(s->artist_label, has ? m.artist : "—");
    lv_obj_set_style_text_color(s->artist_label, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(s->artist_label, theme::font_meta(), 0);
    lv_obj_set_style_text_align(s->artist_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(s->artist_label, 2, 0);

    return meta;
}

// User-assignable shortcut buttons. M5 wires tap → KeyboardCommand{op:"shortcut", arg:N}.
lv_obj_t* make_shortcuts_row(lv_obj_t* parent) {
    lv_obj_t* row = lv_obj_create(parent);
    // Row box height = button height (82) + internal pad_top (8).
    lv_obj_set_size(row, lv_pct(100), 90);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_top(row, 8, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 14, 0);

    const auto* slots = mock_data::shortcuts();
    for (size_t i = 0; i < mock_data::SHORTCUT_COUNT; ++i) {
        lv_obj_t* btn = lv_obj_create(row);
        // Width: (484px available − 2×14px gap) ÷ 3 = 152px fills the row.
        lv_obj_set_size(btn, 152, 82);
        lv_obj_set_style_radius(btn, 14, LV_PART_MAIN);
        lv_obj_set_style_bg_color(btn, theme::color(theme::COLOR_CARD), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(btn, theme::color(theme::COLOR_DIVIDER), LV_PART_MAIN);
        lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
        lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

        // Placeholder glyph — capital letter from the icon token, centred.
        lv_obj_t* glyph = lv_label_create(btn);
        const char* token = slots[i].icon_token;
        char letter[2] = { (char) (token && token[0] ? (token[0] - 'a' + 'A') : '?'), 0 };
        lv_label_set_text(glyph, letter);
        lv_obj_set_style_text_color(glyph, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
        lv_obj_set_style_text_font(glyph, theme::font_h2(), 0);
        lv_obj_center(glyph);
    }
    return row;
}

} // namespace

namespace screen_keyboard_mode {

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
    //   10 (pad_top)  +  216 (art h)  +  10 (meta pad_top)  +  28 (title box)  +
    //   2 (artist pad_top)  +  26 (artist box)  +  8 (shortcuts pad_top)  +
    //   82 (shortcuts)  +  10 (pad_bottom)  =  392  ✓ (4 px headroom)
    // Art is now 384 × 216 (16:9). The seek overlay is inside the art (zero
    // extra vertical cost). Art width (384) < usable panel (484) — 50 px each side.
    // pad_bottom = pad_top = 10 so the art's top gap mirrors the buttons' bottom gap.
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
    make_shortcuts_row(left);

    ui::make_panel_divider(body);

    // Right profile card.
    widget_profile_card::create(body);

    // Store the ArtState pointer and free it when the screen is destroyed.
    lv_obj_set_user_data(screen, state);
    lv_obj_add_event_cb(screen, [](lv_event_t* e) {
        delete static_cast<ArtState*>(lv_event_get_user_data(e));
    }, LV_EVENT_DELETE, state);
    return screen;
}

} // namespace screen_keyboard_mode
