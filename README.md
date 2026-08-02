# AIRSTEP Bridge

Turns an **XSONIC AIRSTEP Spk Edition** into a general-purpose MIDI controller for
macOS, without a Spark amp and without Bluetooth HID pairing.

The Mac impersonates a Positive Grid Spark 40 over BLE. The pedal — running its
normal firmware in **Mode 1** — connects to it as if it were the amp and reports
footswitch presses. Those are decoded and re-emitted on a virtual CoreMIDI source
named `AIRSTEP Bridge`, which any DAW or looper can subscribe to.

## Why this exists

The Spk Edition's Looper Mode is meant to present itself as a Bluetooth HID
keyboard, but on this setup it never advertised for pairing — the pedal would not
appear in macOS Bluetooth settings at all. Rather than fight that, this reverses
the roles: the pedal keeps doing the one thing it does reliably, which is talking
to a Spark amp, and we pretend to be the amp.

## Build

```sh
./build.sh              # -> build/AIRSTEP Bridge.app
./build.sh --install    # also copies to /Applications
```

No dependencies beyond the Xcode command line tools. The app is ad-hoc signed, so
the first launch may need Right-click → Open, and macOS will ask for Bluetooth
permission.

## Usage

1. Turn the real Spark amp **off** — otherwise the pedal connects to it instead.
2. Start AIRSTEP Bridge.
3. Power on the AIRSTEP holding footswitch **A** to force **Mode 1**.
4. The status dot goes green when the pedal connects. Pads light up on each press.

Per switch you can configure message type (Note / CC / Program Change), number,
channel, and whether CC should follow the pedal's on/off state. Settings persist in
`UserDefaults`.

## Protocol notes

Reverse-engineered from the pedal's traffic, cross-checked against
[stangreg/Ignitron](https://github.com/stangreg/Ignitron), and verified against
captures labelled by hand with the actual switch pressed.

BLE service and characteristics of the Spark amp:

| UUID | Role |
|------|------|
| `FFC0` | Spark service |
| `FFC1` | write — client → amp |
| `FFC2` | notify — amp → client |

Frame layout:

```
01 fe 00 00 53 fe <len> 00*9              16-byte block header
f0 01 <seq> <chk> <cmd> <sub> <payload> f7
```

The payload is 7-bit packed: groups of 8 bytes where the first byte carries the
high bit of the following 7.

### Mode 1 mapping

| Switch | Frame | Meaning |
|--------|-------|---------|
| A | `01 38` … `00` | hardware preset 0 |
| B | `01 38` … `01` | hardware preset 1 |
| C | `01 38` … `02` | hardware preset 2 |
| D | `01 38` … `03` | hardware preset 3 |
| E | `01 15` + `"Booster"` | drive on/off |

The effect on/off flag is the last `0x43` (on) / `0x42` (off) in the packed
payload. It sits after the effect name, so a `B`/`C` inside the name cannot be
mistaken for it.

### Things worth knowing

- **The pedal reports amp state, not switch identity.** `01 38` carries the
  *resulting* preset, so a switch with a relative function (Channel ± in Mode 2/3)
  emits a different value on every press. Only Mode 1 gives stable per-switch
  triggers.
- **Handshake queries are ignored.** On connect the pedal asks `02 10` (current
  preset number) and `02 01` (current preset). Not answering them costs nothing for
  presets, but means the pedal does not know which effect model is loaded — in
  other modes one press of an effect switch then produces an `01 15` for *every*
  model in that family, ~14 frames. Mode 1 sends a single frame per press. Bursts
  are coalesced within 300 ms regardless.
- **`03 00 00 00 4x`** frames are a chunk counter, not input. Ignored.
- The pedal reconnects on its own after the app restarts.

## Layout

```
Sources/SparkBridgeApp.swift   SwiftUI app, BLE peripheral, MIDI output
Sources/decode.swift           Spark frame parsing and classification
Resources/Info.plist           bundle metadata, Bluetooth usage description
build.sh                       build and optional install
```
