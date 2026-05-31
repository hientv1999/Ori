#include "backlight.h"

#include <Arduino.h>

#include "io_expander_ch422g.h"
#include "pins.h"

// Backlight is always ON. CH422G EXIO2 high = panel visible. No runtime control.

namespace backlight {

void init() {
    ch422g::write_output(ORI_CH422G_EXIO_LCD_BL, true);
    Serial.println("[backlight] on");
}

} // namespace backlight
