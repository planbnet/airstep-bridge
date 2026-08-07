#pragma once
#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "spark_decode.h"  // Tristate

// Serial MIDI on UART1 -> optocoupler board -> DIN-5 -> HX Stomp MIDI IN.
// Output only; the board's MIDI IN stays unconnected.

namespace midi {

void begin();

/// Sends the message configured for `index` (0..4 = A..E).
///
/// `state` is the pedal's reported on/off state and only matters for switch E:
/// it selects the CC value with followToggle, or the message number with
/// splitToggle. Writes a human-readable description of what was sent into
/// `desc`, for the log.
void fire(uint8_t index, Tristate state, char* desc, size_t descCap);

}  // namespace midi
