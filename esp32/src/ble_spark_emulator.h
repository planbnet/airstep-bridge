#pragma once
#include <stdint.h>

#include "spark_decode.h"

// The Positive Grid Spark 40 emulation: the GATT service the AIRSTEP connects
// to and writes footswitch events into. The stack, the server and advertising
// belong to ble_server; this is only the service.
//
// The pedal writes footswitch events to FFC1 and subscribes to FFC2 -- which we
// never notify on, exactly as the macOS implementation does not. Reads are
// answered with an empty value.
//
// Decoding happens on the NimBLE host task; finished events are queued and
// drained from loop() so MIDI and the LED are only ever touched from one task.

class NimBLEServer;

struct SwitchEvent {
    uint8_t index;   // 0..4 -> A..E
    Tristate state;  // pedal's on/off state; only meaningful for E
    char name[24];   // effect name from the frame, for logging
};

namespace spark {

/// Adds the Spark service to an existing server.
bool attach(NimBLEServer* server);

/// Pops the next footswitch event, if any. Call from loop().
bool pop(SwitchEvent& e);

/// True while the pedal is connected. A MIDI host connecting does not count:
/// the status LED tracks the pedal, which is the link a player can see.
bool connected();

/// Called when any peer disconnects; clears state if it was the pedal.
void onPeerGone(uint16_t connHandle);

}  // namespace spark
