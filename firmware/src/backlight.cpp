#include "backlight.h"

#include <Arduino.h>

#include "io_expander_ch422g.h"
#include "nvs_store.h"
#include "pins.h"

// Binary backlight: CH422G EXIO2 high = ON, low = OFF. EXIO2 is digital-only
// on the Waveshare ESP32-S3-Touch-LCD-4.3 board and also drives the panel
// DISP signal, so toggling it gates the whole visible output (not just the
// LED). The ESP32 continues to run while the backlight is off — only the
// visible LED is gated. See gestures.md / memory.md for the constraint.

namespace {

constexpr bool BACKLIGHT_DEFAULT_ON = true;

bool current_on = BACKLIGHT_DEFAULT_ON;

void apply_level(bool on) {
    ch422g::write_output(ORI_CH422G_EXIO_LCD_BL, on);
}

} // namespace

namespace backlight {

void init() {
    // Restore the saved state BEFORE the panel framebuffer has any content.
    // ch422g::init() leaves EXIO2 low (backlight off); one write here brings
    // it up to the user's saved state in time for lcd_panel::init() to draw
    // into a panel that's already at the right state. No white flash.
    current_on = nvs::load_backlight_on(BACKLIGHT_DEFAULT_ON);
    apply_level(current_on);

    Serial.printf("[backlight] init via ch422g.EXIO%d state=%s\n",
                  (int)ORI_CH422G_EXIO_LCD_BL,
                  current_on ? "on" : "off");
}

void set_on(bool on) {
    if (on == current_on) return;  // idempotent
    current_on = on;
    apply_level(current_on);
    nvs::save_backlight_on(current_on);  // debounced inside nvs_store
    Serial.printf("[backlight] %s\n", current_on ? "on" : "off");
}

bool is_on() {
    return current_on;
}

} // namespace backlight
