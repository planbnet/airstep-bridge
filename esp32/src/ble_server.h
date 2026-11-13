#pragma once
#include <stdint.h>

// Owns the NimBLE stack, the GATT server and advertising. Two services hang off
// it and neither knows about the other:
//
//   spark    the Positive Grid Spark 40 emulation the AIRSTEP connects to
//   blemidi  standard BLE-MIDI, for a tablet or laptop
//
// Both are served to separate centrals at the same time, which works because
// NimBLE allows three simultaneous connections by default.
//
// The advertising arrangement is the interesting part. A legacy advertisement
// holds 31 bytes, and the Spark identity already spends 21 of them: flags (3),
// the complete local name "Spark 40 BLE" (14) and the 16-bit service UUID FFC0
// (4). The BLE-MIDI service UUID is 128-bit and needs 18 more, so it cannot
// join them. It goes in the scan response instead, which has its own 31 bytes.
//
// That split is deliberate rather than arbitrary: the pedal's view of the
// advertisement stays byte-for-byte what it has always been, so the half that
// works today cannot be broken by the half being added. The MIDI host is the
// side that has to do an active scan to see the whole picture -- which every
// host that implements BLE-MIDI does, since it is scanning for that service.

namespace ble {

/// Brings up the stack, both services and advertising. False means the LED
/// goes red.
bool begin();

/// Peers currently connected: the pedal, a MIDI host, or both.
uint8_t peerCount();

}  // namespace ble
