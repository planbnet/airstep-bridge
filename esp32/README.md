# AIRSTEP Bridge — ESP32-H2

Standalone hardware version of the [macOS/iPadOS bridge](../README.md): an
**ESP32-H2-DevKitM-1** pretends to be a Positive Grid Spark 40 amp over Bluetooth
LE, an **XSONIC AIRSTEP Spk Edition** connects to it in its stock Mode 1, and every
footswitch press comes back out as **5-pin DIN MIDI** — straight into the MIDI IN of
a Line 6 HX Stomp.

No computer in the signal path, no Bluetooth HID pairing, no Spark amp.

## Hardware

| | |
|---|---|
| Board | ESP32-H2-DevKitM-1 (ESP32-H2-MINI-1; BLE 5.2 + 802.15.4, **no Wi-Fi**) |
| MIDI out | external optocoupler MIDI board on **GPIO10** + GND ([wiring](doc/verkabelung.html)) |
| Status LED | onboard WS2812 on GPIO8 |
| Console | native USB-Serial/JTAG port (the one marked *ESP32-H2 USB*) |

Only MIDI OUT is used. Leave the MIDI IN side of the optocoupler board unconnected —
this is a one-way street, pedal → HX Stomp.

### Why GPIO10

The H2 breaks out GPIO 0–5, 8–14 and 22–27 on its headers, and most of those are
already busy: 2/3/8/9/25 are strapping pins, 2–5 are JTAG, 13/14 carry the
32.768 kHz crystal, 23/24 are UART0 to the USB-UART bridge, and 26/27 are the
native USB data lines. GPIO10 is free, as are 11, 12 and 22 if you need to move it
— change `MIDI_TX_PIN` in [config.h](src/config.h).

31250 baud is exact here: the H2 runs off a 32 MHz crystal and 32000000/31250 is
1024, so the UART divider is integral and there is no baud error to accumulate.

## Build and flash

```bash
cd esp32 && pio run --target upload && pio device monitor
```

The **official `platformio/espressif32` platform cannot build this.** Every release
through 7.x still ships Arduino core 2.0.17 / IDF 4.4, and it has no ESP32-H2 board
definition at all. H2 support arrived in arduino-esp32 3.0.0, which for PlatformIO
only the [pioarduino](https://github.com/pioarduino/platform-espressif32) fork
packages — that is what [platformio.ini](platformio.ini) points at.

Two other version constraints that are not optional:

- **NimBLE-Arduino ≥ 2.3.8.** The H2 Arduino core enables NimBLE itself (Bluedroid
  is not built for this chip), and older versions crash on init or leak memory in
  exactly that configuration. 2.5.1 also fixes building against Arduino 3.3.11,
  which is what the pioarduino stable channel currently ships.
- **No FastLED, no Adafruit NeoPixel.** Neither supports the H2. The WS2812 is
  driven through the core's built-in `rgbLedWrite()`, which uses RMT — present on
  the H2 — and needs no dependency at all.

The build sets `ARDUINO_USB_MODE=1` and `ARDUINO_USB_CDC_ON_BOOT=1` so the console
appears on the native USB port. Without them `Serial` is UART0 on GPIO23/24 and only
reachable through the *other* USB port, so the monitor would sit silent. Delete both
flags if you prefer the UART bridge port; MIDI is on UART1 either way and never
collides with the console.

## Testing without a MIDI board

The decoder — 7-bit unpacking, the effect state flag, frame reassembly — runs on
the host with no hardware at all:

```bash
c++ -std=c++17 -I src test/host_decode.cpp src/spark_decode.cpp -o /tmp/t && /tmp/t
```

To check the actual bytes on the wire before you have anything to plug into, set
`MIDI_LOOPBACK_RX_PIN` in [config.h](src/config.h) to `11` and bridge **GPIO10 to
GPIO11** with a jumper. The firmware then reads back its own output and hexdumps it
on every press:

```
[A] preset 0 -> CC 49 = 127 ch 1
      loopback: B0 31 7F
```

`B0` is CC on channel 1, `31` is 49, `7F` is 127. Set the pin back to `-1` for
normal operation.

## Usage

1. Turn the real Spark amp **off**, if you own one — otherwise the pedal connects to
   it instead.
2. Power the ESP32.
3. Power on the AIRSTEP **holding footswitch A** to force Mode 1.
4. The LED goes solid when the pedal connects.
5. Set the HX Stomp to listen on the right MIDI channel
   (*Global Settings → MIDI/Tempo*) and add a looper block.

The pedal reconnects on its own after an ESP32 restart or a brief dropout. There is
no pairing state on either side.

## Status LED

| State | LED |
|---|---|
| Booting | white, solid |
| Advertising, waiting for the pedal | white, slowly blinking |
| Connected | solid — white until the first press, then the colour of the last switch pressed |
| Footswitch pressed | brief dip to dark, then the new colour |
| Error | red, solid |

Switch colours are in [config.h](src/config.h): A green, B cyan, C blue, D magenta,
E amber (dimmed while the pedal reports "off"). Red is reserved for errors.

## MIDI mapping

Per switch you can configure message type (Note / CC / Program Change), number and
channel in the `SWITCHES` table in [config.h](src/config.h) — the compile-time
equivalent of the Swift app's settings screen.

The default mapping drives the HX Stomp's 1 Switch Looper, all on channel 1:

| Switch | Sends | Effect |
|---|---|---|
| A | CC 49 = 127 | record / stop / overdub — taps the footswitch the block sits on |
| A *(after a stop)* | CC 60 = 127 | starts a new loop, discarding the old one |
| B | CC 63 = 127 | undo / redo |
| C | CC 69 = 8 | next snapshot |
| D | CC 62 = 127 | play once |
| E | CC 61 = 127 / 0 | play / stop, following the pedal's state |

Two things here are not obvious.

**A taps a footswitch instead of sending a looper CC.** None of the looper CCs can
cycle: CC 60 encodes the action in the value, so 127 always means "record" and it
never stops. Tapping the footswitch the block sits on hands the Record → Play →
Overdub cycle back to the HX Stomp, which is the only party that knows what state
the looper is in. This costs nothing, because a looper block cannot be added
without assigning it to a footswitch in the first place. Point A at CC 50 or CC 51
if you put the block on a different switch.

**There is no clear-loop command, so deleting is not a button.** Clearing is
press-and-hold on the hardware, and CC 49–53 emulate a tap only — verified on the
unit, where three messages with values 0, 127 and 127 advanced the looper exactly
three steps, so the value is ignored and a hold cannot be expressed. Instead, the
bridge uses the one thing it knows for certain: switch E carries a state the pedal
itself tracks, so after a stop via E, A starts a new loop rather than advancing the
cycle. Recording over a stopped loop discards it, so deleting becomes what already
happens when you record again.

Both share one limitation with switch E: stop the looper from the HX Stomp's own
controls and the bridge does not hear about it, so one press goes to the wrong
place before things line up again.

CC numbers are from the HX Stomp 3.80 Owner's Manual, p. 65–66.
[doc/midi.html](doc/midi.html) has the full table, the Program Change mapping, and
two errata in the Line 6 manual.

### Not USB

The HX Stomp's USB port is a device port, so reaching it over USB would mean being
a USB *host*. The ESP32-H2 cannot: its USB is a fixed-function USB-Serial/JTAG
controller with no OTG and no TinyUSB, so it cannot even present itself as a USB
MIDI device. DIN MIDI is the only route from this chip — an ESP32-S3 with the
`usb_host` stack could do it, at considerably more effort.

### Switch E is different

A–D are pure triggers. E is the pedal's drive toggle, so it carries an on/off state
that the pedal tracks itself and shows on its own footswitch LED. Two options exist
for it:

- `followToggle` — same CC number, value 127 for on and 0 for off.
- `splitToggle` — two *different* message numbers for on and off, so a device that
  only understands triggers can still bind them to separate actions.

Either way the pedal's LED becomes a status indicator for whatever E controls. The
catch is that the state belongs to the pedal: change the same thing from the HX
Stomp's own switches and the pedal's LED will be out of step until the next press.

## Protocol

The BLE side is unchanged from the [Swift implementation](../README.md#protocol-notes),
which is where it was reverse-engineered. The parts that matter for this port:

| UUID | Role |
|---|---|
| `FFC0` | Spark service |
| `FFC1` | write — pedal → "amp" |
| `FFC2` | notify — "amp" → pedal |

**Advertising is just a name and a service UUID:** local name `Spark 40 BLE` plus
`FFC0`. No manufacturer data, no bonding. That is the whole handshake the AIRSTEP
needs to accept the ESP32 as its amp.

`FFC2` is never notified on. It exists because the pedal subscribes to it, and it
answers reads with an empty value — same as the macOS bridge.

Frame layout:

```
01 fe 00 00 53 fe <len> 00*9              16-byte block header
f0 01 <seq> <chk> <cmd> <sub> <payload> f7
```

The payload is 7-bit packed: groups of 8 bytes where the first carries the high bit
of the following 7. A useful consequence is that **every payload byte is ≤ 0x7F**,
so `0xF0` and `0xF7` are unambiguous delimiters. That is what
[spark_decode.cpp](src/spark_decode.cpp) relies on: it scans a rolling buffer for
those two bytes, which skips the block header for free and reassembles a frame
split across two ATT writes instead of decoding it as nothing — or as a second
press.

### Mode 1 mapping

| Switch | Frame | Meaning |
|---|---|---|
| A | `01 38` … `00` | hardware preset 0 |
| B | `01 38` … `01` | hardware preset 1 |
| C | `01 38` … `02` | hardware preset 2 |
| D | `01 38` … `03` | hardware preset 3 |
| E | `01 15` + `"Booster"` | drive on/off |

The effect on/off flag is the last `0x43` (on) / `0x42` (off) in the *packed*
payload. It sits after the effect name, so a `B`/`C` inside the name cannot be
mistaken for it.

### Things worth knowing

- **The pedal reports amp state, not switch identity.** `01 38` carries the
  *resulting* preset, so a switch with a relative function (Channel ± in Mode 2/3)
  emits a different value on every press. Only Mode 1 gives stable per-switch
  triggers — hence Mode 1 only, deliberately.
- **Handshake queries are ignored.** On connect the pedal asks `02 10` (current
  preset number) and `02 01` (current preset). Not answering costs nothing for
  presets, but the pedal then does not know which effect model is loaded — in modes
  other than 1 that makes one press of an effect switch produce an `01 15` for
  *every* model in the family, around 14 frames. Mode 1 sends a single frame.
  Bursts are coalesced within 300 ms regardless.
- **`03 00 00 00 4x`** frames are a chunk counter, not input. Ignored.
- MTU is raised to 255 so a frame fits in one write. The reassembler would cope
  without it, but there is no reason to make it work.

## Layout

```
platformio.ini              pioarduino platform, board and USB CDC flags
doc/verkabelung.html        wiring diagram, and why the 5V pin gets 3.3V
doc/midi.html               MIDI mapping and the HX Stomp CC reference
test/host_decode.cpp        decoder tests, run on the host
src/config.h                pins, timing, per-switch MIDI mapping and LED colours
src/main.cpp                setup/loop, debouncing, orchestration
src/ble_spark_emulator.*    NimBLE GATT server, advertising, Spark impersonation
src/spark_decode.*          frame reassembly, 7-bit unpacking, classification
src/midi_out.*              UART serial MIDI
src/status_led.*            WS2812 status display
```

Decoding runs on the NimBLE host task and finished events go through a queue, so
MIDI and the LED are only ever touched from `loop()`.

## Not implemented, on purpose

- MIDI IN from the HX Stomp back to the pedal.
- AIRSTEP modes 2 and 3 (relative values — see above).
- Wi-Fi, OTA, web config. The H2 has no Wi-Fi radio to begin with.
