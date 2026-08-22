#include "midi_out.h"

#include <Arduino.h>
#include <stdio.h>

#include "spark_decode.h"

namespace midi {

static HardwareSerial& port = Serial1;

void begin() {
    // RX is normally unused: the bridge is a one-way street, pedal -> HX Stomp.
    // It is only wired up for the loopback self-test.
    port.begin(MIDI_BAUD, SERIAL_8N1, MIDI_LOOPBACK_RX_PIN, MIDI_TX_PIN);
}

/// Reads back whatever the loopback jumper returned and hexdumps it, so the
/// bytes on the wire can be checked against the message that was meant to go out.
static void drainLoopback() {
    if (MIDI_LOOPBACK_RX_PIN < 0) return;
    // 3 bytes at 31250 baud take ~1 ms; give the last one time to land.
    delay(3);
    if (!port.available()) { Serial.println("      loopback: nothing received"); return; }
    char hex[64];
    size_t n = 0;
    while (port.available() && n < sizeof(hex) - 4)
        n += snprintf(hex + n, sizeof(hex) - n, "%02X ", port.read());
    Serial.printf("      loopback: %s\n", hex);
}

static void raw2(uint8_t status, uint8_t a) {
    const uint8_t buf[2] = {status, a};
    port.write(buf, 2);
}

static void raw3(uint8_t status, uint8_t a, uint8_t b) {
    const uint8_t buf[3] = {status, a, b};
    port.write(buf, 3);
}

void fire(uint8_t index, Tristate state, bool looperStopped, char* desc,
          size_t descCap) {
    if (index >= 5) { snprintf(desc, descCap, "?"); return; }
    const SwitchConfig& c = SWITCHES[index];
    if (!c.enabled) { snprintf(desc, descCap, "disabled"); return; }

    const uint8_t chan = (c.channel < 1 ? 1 : (c.channel > 16 ? 16 : c.channel)) - 1;

    // A stopped looper takes the alternative sequence where one is configured.
    const MidiStep* seq = c.seq;
    uint8_t seqLen = c.seqLen;
    const char* seqTag = "";
    if (looperStopped && c.seqWhenStopped != nullptr) {
        seq = c.seqWhenStopped;
        seqLen = c.seqWhenStoppedLen;
        seqTag = " (new loop)";
    }

    // A sequence replaces the single message entirely. The pauses block the main
    // loop, which is fine at these lengths: BLE runs on its own task so nothing is
    // dropped, presses queue up behind it, and the only visible cost is the status
    // LED updating a fraction of a second late.
    if (seq != nullptr && seqLen > 0) {
        for (uint8_t i = 0; i < seqLen; ++i) {
            const MidiStep& st = seq[i];
            if ((st.status & 0xF0) == 0xC0)
                raw2(st.status | chan, st.d1);
            else
                raw3(st.status | chan, st.d1, st.d2);
            port.flush();
            if (st.waitAfterMs) delay(st.waitAfterMs);
        }
        if (seqLen == 1 && (seq[0].status & 0xF0) == 0xB0)
            snprintf(desc, descCap, "CC %u = %u ch %u%s", seq[0].d1, seq[0].d2, chan + 1,
                     seqTag);
        else
            snprintf(desc, descCap, "sequence, %u steps%s", seqLen, seqTag);
        return;
    }

    // With splitToggle, "off" gets its own message number so the HX Stomp can
    // bind on and off to separate actions. Falls back to the single number when
    // the pedal did not report a state.
    const bool isOff = c.splitToggle && state == Tristate::Off;
    uint8_t n = isOff ? c.offNumber : c.number;
    if (n > 127) n = 127;
    const char* tag = c.splitToggle ? (isOff ? " (off)" : " (on)") : "";

    switch (c.kind) {
        case MsgKind::Note:
            raw3(0x90 | chan, n, 0x7F);
            raw3(0x80 | chan, n, 0x00);
            snprintf(desc, descCap, "Note %u ch %u%s", n, chan + 1, tag);
            break;

        case MsgKind::CC: {
            const uint8_t v = (c.followToggle && !c.splitToggle)
                                  ? (state == Tristate::Off ? 0 : 127)
                                  : c.value;
            raw3(0xB0 | chan, n, v);
            snprintf(desc, descCap, "CC %u = %u ch %u%s", n, v, chan + 1, tag);
            break;
        }

        case MsgKind::Program:
            raw2(0xC0 | chan, n);
            snprintf(desc, descCap, "Program %u ch %u%s", n, chan + 1, tag);
            break;
    }

    // Serial MIDI is 320 us per byte; flushing keeps the "sent" log line and the
    // LED blip honest rather than reporting bytes still sitting in the FIFO.
    port.flush();
    drainLoopback();
}

}  // namespace midi
