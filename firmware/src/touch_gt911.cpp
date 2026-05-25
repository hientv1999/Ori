#include "touch_gt911.h"

#include <Arduino.h>
#include <Wire.h>

#include "io_expander_ch422g.h"
#include "pins.h"

namespace {

// GT911 register map (only what we touch here).
constexpr uint16_t REG_STATUS    = 0x814E;  // touch count + status flag
constexpr uint16_t REG_POINT1    = 0x814F;  // 8 bytes per point, 5 points (id,xl,xh,yl,yh,wl,wh,res)
constexpr uint8_t  POINT_STRIDE  = 8;

// Set by the INT-line ISR; cleared after poll() consumes a frame. Volatile
// because the ISR runs on a different context than loop().
volatile bool touch_pending = false;

void IRAM_ATTR on_touch_int() {
    touch_pending = true;
}

bool write_reg16(uint16_t reg, const uint8_t* data, size_t len) {
    Wire.beginTransmission(ORI_TOUCH_I2C_ADDR);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)(reg & 0xFF));
    if (data && len) Wire.write(data, len);
    return Wire.endTransmission() == 0;
}

bool read_reg16(uint16_t reg, uint8_t* out, size_t len) {
    Wire.beginTransmission(ORI_TOUCH_I2C_ADDR);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)(reg & 0xFF));
    if (Wire.endTransmission(false) != 0) return false;

    size_t got = Wire.requestFrom((int)ORI_TOUCH_I2C_ADDR, (int)len);
    if (got != len) return false;
    for (size_t i = 0; i < len; ++i) out[i] = Wire.read();
    return true;
}

void reset_sequence() {
    // GT911 address-select dance per datasheet: with INT held low during
    // RST release the chip latches I²C address 0x5D.
    //
    // On the Waveshare board TP_RST is wired to CH422G EXIO1, not a direct
    // GPIO — so the reset edges are driven via the expander. Wire must
    // already be initialised (touch::init does that before calling us) and
    // ch422g::init() must already have been issued.
    pinMode(ORI_TOUCH_INT_PIN, OUTPUT);

    // 1) Hold INT low and assert TP_RST.
    digitalWrite(ORI_TOUCH_INT_PIN, LOW);
    ch422g::write_output(ORI_CH422G_EXIO_TP_RST, false);
    delay(10);

    // 2) Release TP_RST while INT is still low — this is what latches the
    //    0x5D address selection.
    ch422g::write_output(ORI_CH422G_EXIO_TP_RST, true);
    delay(10);

    // 3) Release INT — chip drives it from here on.
    pinMode(ORI_TOUCH_INT_PIN, INPUT);
    delay(50);
}

} // namespace

namespace touch {

void init() {
    // Wire must come up first: both the GT911 and the CH422G live on it,
    // and the reset sequence below pokes TP_RST via the CH422G.
    Wire.begin(ORI_TOUCH_SDA_PIN, ORI_TOUCH_SCL_PIN);
    Wire.setClock(400000);

    // Bring the I/O expander up before issuing the GT911 reset — it owns
    // TP_RST on this board.
    bool exp_ok = ch422g::init();

    reset_sequence();

    // Probe the status register so init failures show up immediately in logs.
    uint8_t status = 0;
    bool ok = read_reg16(REG_STATUS, &status, 1);

    // Write display resolution (800x480) to GT911 config registers.
    // 0x8048-0x8049 = X max (LE), 0x804A-0x804B = Y max (LE).
    // Must read full config (184 bytes from 0x8047), patch, recalculate
    // checksum at 0x80FF, write back, then assert fresh flag at 0x8100.
    {
        constexpr uint16_t CFG_START = 0x8047;
        constexpr uint8_t  CFG_LEN   = 184;  // 0x8047..0x80FE inclusive
        uint8_t cfg[CFG_LEN] = {};
        if (read_reg16(CFG_START, cfg, CFG_LEN)) {
            cfg[1] = 0x20;  // X max low  (800 = 0x0320)
            cfg[2] = 0x03;  // X max high
            cfg[3] = 0xE0;  // Y max low  (480 = 0x01E0)
            cfg[4] = 0x01;  // Y max high
            cfg[5] = 0x01;  // touch points = 1 (we only use 1, but this is the minimum allowed)
            // Recalculate checksum over cfg[0..CFG_LEN-1].
            uint8_t cksum = 0;
            for (uint8_t i = 0; i < CFG_LEN; ++i) cksum += cfg[i];
            cksum = (~cksum + 1) & 0xFF;
            write_reg16(CFG_START, cfg, CFG_LEN);
            write_reg16(0x80FF, &cksum, 1);
            uint8_t fresh = 0x01;
            write_reg16(0x8100, &fresh, 1);
            Serial.println("[touch] GT911 resolution set to 800x480");
        } else {
            Serial.println("[touch] GT911 config read FAILED — resolution not set");
        }
    }

    // Hook the INT line so we only hit the I²C bus when the chip has something
    // to say. GT911 drives INT low when a new frame is ready.
    pinMode(ORI_TOUCH_INT_PIN, INPUT);
    touch_pending = true;  // do one read at startup to clear any stale latched frame
    attachInterrupt(digitalPinToInterrupt(ORI_TOUCH_INT_PIN),
                    on_touch_int, FALLING);

    Serial.printf("[touch] init addr=0x%02X sda=%d scl=%d int=%d rst=ch422g.EXIO%d expander=%s probe=%s\n",
                  (int)ORI_TOUCH_I2C_ADDR,
                  (int)ORI_TOUCH_SDA_PIN, (int)ORI_TOUCH_SCL_PIN,
                  (int)ORI_TOUCH_INT_PIN,
                  (int)ORI_CH422G_EXIO_TP_RST,
                  exp_ok ? "ok" : "FAIL",
                  ok ? "ok" : "FAIL");
}

// Cached last known frame — returned verbatim on quiet polls so we don't
// flap PRESSED→RELEASED→PRESSED→RELEASED while the user's finger is still
// down. See poll() for the full rationale.
TouchPoint last_frame[5] = {};
uint8_t    last_count    = 0;

uint8_t poll(TouchPoint out[5]) {
    // ISR-gated read: only hit the I²C bus when the GT911 INT line has pulsed.
    //
    // The GT911 drives INT FALLING once per ~20 ms frame in polling mode.
    // Our loop runs at ~100 Hz (10 ms). Between INT pulses, touch_pending is
    // false — return the cached state without touching the bus. This preserves
    // LVGL's continuous PRESSED signal during a sustained touch (last_count > 0
    // keeps returning until the GT911 reports 0) while eliminating idle I²C
    // transactions that would otherwise fire 5× per GT911 frame.
    if (!touch_pending) {
        for (uint8_t i = 0; i < 5; ++i) out[i] = last_frame[i];
        return last_count;
    }
    touch_pending = false;  // ack the edge before the read so a race re-arms it

    uint8_t status = 0;
    if (!read_reg16(REG_STATUS, &status, 1)) {
        // Bus error — hold last state to avoid flapping.
        for (uint8_t i = 0; i < 5; ++i) out[i] = last_frame[i];
        return last_count;
    }

    // Bit 7 (0x80) = "buffer ready". If clear, the GT911 hasn't
    // published a new frame since our last read. The finger may still
    // be down — return what we last saw.
    if ((status & 0x80) == 0) {
        for (uint8_t i = 0; i < 5; ++i) out[i] = last_frame[i];
        return last_count;
    }

    // Fresh frame.
    for (uint8_t i = 0; i < 5; ++i) {
        out[i].x = 0; out[i].y = 0; out[i].pressed = false;
    }

    uint8_t count = status & 0x0F;
    if (count > 5) count = 5;

    if (count > 0) {
        uint8_t buf[5 * POINT_STRIDE] = {0};
        if (read_reg16(REG_POINT1, buf, count * POINT_STRIDE)) {
            for (uint8_t i = 0; i < count; ++i) {
                const uint8_t* p = &buf[i * POINT_STRIDE];
                // bytes: [0]=track id, [1]=x low, [2]=x high, [3]=y low, [4]=y high,
                //        [5..6]=size, [7]=reserved (little-endian fields)
                uint16_t x = (uint16_t)p[1] | ((uint16_t)p[2] << 8);
                uint16_t y = (uint16_t)p[3] | ((uint16_t)p[4] << 8);
                out[i].x = x;
                out[i].y = y;
                out[i].pressed = true;
            }
        } else {
            count = 0;
        }
    }

    // Clear the status flag so the GT911 will publish the next frame.
    uint8_t clear = 0;
    write_reg16(REG_STATUS, &clear, 1);

    // Cache for the next quiet poll.
    for (uint8_t i = 0; i < 5; ++i) last_frame[i] = out[i];
    last_count = count;

    return count;
}

} // namespace touch
