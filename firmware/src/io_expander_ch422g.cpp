#include "io_expander_ch422g.h"

#include <Arduino.h>
#include <Wire.h>

// CH422G I²C protocol — verified against the WCH datasheet and the ESPHome
// CH422G component (esphome/components/ch422g/ch422g.cpp).
//
// The chip is unusual: rather than a single device address with sub-register
// bytes, it exposes each register as its own I²C device address. The "device
// address" we put on the wire is therefore the register selector itself, and
// the only data byte transmitted is the register value.
//
//   0x24  Mode register     (write 0x01 for push-pull output mode on IO0..7)
//   0x38  Output register   (write 8-bit mask -> EXIO0..EXIO7 levels)
//   0x23  Output upper bits (OC0..3 — unused on this board)
//
// On the Waveshare ESP32-S3 Touch LCD 4.3" board the device is hardwired to
// I²C base 0x24 with no address strapping pins; the "0x24" we keep referring
// to is the mode register selector, which doubles as the base address.

namespace {

constexpr uint8_t REG_MODE      = 0x24;
constexpr uint8_t REG_OUT_LOWER = 0x38;

constexpr uint8_t MODE_OUTPUT_PP = 0x01;  // push-pull output on IO0..IO7

// RAM shadow of the current EXIO0..EXIO7 levels. The chip is write-only in
// output mode (reading would require flipping to input mode), so we maintain
// the level state here and read-modify-write against it.
uint8_t shadow_mask = 0x00;

bool i2c_write_byte(uint8_t dev_addr, uint8_t value) {
    Wire.beginTransmission(dev_addr);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

} // namespace

namespace ch422g {

bool init() {
    // Configure IO0..IO7 as push-pull outputs. Without this the pins default
    // to high-impedance and EXIO writes have no observable effect.
    bool mode_ok = i2c_write_byte(REG_MODE, MODE_OUTPUT_PP);

    // Sensible defaults: de-assert both reset lines, hold backlight off (so
    // backlight::init() can raise it deliberately at the saved level — no
    // white flash). Bit positions match EXIOn numbering.
    //   bit 1 = TP_RST  -> HIGH (released)
    //   bit 2 = LCD_BL  -> LOW  (off until backlight::init() raises it)
    //   bit 3 = LCD_RST -> HIGH (released; lcd_panel::init() will pulse it)
    shadow_mask = (1u << 1) | (1u << 3);
    bool out_ok = i2c_write_byte(REG_OUT_LOWER, shadow_mask);

    Serial.printf("[ch422g] init mode=%s out=%s mask=0x%02X\n",
                  mode_ok ? "ok" : "FAIL",
                  out_ok  ? "ok" : "FAIL",
                  shadow_mask);
    return mode_ok && out_ok;
}

bool write_output(uint8_t pin, bool high) {
    if (pin > 7) return false;
    uint8_t bit = (uint8_t)(1u << pin);
    uint8_t next = high ? (uint8_t)(shadow_mask | bit)
                        : (uint8_t)(shadow_mask & ~bit);
    if (next == shadow_mask) return true;  // no-op, save a bus transaction

    if (!i2c_write_byte(REG_OUT_LOWER, next)) return false;
    shadow_mask = next;
    return true;
}

bool set_all_outputs(uint8_t mask) {
    if (!i2c_write_byte(REG_OUT_LOWER, mask)) return false;
    shadow_mask = mask;
    return true;
}

uint8_t get_output_state() {
    return shadow_mask;
}

} // namespace ch422g
