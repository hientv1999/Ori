#pragma once

// Backlight control — always ON. The enable line is CH422G EXIO2 (digital-only).
// init() turns it on once at boot; there is no runtime control.
namespace backlight {

void init();

} // namespace backlight
