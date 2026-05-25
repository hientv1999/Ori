#pragma once

#include <lvgl.h>

// Ori — circular progress ring.
//
// Built on lv_arc. Used by:
//   - Step 3 "Orioning"            — indeterminate or %
//   - 5-min countdown modal        — % (5:00 → 0:00)
//   - Reconnect-Syncing overlay    — indeterminate
//   - OTA-Updating screen          — %
//
// The center label is a child of the ring container; callers can update it
// (e.g. "67%", "3:07") via set_label_text().

namespace widget_progress_ring {

// `size_px` is the outer diameter. Track + progress stroke width auto-scales.
lv_obj_t* create(lv_obj_t* parent, uint16_t size_px);

// Indeterminate mode: a fixed-length arc head rotates continuously. Used for
// pre-progress "we're working" states.
void set_indeterminate(lv_obj_t* ring, bool on);

// Determinate mode: progress goes from 0..100 clockwise from 12 o'clock.
void set_value(lv_obj_t* ring, uint8_t percent);

// Direct degree control (0..360) for sub-percent precision (e.g. countdown).
void set_angle(lv_obj_t* ring, uint16_t degrees);

// Replace the center label text. Pass nullptr to hide the label entirely.
void set_label_text(lv_obj_t* ring, const char* s);

void set_label_text_center(lv_obj_t* ring, const char* s);

// Optional sub-label below the main label (e.g. "UNTIL START").
void set_sub_label_text(lv_obj_t* ring, const char* s);

// Optional override for the center label font.
void set_label_font(lv_obj_t* ring, const lv_font_t* font);

} // namespace widget_progress_ring
