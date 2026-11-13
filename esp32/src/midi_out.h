#pragma once
#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "spark_decode.h"  // Tristate

// Serial MIDI on UART1 -> optocoupler board -> DIN-5 -> HX Stomp MIDI IN.
//
// Two sources share this port now: footswitch presses decoded from the pedal,
// and messages arriving over BLE-MIDI from a tablet or laptop. They cannot be
// allowed to interleave their bytes, and neither may block the other, so output
// goes through a ring buffer that pump() drains into the UART as fast as 31250
// baud allows. Messages are enqueued whole or not at all.
//
// Producers are the NimBLE host task and loop(); the buffer is mutex-protected.
// Only pump(), on loop(), ever touches the UART.

namespace midi {

void begin();

/// Enqueues a locally generated message -- a footswitch press or the power-up
/// preset select. Goes out the DIN port and, with MIRROR_PRESSES_TO_BLE_MIDI,
/// is also handed to the tap so the MIDI host hears it.
/// Returns false if the buffer was too full to take the whole message.
bool emit(const uint8_t* bytes, size_t len);

/// Enqueues a message received from the MIDI host. DIN only -- never offered to
/// the tap, which would echo it straight back to where it came from.
bool forward(const uint8_t* bytes, size_t len);

/// Receives every message emit() sends, for mirroring onto another transport.
using Tap = void (*)(const uint8_t* msg, size_t len);
void setTap(Tap tap);

/// Moves queued bytes into the UART and releases any sequence step that has come
/// due. Non-blocking; call from loop().
void pump();

/// Bytes currently waiting to go out, for diagnostics.
size_t pending();
/// Messages dropped because the buffer was full, since boot.
uint32_t drops();

/// Sends the message configured for `index` (0..4 = A..E).
///
/// `state` is the pedal's reported on/off state and only matters for switch E:
/// it selects the CC value with followToggle, or the message number with
/// splitToggle.
///
/// `looperStopped` says we last told the HX Stomp to stop, which lets switch A
/// start a new loop rather than advance the looper's cycle.
///
/// Writes a human-readable description of what was sent into `desc`, for the log.
void fire(uint8_t index, Tristate state, bool looperStopped, char* desc, size_t descCap);

/// Sends a bare program change. Used for the initial preset selection, which is
/// not tied to any switch's configured message.
void program(uint8_t number, uint8_t channel);

}  // namespace midi
