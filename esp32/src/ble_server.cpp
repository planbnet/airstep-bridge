#include "ble_server.h"

#include <Arduino.h>
#include <NimBLEDevice.h>

#include "ble_midi.h"
#include "ble_spark_emulator.h"
#include "config.h"

namespace ble {

static NimBLEServer* server = nullptr;

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
        Serial.printf("[ble] connected: %s (%u peers)\n",
                      info.getAddress().toString().c_str(), s->getConnectedCount());

        // NimBLE stops advertising on a successful connection and does not bring
        // it back by itself. With one peripheral serving two centrals that is
        // fatal to whichever one is late: the pedal connects on power-up and the
        // tablet is then left scanning a band we have gone quiet on.
        if (s->getConnectedCount() < BLE_MAX_PEERS) {
            NimBLEDevice::startAdvertising();
        }
    }

    void onDisconnect(NimBLEServer*, NimBLEConnInfo& info, int reason) override {
        Serial.printf("[ble] disconnected (reason %d), advertising again\n", reason);
        // Neither service knows which handle was whose, so both get told and
        // each decides whether it was theirs.
        spark::onPeerGone(info.getConnHandle());
#if BLE_MIDI_ENABLED
        blemidi::onPeerGone(info.getConnHandle());
#endif
        // advertiseOnDisconnect(true) restarts advertising for us.
    }

    void onMTUChange(uint16_t mtu, NimBLEConnInfo&) override {
        Serial.printf("[ble] MTU %u\n", mtu);
    }
};

static ServerCallbacks serverCallbacks;

static bool startAdvertising() {
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();

    // Built by hand rather than through the convenience setters, because those
    // rebuild the payload on every call and would move the name into the scan
    // response the moment one is enabled. Element order matches what the pedal
    // has been seeing since the first working build: flags, name, service.
    NimBLEAdvertisementData advData;
    NimBLEAdvertisementData scanData;

    // Each of these returns false rather than truncating when the 31 bytes run
    // out, and a silently shortened advertisement is the one failure the pedal
    // could not recover from -- it would simply stop finding its amp.
    if (!advData.setFlags(BLE_HS_ADV_F_DISC_GEN)) return false;

#if BLE_MIDI_ENABLED && BLE_MIDI_UUID_IN_PRIMARY
    // BLE-MIDI UUID and FFC0 in the primary advertisement, name in the scan
    // response. CoreBluetooth filters its scan on the service UUID and does not
    // match one that appears only in a scan response, so a MIDI host never sees
    // a device arranged the other way round. The pedal is unaffected as long as
    // it finds its amp by FFC0, which is still here; it needs an active scan to
    // read the name. See BLE_MIDI_UUID_IN_PRIMARY in config.h.
    if (!advData.addServiceUUID(NimBLEUUID(BLE_MIDI_SERVICE_UUID))) return false;
    if (!advData.addServiceUUID(NimBLEUUID(SPARK_SERVICE_UUID))) return false;
    if (!scanData.setName(SPARK_ADV_NAME)) return false;
#elif BLE_MIDI_ENABLED
    // The pedal's original view: name and FFC0 primary, MIDI UUID in the scan
    // response where a CoreBluetooth host will probably not find it.
    if (!advData.setName(SPARK_ADV_NAME)) return false;
    if (!advData.addServiceUUID(NimBLEUUID(SPARK_SERVICE_UUID))) return false;
    if (!scanData.addServiceUUID(NimBLEUUID(BLE_MIDI_SERVICE_UUID))) return false;
#else
    if (!advData.setName(SPARK_ADV_NAME)) return false;
    if (!advData.addServiceUUID(NimBLEUUID(SPARK_SERVICE_UUID))) return false;
#endif

    // setAdvertisementData() marks the payload as explicitly set, which stops
    // start() from regenerating it -- and from overwriting the scan response.
    if (!adv->setAdvertisementData(advData)) return false;
    Serial.printf("[ble] advertisement %u of 31 bytes\n",
                  (unsigned)advData.getPayload().size());

#if BLE_MIDI_ENABLED
    adv->enableScanResponse(true);
    if (!adv->setScanResponseData(scanData)) return false;
    Serial.printf("[ble] scan response %u of 31 bytes\n",
                  (unsigned)scanData.getPayload().size());
#endif

    if (!adv->start()) return false;

    Serial.printf("[ble] advertising as \"%s\", service %04X\n", SPARK_ADV_NAME,
                  SPARK_SERVICE_UUID);
#if BLE_MIDI_ENABLED && BLE_MIDI_UUID_IN_PRIMARY
    Serial.println("[ble] BLE-MIDI UUID primary, name in scan response");
#elif BLE_MIDI_ENABLED
    Serial.println("[ble] BLE-MIDI UUID in scan response, name primary");
#endif
    return true;
}

bool begin() {
    if (!NimBLEDevice::init(SPARK_ADV_NAME)) return false;
    // Spark frames are longer than the 23-byte default MTU allows in one write.
    // The assembler would stitch fragments back together, but a large MTU means
    // there are none to stitch.
    NimBLEDevice::setMTU(255);

    server = NimBLEDevice::createServer();
    if (!server) return false;
    server->setCallbacks(&serverCallbacks);
    server->advertiseOnDisconnect(true);

    if (!spark::attach(server)) return false;
#if BLE_MIDI_ENABLED
    if (!blemidi::attach(server)) return false;
#endif

    // No explicit service start: in NimBLE 2.x services start with the server,
    // which happens implicitly when advertising starts.
    return startAdvertising();
}

uint8_t peerCount() { return server ? server->getConnectedCount() : 0; }

}  // namespace ble
