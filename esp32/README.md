# AIRSTEP Bridge — ESP32-H2

Standalone hardware version of the [macOS/iPadOS bridge](../README.md): an
**ESP32-H2-DevKitM-1** pretends to be a Positive Grid Spark 40 amp over Bluetooth
LE, an **XSONIC AIRSTEP Spk Edition** connects to it in its stock Mode 1, and every
footswitch press comes back out as **5-pin DIN MIDI** — straight into the MIDI IN of
a Line 6 HX Stomp.

No computer in the signal path, no Bluetooth HID pairing, no Spark amp.

The same radio also serves standard **BLE-MIDI**, so the bridge doubles as an
ordinary wireless MIDI interface: a tablet running sheet music software can drive
the HX Stomp through the same DIN cable, and the preset follows the setlist. See
[Generic MIDI bridge](#generic-midi-bridge).

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

So does the BLE-MIDI packet parser — timestamps that look like status bytes,
running status, SysEx split across packets:

```bash
c++ -std=c++17 -I src test/host_blemidi.cpp src/ble_midi_parse.cpp -o /tmp/m && /tmp/m
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
6. Optional: on an iPad or Mac, open the app's Bluetooth MIDI picker and connect
   to **Spark 40 BLE**. The pedal does not have to be disconnected for this.

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

The LED tracks the *pedal* link only. A MIDI host connecting or leaving does not
change it, so a blinking LED still means exactly one thing: the pedal is not here.

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

**The first press after power-up selects a preset instead.** Whichever switch you
step on first sends a program change to the preset the looper lives in, and only
then does the rig behave normally. It is per switch, so different switches can lead
to different presets. Scoped to boot rather than to the BLE connection on purpose: a
brief radio dropout mid-song must not turn the next press into a surprise preset
jump. Needs `MIDI PC Rx` on, under *Global Settings → MIDI/Tempo*.

Both the cycle and the restart share one limitation with switch E: stop the looper from the HX Stomp's own
controls and the bridge does not hear about it, so one press goes to the wrong
place before things line up again.

CC numbers are from the HX Stomp 3.80 Owner's Manual, p. 65–66.
[doc/midi.html](doc/midi.html) has the full table, the Program Change mapping, and
two errata in the Line 6 manual.

### Not USB

The HX Stomp's USB port is a device port, so reaching it over USB would mean being
a USB *host*. The ESP32-H2 cannot: its USB is a fixed-function USB-Serial/JTAG
controller with no OTG and no TinyUSB, so it cannot even present itself as a USB
MIDI device. DIN MIDI is the only wired route from this chip — an ESP32-S3 with
the `usb_host` stack could do it, at considerably more effort.

This is also why a tablet reaches the bridge over BLE-MIDI rather than a cable:
there is no cable it could use. It turns out to be the better end of the deal.

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

## Generic MIDI bridge

Alongside the Spark emulation the ESP32 serves standard **BLE-MIDI** — "MIDI over
Bluetooth Low Energy", Apple's specification as adopted by the MIDI Association,
and what every commercial wireless MIDI dongle speaks. It appears in the iOS
*Bluetooth MIDI Devices* sheet, in Audio MIDI Setup on a Mac, and in any app's
MIDI port list. Whatever is sent to it goes out the same DIN cable.

The case this was built for is sheet music software, which attaches MIDI to a
score and sends it when the song opens — so the HX Stomp's preset follows the
setlist instead of being wired into `LOOPER_PRESET`:

| App | Where |
|---|---|
| forScore | per-score MIDI, as Program Change, Song Select or raw hex |
| BandHelper / Set List Maker | MIDI presets, per song or per song part |
| MobileSheetsPro | per-song MIDI with a selectable output port |

Program Change, Control Change, Song Select, Note, pitch bend and SysEx all pass
through. Running status and SysEx split across packets are handled; real-time
messages are forwarded but not logged, because clock arrives 24 times a beat.

Presses travel the other way too. With `MIRROR_PRESSES_TO_BLE_MIDI` every message
the bridge sends the HX Stomp is also notified to the MIDI host, so a footswitch
can turn the page in a sheet music app while it still drives the looper. Messages
arriving *from* the host are never mirrored back to it.

Set `BLE_MIDI_ENABLED` to 0 in [config.h](src/config.h) to leave the firmware
exactly as it was.

Verified on hardware with macOS as the host and the pedal connected at the same
time:

- The device appears in the Bluetooth MIDI window as `Spark 40 BLE`, connects,
  negotiates MTU 255, subscribes, and survives macOS's opening 32-byte SysEx.
- Program Change, Control Change and Song Select arrive and reach the HX Stomp
  over DIN — a Program Change for the looper preset lands the unit on it.
- The pedal connects alongside it: `[ble] connected ... (2 peers)`, with all five
  switches decoding correctly and the first press still selecting the preset.
- Presses mirror back: one press of B produced `CC 63 = 127` on the DIN port and
  the same message on the Mac over BLE-MIDI.

One operational quirk, which cost an hour of misdiagnosis and is not this
firmware's doing: **macOS's Bluetooth MIDI session can go stale while the window
still offers "Trennen"**, and MIDI then stops flowing in both directions with no
disconnect on the device side. Clicking Trennen and then Verbinden restores it.
Worth ruling out before suspecting the bridge. Note also that console output can
lag a few seconds behind the event, so an empty log is not evidence of a lost
message until it has had time to arrive.

### Two peripherals, one radio

The pedal and the MIDI host both connect as centrals and are both served at once.
NimBLE allows three simultaneous connections by default, so that part needs no
configuration at all.

Advertising is the part that needed thought. A legacy advertisement carries 31
bytes and the Spark identity already spends 21: flags (3), the complete local name
`Spark 40 BLE` (14) and the 16-bit service UUID `FFC0` (4). The BLE-MIDI service
UUID is 128-bit — the profile has no short UUID registered with the Bluetooth SIG
— and needs 18 more. It does not fit.

So one of them has to go in the **scan response**, which has its own 31 bytes.
Which one is not a free choice, and the answer was settled on hardware:

**CoreBluetooth does not match its scan filter against scan-response data.** With
the name primary and the BLE-MIDI UUID in the scan response, macOS's Bluetooth
MIDI window found nothing: `MIDIServer` opened a CoreBluetooth session, came up
empty, and tore it down five seconds later, while the ESP32 never saw a
connection attempt. An active scan does merge advertisement and scan response
into one record for a device that *matched*, but the filter itself runs against
the primary PDU alone. CoreBluetooth is also iOS's Bluetooth stack, so this rules
the arrangement out for iPads just as much as for Macs.

The default is therefore the other way round, switchable with
`BLE_MIDI_UUID_IN_PRIMARY` in [config.h](src/config.h):

| | primary advertisement | scan response |
|---|---|---|
| `1` (default) | flags, BLE-MIDI UUID, `FFC0` — 25 bytes | name — 14 bytes |
| `0` | flags, name, `FFC0` — 21 bytes | BLE-MIDI UUID — 18 bytes |

`FFC0` stays primary either way, so the pedal can still find its amp by service
UUID; with the default it needs an active scan to read the name. Set the flag to
`0` if the pedal ever fails to connect — between the two halves, the MIDI one is
the one that can afford to lose. Both payload sizes are printed at boot.

Two consequences worth knowing:

- **The device shows up on the iPad as `Spark 40 BLE`**, because there is only one
  advertised name to go around. Separate names per identity means extended
  advertising with two advertising sets (`CONFIG_BT_NIMBLE_EXT_ADV`,
  `CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES=2`, instance 0 with
  `setLegacyAdvertising(true)` so the BLE 4.x pedal can still see it). Enabling
  that flag also removes `NimBLEAdvertising` from the library in favour of
  `NimBLEExtAdvertising`, so it is a rewrite of
  [ble_server.cpp](src/ble_server.cpp) rather than a switch to flip.
- **NimBLE stops advertising on a successful connection and does not restart it.**
  With two centrals that is fatal to whichever one is late: the pedal connects on
  power-up and the tablet is left scanning a band we have gone quiet on.
  [ble_server.cpp](src/ble_server.cpp) restarts advertising from `onConnect`
  until `BLE_MAX_PEERS` is reached.

### Sharing one UART

Two sources feeding one 31250-baud port cannot write to it directly, so output
goes through a ring buffer that `midi::pump()` drains from `loop()`. Messages are
enqueued whole or dropped whole — a truncated message is worse than a missing one,
because the HX Stomp would read the next message's status byte as this one's
missing data byte. Drops are counted and reported.

Nothing blocks any more: the `flush()` after every message and the `delay()`
between sequence steps are gone, and a sequence's pauses are released by `pump()`
as they come due. The loopback self-test reports whatever has arrived since the
last poll rather than being tied to a single press.

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
test/host_decode.cpp        Spark decoder tests, run on the host
test/host_blemidi.cpp       BLE-MIDI parser tests, run on the host
src/config.h                pins, timing, per-switch MIDI mapping and LED colours
src/main.cpp                setup/loop, debouncing, orchestration
src/ble_server.cpp          NimBLE stack, GATT server, dual advertising
src/ble_spark_emulator.*    the Spark service: impersonation and press decoding
src/spark_decode.*          frame reassembly, 7-bit unpacking, classification
src/ble_midi.*              the BLE-MIDI service: GATT glue and notifications
src/ble_midi_parse.*        BLE-MIDI packet decoding and encoding
src/midi_out.*              UART serial MIDI, ring buffer shared by both sources
src/status_led.*            WS2812 status display
```

Decoding runs on the NimBLE host task and finished events go through a queue, so
MIDI and the LED are only ever touched from `loop()`.

## Printing the docs

Both pages carry a print stylesheet, so a browser renders them straight to a usable
PDF — light palette regardless of theme, and no figures or table rows sliced across
a page break:

```bash
"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" --headless=new --no-pdf-header-footer --print-to-pdf=verkabelung.pdf file://$PWD/doc/verkabelung.html
```

The PDFs themselves are not checked in; they are generated from the HTML.

## Not implemented, on purpose

- MIDI IN from the HX Stomp. `Serial1` already has an RX pin plumbed for the
  loopback test and GPIO11/12/22 are free, so the wiring is most of the work. It
  would be worth having: with `MIDI PC Tx` on, the HX Stomp sends a Program Change
  whenever you change preset on the unit, and forwarding that to the tablet would
  let a sheet music app follow the rig instead of leading it. Note it would *not*
  fix the looper caveat above — the Stomp transmits preset changes, not looper
  state.
- AIRSTEP modes 2 and 3 (relative values — see above).
- Runtime configuration. The mapping is still compile-time, which a per-song setup
  will eventually outgrow; the natural answer on this chip is SysEx over the
  BLE-MIDI link that is now there, since the H2 has no Wi-Fi radio for a web UI.
- Wi-Fi, OTA, web config. The H2 has no Wi-Fi radio to begin with.
