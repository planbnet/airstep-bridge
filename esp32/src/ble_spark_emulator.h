#pragma once
#include <stdint.h>

#include "spark_decode.h"

// The ESP32 is the BLE peripheral and GATT server, pretending to be a Positive
// Grid Spark 40. The AIRSTEP connects to it as a central, writes footswitch
// events to FFC1 and subscribes to FFC2 -- which we never notify on, exactly as
// the macOS implementation does not. Reads are answered with an empty value.
//
// Decoding happens on the NimBLE host task; finished events are queued and
// drained from loop() so MIDI and the LED are only ever touched from one task.

struct SwitchEvent {
    uint8_t index;   // 0..4 -> A..E
    Tristate state;  // pedal's on/off state; only meaningful for E
    char name[24];   // effect name from the frame, for logging
};

namespace ble {

/// Brings up the stack, service and advertising. False means the LED goes red.
bool begin();

/// Pops the next footswitch event, if any. Call from loop().
bool pop(SwitchEvent& e);

bool connected();

}  // namespace ble
