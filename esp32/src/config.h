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

/// One message of a multi-step sequence, plus the pause that follows it.
/// `status` is the message type only -- the channel comes from the SwitchConfig.
struct MidiStep {
    uint8_t status;  // 0xB0 control change, 0xC0 program change
    uint8_t d1;
    uint8_t d2;  // unused for program change
    uint16_t waitAfterMs;
};

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
    /// When set, the switch sends this whole sequence and every message field
    /// above is ignored. Used where one HX Stomp function needs several messages.
    const MidiStep* seq;
    uint8_t seqLen;
    /// Sent instead, while the looper is known to be stopped -- that is, after
    /// switch E last reported "off". Only switch A uses this.
    const MidiStep* seqWhenStopped;
    uint8_t seqWhenStoppedLen;
    /// Program change sent by the *very first* press after power-up, in place of
    /// this switch's normal message: it puts the HX Stomp on the preset the rig
    /// lives in before anything else happens. NO_PRESET leaves the switch alone.
    ///
    /// It is per switch, so which preset you land on depends on which one you step
    /// on first -- one switch can mean "take me to the looper", another "take me to
    /// the clean rig". Only the first press does this; the rest behave normally.
    uint8_t initialPreset;
};

static constexpr uint8_t NO_PRESET = 0xFF;

/// HX Stomp preset numbering: 42 banks of three, mapped 1:1 onto program changes
/// 0..125 with no bank select at all. Bank b (1-based) with slot A/B/C is
/// (b - 1) * 3 + 0/1/2. Set Global Settings > Displays > Preset Number to 000-125
/// and the display shows exactly this number.
static constexpr uint8_t preset(uint8_t bank, uint8_t slot) {
    return (bank - 1) * 3 + slot;
}
static constexpr uint8_t SLOT_A = 0, SLOT_B = 1, SLOT_C = 2;

/// The preset holding the looper block. Requires MIDI PC Rx to be on, under
/// Global Settings > MIDI/Tempo.
static constexpr uint8_t LOOPER_PRESET = preset(36, SLOT_B);  // 36B = PC 106

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

// Not looper related, but the other thing worth a footswitch. Values 0, 1 and 2
// pick a snapshot directly; 8 steps to the next and 9 to the previous.
//
// The HX Stomp has *three* snapshots. The 3.80 manual's CC table lists a value of 3
// for "Snapshot 4" -- that row is copied from the HX Stomp XL, which has four. Do
// not send 3 to a plain HX Stomp.
static constexpr uint8_t CC_SNAPSHOT = 69;
static constexpr uint8_t SNAPSHOT_NEXT = 8;
static constexpr uint8_t SNAPSHOT_PREV = 9;

// Emulated footswitch presses: CC 49..53 act like a tap on FS1..FS5.
//
// This is how switch A gets the native Record -> Play -> Overdub cycle. None of
// the looper CCs above can do it: CC 60 encodes the *action* in the value, so 127
// always means "record" no matter what the looper is currently doing, and it never
// stops. Tapping the footswitch the block lives on hands the cycle back to the
// HX Stomp, which is the only party that knows what state the looper is in.
//
// A looper block cannot even be added without assigning it to a footswitch, so
// this costs nothing that was not already spent.
static constexpr uint8_t CC_FS1 = 49;   // the switch the looper block sits on
static constexpr uint8_t CC_FS2 = 50;
static constexpr uint8_t CC_FS3 = 51;

// Red is reserved for errors, so the switch colours avoid it.
#define COL_GREEN 0x00FF00
#define COL_CYAN 0x00B4FF
#define COL_BLUE 0x2828FF
#define COL_MAGENTA 0xFF00C8
#define COL_AMBER 0xFF8C00

// Starting a genuinely new loop. Switch A sends this instead of a footswitch tap
// while the looper is known to be stopped.
//
// There is no "clear loop" command on the HX Stomp at all: clearing is
// press-and-hold on the hardware, and CC 49..53 emulate a tap only -- verified on
// the unit, where three messages with values 0, 127, 127 advanced the looper
// exactly three steps, so the value is ignored and a hold cannot be expressed.
//
// Rather than fake a delete, this leans on the fact that recording over a *stopped*
// loop discards it. After a stop via switch E, the next press of A starts a new
// loop instead of advancing the cycle. Deleting stops being a button you press and
// becomes what already happens when you record again.
static constexpr MidiStep RESTART_SEQ[] = {
    {0xB0, CC_LOOPER_RECORD, 127, 0},
};

// Mapping for a 1 Switch Looper block placed at the end of an ordinary preset.
// One switch per looper function, in the order the pedal lays them out.
//
// Switch A taps the footswitch the block sits on rather than sending a looper CC,
// because only that gives the Record -> Play -> Overdub cycle; see CC_FS1 above.
// The one exception is a stopped looper, where it starts a new loop instead --
// see RESTART_SEQ.
// Move A to CC_FS2 or CC_FS3 if you put the looper block on a different switch.
//
// Switch E drives Play/Stop as a real toggle: the pedal tracks that state itself
// and lights its own footswitch LED, so the pedal LED becomes the playback
// indicator.
//
// Fields: enabled, kind, number, channel, value, followToggle, splitToggle,
//         offNumber, color, colorOff, seq, seqLen, seqWhenStopped, len,
//         initialPreset.
static constexpr SwitchConfig SWITCHES[5] = {
    // A: record / stop / overdub -- the native cycle, via a footswitch tap.
    {true, MsgKind::CC, CC_FS1, 1, 127, false, false, 0, COL_GREEN, COL_GREEN, nullptr, 0,
     RESTART_SEQ, 1, LOOPER_PRESET},
    // B: undo / redo the last overdub.
    {true, MsgKind::CC, CC_LOOPER_UNDO, 1, 127, false, false, 0, COL_CYAN, COL_CYAN, nullptr, 0, nullptr, 0,
     LOOPER_PRESET},
    // C: step to the next snapshot. Snapshots switch gaplessly and leave the looper
    // alone -- its bypass state is deliberately not snapshot-controlled -- so a loop
    // keeps playing while the sound around it changes.
    {true, MsgKind::CC, CC_SNAPSHOT, 1, SNAPSHOT_NEXT, false, false, 0, COL_BLUE,
     COL_BLUE, nullptr, 0, nullptr, 0, LOOPER_PRESET},
    // D: play the loop exactly once, then stop.
    {true, MsgKind::CC, CC_LOOPER_ONCE, 1, 127, false, false, 0, COL_MAGENTA, COL_MAGENTA, nullptr, 0, nullptr, 0,
     LOOPER_PRESET},
    // E: play / stop, following the pedal's own on/off state (127 = play).
    {true, MsgKind::CC, CC_LOOPER_PLAY, 1, 127, true, false, 0, COL_AMBER, 0x301400, nullptr, 0, nullptr, 0,
     LOOPER_PRESET},
};
