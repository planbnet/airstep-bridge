#pragma once
#include <stdint.h>

// Pin assignment, timing and the per-switch MIDI mapping. Everything a user is
// expected to change lives in this file.

// ---------------------------------------------------------------- BLE identity

// What the AIRSTEP looks for. The pedal has exactly one partner it knows how to
// talk to, a Positive Grid Spark 40, and finds it by advertised name plus the
// Spark service UUID -- there is no bonding and no manufacturer data involved.
// Values taken from the working macOS/iPadOS implementation.
#define SPARK_ADV_NAME "Spark 40 BLE"

static constexpr uint16_t SPARK_SERVICE_UUID = 0xFFC0;
static constexpr uint16_t SPARK_WRITE_UUID = 0xFFC1;   // pedal -> "amp"
static constexpr uint16_t SPARK_NOTIFY_UUID = 0xFFC2;  // "amp" -> pedal (unused)

// ---------------------------------------------------------------- MIDI hardware

// UART1 TX -> optocoupler MIDI board -> DIN-5 -> HX Stomp MIDI IN.
//
// GPIO10 is broken out on header J3 and carries no competing duty: it is not a
// strapping pin (2/3/8/9/25), not JTAG (2..5), not the 32.768 kHz crystal
// (13/14), not UART0 to the USB-UART bridge (23/24) and not the native
// USB-Serial/JTAG data lines (26/27). GPIO11, 12 and 22 are equally safe if you
// need to move it. Wire GPIO10 to the MIDI board's input and share GND.
static constexpr int MIDI_TX_PIN = 10;

// Standard MIDI rate. The H2 runs off a 32 MHz crystal and 32000000/31250 is
// exactly 1024, so the UART divider is integral and there is no baud error.
static constexpr uint32_t MIDI_BAUD = 31250;

// Loopback self-test, for checking the MIDI byte stream without a MIDI board or
// anything to plug into: jumper MIDI_TX_PIN to this pin and the firmware reads
// back its own output, hexdumping it to the console on every press. Set to -1
// for normal operation -- with a MIDI board attached this pin is unused anyway,
// but leaving it enabled costs a UART RX pin for nothing.
static constexpr int MIDI_LOOPBACK_RX_PIN = -1;  // e.g. 11 to test

// ---------------------------------------------------------------- timing

// Switch E is the pedal's drive toggle. Without answering the pedal's handshake
// queries it does not know which effect model is loaded, which in modes other
// than 1 makes one press emit an 01 15 frame per model in the family. Mode 1
// sends a single frame, but coalescing costs nothing and keeps a burst from
// turning into a burst of MIDI.
static constexpr uint32_t EFFECT_BURST_MS = 300;

// A–D send one frame per press, so this is only a guard against BLE
// retransmissions -- deliberately short enough to let you double-tap a switch.
static constexpr uint32_t PRESET_DEBOUNCE_MS = 50;

// ---------------------------------------------------------------- status LED

// Addressable WS2812 on GPIO8 of the ESP32-H2-DevKitM-1, driven through the
// core's built-in rgbLedWrite() (RMT under the hood). Neither FastLED nor
// Adafruit NeoPixel supports the H2.
static constexpr uint8_t LED_BRIGHTNESS = 28;  // of 255; full scale is blinding
static constexpr uint32_t LED_BLINK_MS = 700;  // advertising: slow white blink
static constexpr uint32_t LED_BLIP_MS = 70;    // press: brief dip to dark

// ---------------------------------------------------------------- MIDI mapping

enum class MsgKind : uint8_t { Note, CC, Program };

struct SwitchConfig {
    bool enabled;
    MsgKind kind;
    uint8_t number;   // note / CC / program number, 0..127
    uint8_t channel;  // 1..16
    /// CC only: the value to send. Many HX Stomp functions encode the action in
    /// the value rather than the number -- CC 60 is Record at 64..127 and Overdub
    /// at 0..63 -- so two switches can share a number and differ only here.
    /// Ignored when followToggle is set. Unused for Note and Program Change.
    uint8_t value;
    /// CC only: send 127 or 0 following the pedal's toggle state instead of 127
    /// every time. Ignored when splitToggle is set.
    bool followToggle;
    /// Switch E only. The pedal keeps its own on/off state for the drive toggle
    /// and shows it on the footswitch LED. With this on, "on" and "off" are sent
    /// as two distinct message numbers so the HX Stomp can bind them to separate
    /// actions, and the pedal's LED becomes a status indicator.
    bool splitToggle;
    uint8_t offNumber;  // number used for "off"; only with splitToggle
    uint32_t color;     // status LED colour after this switch was pressed
    uint32_t colorOff;  // colour when the pedal reports "off" (switch E)
};

// HX Stomp looper CCs, from the "MIDI CC" table on p. 65-66 of the HX Stomp 3.80
// Owner's Manual (identical in 2.65). Note that several of these encode the action
// in the *value*, not the number.
//
// There is deliberately no CC_LOOPER_BLOCK here: CC 67 toggles the looper block on
// full Helix / Helix LT, but on the HX Stomp it is reserved and does nothing.
static constexpr uint8_t CC_LOOPER_RECORD = 60;   // 0..63 overdub, 64..127 record
static constexpr uint8_t CC_LOOPER_PLAY = 61;     // 0..63 stop, 64..127 play
static constexpr uint8_t CC_LOOPER_ONCE = 62;     // 64..127 play once
static constexpr uint8_t CC_LOOPER_UNDO = 63;     // 64..127 undo/redo
static constexpr uint8_t CC_TAP_TEMPO = 64;       // 64..127 tap
static constexpr uint8_t CC_LOOPER_REVERSE = 65;  // 0..63 forward, 64..127 reverse
static constexpr uint8_t CC_LOOPER_HALF = 66;     // 0..63 full, 64..127 half speed

// Red is reserved for errors, so the switch colours avoid it.
#define COL_GREEN 0x00FF00
#define COL_CYAN 0x00B4FF
#define COL_BLUE 0x2828FF
#define COL_MAGENTA 0xFF00C8
#define COL_AMBER 0xFF8C00

// Default mapping, aimed at the HX Stomp's 1 Switch Looper so the bridge is
// useful the moment it boots. A and B share CC 60 and differ only in value,
// which is how that CC distinguishes Record from Overdub.
//
// Switch E drives Play/Stop as a real toggle: the pedal tracks that state itself
// and lights its own footswitch LED, so the pedal LED becomes the playback
// indicator.
//
// Requires a *1 Switch Looper* block in the preset, assigned to a footswitch in
// Stomp mode -- the manual only documents these CCs for that block.
//
// Fields: enabled, kind, number, channel, value, followToggle, splitToggle,
//         offNumber, color, colorOff.
static constexpr SwitchConfig SWITCHES[5] = {
    // A: start recording a new loop.
    {true, MsgKind::CC, CC_LOOPER_RECORD, 1, 127, false, false, 0, COL_GREEN, COL_GREEN},
    // B: overdub onto it -- same CC, low value.
    {true, MsgKind::CC, CC_LOOPER_RECORD, 1, 0, false, false, 0, COL_CYAN, COL_CYAN},
    // C: play the loop exactly once.
    {true, MsgKind::CC, CC_LOOPER_ONCE, 1, 127, false, false, 0, COL_BLUE, COL_BLUE},
    // D: undo / redo the last overdub.
    {true, MsgKind::CC, CC_LOOPER_UNDO, 1, 127, false, false, 0, COL_MAGENTA, COL_MAGENTA},
    // E: play / stop, following the pedal's own on/off state (127 = play).
    {true, MsgKind::CC, CC_LOOPER_PLAY, 1, 127, true, false, 0, COL_AMBER, 0x301400},
};
