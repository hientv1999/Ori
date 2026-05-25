#pragma once

#include <lvgl.h>

namespace modal_profile {

// Full-screen profile detail overlay. Opened by tapping the profile photo.
// Shows name, title, email, and phone. Closed only via the Close button.
// Long-press (factory reset) is unaffected — it fires on the photo object
// itself before this modal exists.
//
// ref_photo: the live calendar-mode photo widget. When non-null its absolute
// Y coordinate is read from the LVGL layout engine so the overlay photo lands
// at exactly the same screen position. Pass nullptr to fall back to y=0.
lv_obj_t* create(lv_obj_t* base_screen, lv_obj_t* ref_photo = nullptr);

} // namespace modal_profile
