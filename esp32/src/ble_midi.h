#pragma once
#include <stddef.h>
#include <stdint.h>

// Standard BLE-MIDI, hung off the same GATT server as the Spark emulation.
//
// To a tablet or laptop this looks like any other wireless MIDI interface: it
// turns up in the iOS "Bluetooth MIDI Devices" sheet, in Audio MIDI Setup on a
// Mac, and in whatever MIDI port list an app offers. Messages written to the
// characteristic are forwarded to the DIN port, so a sheet music app changing
// song changes the HX Stomp's preset.
//
// The other direction is optional (MIRROR_PRESSES_TO_BLE_MIDI): everything the
// bridge sends to the HX Stomp is also notified to the host, so a footswitch can
// turn the page while it drives the looper.
//
// Packet decoding lives in ble_midi_parse, which has no NimBLE in it and is
// tested on the host.

class NimBLEServer;

namespace blemidi {

/// Adds the service and characteristic to an existing server.
bool attach(NimBLEServer* server);

/// Notifies one MIDI message to the connected host. Does nothing when no host
/// is subscribed, which is the normal case when only the pedal is around.
void send(const uint8_t* msg, size_t len);

/// True once a host has subscribed to notifications.
bool connected();

/// Called when any peer disconnects; clears state if it was the MIDI host.
void onPeerGone(uint16_t connHandle);

}  // namespace blemidi
