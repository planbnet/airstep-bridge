#include "status_led.h"

#include <Arduino.h>

#include "config.h"

#ifndef RGB_BUILTIN
#define RGB_BUILTIN 8  // ESP32-H2-DevKitM-1 wires the WS2812 to GPIO8
#endif

namespace led {

static State state = State::Off;
static uint32_t switchColor = 0xFFFFFF;  // white until a switch has been pressed
static uint32_t blipUntil = 0;
static uint32_t lastWrite = 0xFFFFFFFF;  // impossible value, forces the first write

static void write(uint32_t rgb) {
    if (rgb == lastWrite) return;  // RMT is not free; only push real changes
    lastWrite = rgb;
    const uint8_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    rgbLedWrite(RGB_BUILTIN, (r * LED_BRIGHTNESS) / 255, (g * LED_BRIGHTNESS) / 255,
                (b * LED_BRIGHTNESS) / 255);
}

void begin() {
    lastWrite = 0xFFFFFFFF;
    write(0);
}

void setState(State s) { state = s; }

void setSwitchColor(uint32_t rgb) { switchColor = rgb; }

void blip() { blipUntil = millis() + LED_BLIP_MS; }

void tick() {
    const uint32_t now = millis();

    // The press acknowledgement is a gap in whatever is currently shown, so it
    // reads the same way against a solid colour or a blink.
    if (blipUntil && (int32_t)(blipUntil - now) > 0) { write(0); return; }
    blipUntil = 0;

    switch (state) {
        case State::Off:
            write(0);
            break;
        case State::Booting:
            write(0xFFFFFF);
            break;
        case State::Waiting:
            write((now / LED_BLINK_MS) % 2 ? 0 : 0xFFFFFF);
            break;
        case State::Connected:
            write(switchColor);
            break;
        case State::Error:
            write(0xFF0000);
            break;
    }
}

}  // namespace led
