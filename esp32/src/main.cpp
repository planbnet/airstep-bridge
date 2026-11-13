#include <Arduino.h>

#include "ble_midi.h"
#include "ble_server.h"
#include "ble_spark_emulator.h"
#include "config.h"
#include "midi_out.h"
#include "status_led.h"

// AIRSTEP Spk Edition -> ESP32-H2 -> 5-pin DIN MIDI -> Line 6 HX Stomp.
//
// The pedal only knows how to talk to a Positive Grid Spark 40 amp, so the
// ESP32 pretends to be one: it advertises the Spark name and service, the pedal
// connects as if it had found its amp, and every footswitch press arrives as a
// write on FFC1. Presses are decoded and re-emitted as serial MIDI.
//
// Only Mode 1 is supported (power the pedal on while holding footswitch A).
// The other modes report *relative* changes, so a switch emits a different
// value on every press and cannot be mapped to a stable trigger.
//
// With BLE_MIDI_ENABLED the same radio also serves standard BLE-MIDI, so a
// tablet or laptop can drive the HX Stomp through the same DIN cable -- a sheet
// music app sending a Program Change as you open a song is the case this was
// built for. The two roles are independent: either can be connected without the
// other, and the pedal's half behaves exactly as it did before.
//
// Port of https://github.com/planbnet/airstep-bridge (macOS/iPadOS, Swift).

static const char* NAMES[5] = {"A", "B", "C", "D", "E"};

static uint32_t lastEffectAt = 0;
static uint32_t lastPresetAt[4] = {0, 0, 0, 0};
static Tristate driveState = Tristate::Unknown;
/// True while the last thing we told the HX Stomp was "stop". Only switch E can
/// establish this, because only E carries a state the pedal itself tracks -- which
/// makes it the one thing we know for certain about the looper. Switch A uses it to
/// start a new loop instead of stepping the cycle, and that is what stands in for
/// the delete command the HX Stomp does not have.
static bool looperStopped = false;
/// Spent by the first footswitch press after power-up, which selects the preset the
/// rig lives in instead of doing its normal job. Scoped to boot rather than to the
/// BLE connection on purpose: a brief radio dropout mid-song must not turn the next
/// press into a surprise preset jump.
static bool firstPressPending = true;
static bool errored = false;

void setup() {
    Serial.begin(115200);
    delay(300);  // let the host attach to USB CDC before the first log line

    led::begin();
    led::setState(led::State::Booting);
    led::tick();

    midi::begin();
    Serial.printf("[midi] UART1 TX on GPIO%d, %lu baud\n", MIDI_TX_PIN,
                  (unsigned long)MIDI_BAUD);

#if BLE_MIDI_ENABLED
    // Everything the bridge sends to the HX Stomp is also offered to the MIDI
    // host, so a footswitch can turn the page in a sheet music app while it
    // drives the looper. Messages arriving *from* the host are forwarded with
    // midi::forward(), which skips the tap and cannot echo back.
    if (MIRROR_PRESSES_TO_BLE_MIDI) midi::setTap(blemidi::send);
#endif

    if (!ble::begin()) {
        errored = true;
        led::setState(led::State::Error);
        Serial.println("[ble] init failed");
        return;
    }
    led::setState(led::State::Waiting);
}

static void handle(const SwitchEvent& e) {
    const uint32_t now = millis();

    if (e.index < 4) {
        // A-D send one frame per press in Mode 1, so this only swallows BLE
        // retransmissions -- short enough to leave a deliberate double-tap alone.
        if (now - lastPresetAt[e.index] <= PRESET_DEBOUNCE_MS) return;
        lastPresetAt[e.index] = now;
    } else {
        // Effect frames can arrive in bursts. Every frame extends the window, so
        // a burst of any length collapses into its first frame.
        const bool continuation = now - lastEffectAt <= EFFECT_BURST_MS;
        lastEffectAt = now;
        if (continuation) return;
        if (e.state != Tristate::Unknown) driveState = e.state;
    }

    const SwitchConfig& cfg = SWITCHES[e.index];

    if (firstPressPending) {
        firstPressPending = false;
        if (cfg.initialPreset != NO_PRESET) {
            midi::program(cfg.initialPreset, cfg.channel);
            led::setSwitchColor(cfg.color);
            led::blip();
            Serial.printf("[%s] first press -> Program %u ch %u\n", NAMES[e.index],
                          cfg.initialPreset, cfg.channel);
            return;
        }
    }

    const Tristate state = e.index == 4 ? driveState : Tristate::Unknown;

    char desc[48];
    midi::fire(e.index, state, looperStopped, desc, sizeof(desc));

    if (e.index == 0) looperStopped = false;                    // A set it running
    if (e.index == 4) looperStopped = state == Tristate::Off;   // E stopped/started

    led::setSwitchColor(state == Tristate::Off ? cfg.colorOff : cfg.color);
    led::blip();

    if (e.index == 4)
        Serial.printf("[%s] %s %s -> %s\n", NAMES[e.index], e.name,
                      state == Tristate::On ? "on" : (state == Tristate::Off ? "off" : "?"),
                      desc);
    else
        Serial.printf("[%s] preset %u -> %s\n", NAMES[e.index], e.index, desc);
}

void loop() {
    if (errored) {
        led::tick();
        delay(20);
        return;
    }

    static bool wasConnected = false;
    const bool isConnected = spark::connected();
    if (isConnected != wasConnected) {
        wasConnected = isConnected;
        led::setState(isConnected ? led::State::Connected : led::State::Waiting);
    }

    SwitchEvent e;
    while (spark::pop(e)) handle(e);

    // Moves queued MIDI into the UART. Both the pedal and the MIDI host feed
    // that queue, and this is the only place the port is written.
    midi::pump();

    // A full output buffer means more MIDI arrived than 31250 baud can carry,
    // which is worth knowing about but not worth a line per message.
    static uint32_t lastDrops = 0;
    const uint32_t d = midi::drops();
    if (d != lastDrops) {
        Serial.printf("[midi] output buffer full, %lu messages dropped\n",
                      (unsigned long)d);
        lastDrops = d;
    }

    led::tick();
    delay(2);
}
