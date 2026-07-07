#pragma once
#include <stdint.h>

// CH422G 8-bit I/O expander driver.
//
// On the Waveshare ESP32-S3 Touch LCD 4.3" board the CH422G sits on the
// shared touch I²C bus (`Wire`, address 0x24) alongside the GT911 (0x5D)
// and carries three board-critical signals as outputs:
//
//   EXIO1 -> TP_RST   (GT911 reset)
//   EXIO2 -> LCD_BL   (backlight enable; tied to panel DISP — see backlight.cpp)
//   EXIO3 -> LCD_RST  (RGB panel reset)
//
// The chip is digital-only — no PWM. We use it purely as 8 binary outputs.
//
// Coexistence: this module never calls `Wire.begin()`. It relies on
// touch::init() to bring the bus up first, then issues writes against the
// existing transaction state. See main.cpp for the boot order.
//
// CH422G output register layout (per the chip datasheet and Waveshare
// example code): a single I²C transaction with one command byte sets all
// eight EXIOn levels simultaneously. We keep a RAM shadow of the last
// written byte so write_output(pin, level) can do a read-modify-write
// without needing to read back from the chip (which would require putting
// it into input mode — not what we want).

namespace ch422g {

// Initialise the driver. Requires `Wire.begin()` to have already been
// called by touch::init(). Pushes a known-good default mask to the chip:
//   - TP_RST   (EXIO1) HIGH (de-asserted)
//   - LCD_BL   (EXIO2) LOW  (backlight off — raised to ON by backlight::init())
//   - LCD_RST  (EXIO3) HIGH (de-asserted)
//   - All other EXIOn pins LOW.
//
// Returns true if the I²C write ACKed.
bool init();

// Drive a single EXIOn output. `pin` is 0..7. Performs a read-modify-write
// against the RAM shadow and pushes the updated byte to the chip in one
// I²C transaction. Returns true on I²C success.
bool write_output(uint8_t pin, bool high);

} // namespace ch422g
