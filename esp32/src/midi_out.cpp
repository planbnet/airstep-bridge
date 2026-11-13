#include "midi_out.h"

#include <Arduino.h>
#include <stdio.h>

#include "spark_decode.h"

namespace midi {

static HardwareSerial& port = Serial1;

// ---------------------------------------------------------------- ring buffer

static uint8_t ring[MIDI_TX_BUFFER];
static size_t head = 0;  // next write
static size_t tail = 0;  // next read
static SemaphoreHandle_t lock = nullptr;
static uint32_t dropped = 0;
static Tap tap = nullptr;

static inline size_t usedLocked() {
    return (head + MIDI_TX_BUFFER - tail) % MIDI_TX_BUFFER;
}

/// Appends a whole message or nothing. Partial writes would put a truncated
/// message on the wire, which is worse than a dropped one: the HX Stomp would
/// read the next message's status byte as this one's missing data byte.
static bool push(const uint8_t* bytes, size_t len) {
    if (len == 0) return true;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (MIDI_TX_BUFFER - 1 - usedLocked() < len) {
        ++dropped;
        xSemaphoreGive(lock);
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        ring[head] = bytes[i];
        head = (head + 1) % MIDI_TX_BUFFER;
    }
    xSemaphoreGive(lock);
    return true;
}

// ---------------------------------------------------------------- sequences

// A configured sequence with a pause between its steps cannot block loop() any
// more, so the remainder is held here and released by pump() as it comes due.
// One sequence at a time: they are footswitch-triggered and a few milliseconds
// long, and a press that lands mid-sequence is better off replacing it than
// queueing behind it.
static const MidiStep* pendingSteps = nullptr;
static uint8_t pendingLen = 0;
static uint8_t pendingNext = 0;
static uint8_t pendingChan = 0;
static uint32_t pendingDueAt = 0;

static void emitStep(const MidiStep& st, uint8_t chan) {
    if ((st.status & 0xF0) == 0xC0) {
        const uint8_t buf[2] = {(uint8_t)(st.status | chan), st.d1};
        emit(buf, 2);
    } else {
        const uint8_t buf[3] = {(uint8_t)(st.status | chan), st.d1, st.d2};
        emit(buf, 3);
    }
}

static void startSequence(const MidiStep* steps, uint8_t len, uint8_t chan) {
    emitStep(steps[0], chan);
    if (len > 1) {
        pendingSteps = steps;
        pendingLen = len;
        pendingNext = 1;
        pendingChan = chan;
        pendingDueAt = millis() + steps[0].waitAfterMs;
    } else {
        pendingSteps = nullptr;
    }
}

static void releaseDueSteps() {
    while (pendingSteps && (int32_t)(millis() - pendingDueAt) >= 0) {
        const MidiStep& st = pendingSteps[pendingNext];
        emitStep(st, pendingChan);
        pendingDueAt = millis() + st.waitAfterMs;
        if (++pendingNext >= pendingLen) pendingSteps = nullptr;
    }
}

// ---------------------------------------------------------------- loopback

/// Reads back whatever the loopback jumper returned and hexdumps it, so the
/// bytes on the wire can be checked against the message that was meant to go
/// out. Output is no longer tied to a single press -- the UART drains
/// asynchronously now -- so this reports whatever has arrived since last time.
static void drainLoopback() {
    if (MIDI_LOOPBACK_RX_PIN < 0 || !port.available()) return;
    char hex[64];
    size_t n = 0;
    while (port.available() && n < sizeof(hex) - 4)
        n += snprintf(hex + n, sizeof(hex) - n, "%02X ", port.read());
    Serial.printf("      loopback: %s\n", hex);
}

// ---------------------------------------------------------------- public API

void begin() {
    lock = xSemaphoreCreateMutex();
    // RX is normally unused: the bridge is a one-way street, pedal -> HX Stomp.
    // It is only wired up for the loopback self-test.
    port.begin(MIDI_BAUD, SERIAL_8N1, MIDI_LOOPBACK_RX_PIN, MIDI_TX_PIN);
}

void setTap(Tap t) { tap = t; }

bool emit(const uint8_t* bytes, size_t len) {
    if (tap) tap(bytes, len);
    return push(bytes, len);
}

bool forward(const uint8_t* bytes, size_t len) { return push(bytes, len); }

void pump() {
    releaseDueSteps();

    xSemaphoreTake(lock, portMAX_DELAY);
    // availableForWrite() keeps this from blocking: at 31250 baud a byte takes
    // 320 us, and stalling loop() on a full FIFO would stall the status LED and
    // the event queue with it.
    while (tail != head && port.availableForWrite() > 0) {
        port.write(ring[tail]);
        tail = (tail + 1) % MIDI_TX_BUFFER;
    }
    xSemaphoreGive(lock);

    drainLoopback();
}

size_t pending() {
    xSemaphoreTake(lock, portMAX_DELAY);
    const size_t n = usedLocked();
    xSemaphoreGive(lock);
    return n;
}

uint32_t drops() { return dropped; }

static uint8_t channelOf(uint8_t channel) {
    return (channel < 1 ? 1 : (channel > 16 ? 16 : channel)) - 1;
}

void program(uint8_t number, uint8_t channel) {
    const uint8_t buf[2] = {(uint8_t)(0xC0 | channelOf(channel)),
                            (uint8_t)(number > 127 ? 127 : number)};
    emit(buf, 2);
}

void fire(uint8_t index, Tristate state, bool looperStopped, char* desc,
          size_t descCap) {
    if (index >= 5) { snprintf(desc, descCap, "?"); return; }
    const SwitchConfig& c = SWITCHES[index];
    if (!c.enabled) { snprintf(desc, descCap, "disabled"); return; }

    const uint8_t chan = channelOf(c.channel);

    // A stopped looper takes the alternative sequence where one is configured.
    const MidiStep* seq = c.seq;
    uint8_t seqLen = c.seqLen;
    const char* seqTag = "";
    if (looperStopped && c.seqWhenStopped != nullptr) {
        seq = c.seqWhenStopped;
        seqLen = c.seqWhenStoppedLen;
        seqTag = " (new loop)";
    }

    // A sequence replaces the single message entirely.
    if (seq != nullptr && seqLen > 0) {
        startSequence(seq, seqLen, chan);
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
        case MsgKind::Note: {
            const uint8_t on[3] = {(uint8_t)(0x90 | chan), n, 0x7F};
            const uint8_t off[3] = {(uint8_t)(0x80 | chan), n, 0x00};
            emit(on, 3);
            emit(off, 3);
            snprintf(desc, descCap, "Note %u ch %u%s", n, chan + 1, tag);
            break;
        }

        case MsgKind::CC: {
            const uint8_t v = (c.followToggle && !c.splitToggle)
                                  ? (state == Tristate::Off ? 0 : 127)
                                  : c.value;
            const uint8_t buf[3] = {(uint8_t)(0xB0 | chan), n, v};
            emit(buf, 3);
            snprintf(desc, descCap, "CC %u = %u ch %u%s", n, v, chan + 1, tag);
            break;
        }

        case MsgKind::Program: {
            const uint8_t buf[2] = {(uint8_t)(0xC0 | chan), n};
            emit(buf, 2);
            snprintf(desc, descCap, "Program %u ch %u%s", n, chan + 1, tag);
            break;
        }
    }
}

}  // namespace midi
