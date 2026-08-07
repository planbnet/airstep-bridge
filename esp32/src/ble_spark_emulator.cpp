#include "ble_spark_emulator.h"

#include <Arduino.h>
#include <NimBLEDevice.h>

#include "config.h"

namespace ble {

static NimBLEServer* server = nullptr;
static QueueHandle_t queue = nullptr;
static volatile bool linkUp = false;
static FrameAssembler assembler;  // only ever touched from the NimBLE host task

static void push(uint8_t index, Tristate state, const char* name) {
    SwitchEvent e{};
    e.index = index;
    e.state = state;
    strncpy(e.name, name ? name : "", sizeof(e.name) - 1);
    xQueueSend(queue, &e, 0);
}

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer*, NimBLEConnInfo& info) override {
        linkUp = true;
        assembler.reset();
        Serial.printf("[ble] connected: %s\n", info.getAddress().toString().c_str());
    }

    void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
        linkUp = false;
        assembler.reset();
        // advertiseOnDisconnect(true) brings advertising back up for us, which is
        // all the pedal needs to find us again on its own.
        Serial.printf("[ble] disconnected (reason %d), advertising again\n", reason);
    }

    void onMTUChange(uint16_t mtu, NimBLEConnInfo&) override {
        Serial.printf("[ble] MTU %u\n", mtu);
    }
};

class WriteCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
        const NimBLEAttValue v = c->getValue();
        assembler.append(v.data(), v.length());

        SparkFrame frame;
        while (assembler.nextFrame(frame)) {
            const Control ctl = classify(frame);
            switch (ctl.kind) {
                case ControlKind::Preset:
                    // 01 38 carries the *resulting* hardware preset, which in
                    // Mode 1 is a stable per-switch value. Anything outside 0..3
                    // means the pedal is not in Mode 1 and is not ours to map.
                    if (ctl.preset >= 0 && ctl.preset < 4)
                        push((uint8_t)ctl.preset, Tristate::Unknown, "preset");
                    else
                        Serial.printf("[ble] preset %d out of range -- not Mode 1?\n",
                                      ctl.preset);
                    break;

                case ControlKind::Effect:
                    push(4, ctl.state, ctl.name);
                    break;

                case ControlKind::Query:
                    // 02 10 (current preset number) and 02 01 (current preset).
                    // Deliberately unanswered; see the README.
                    break;

                case ControlKind::Unknown:
                    // 03 00 00 00 4x is a chunk counter, not input.
                    break;
            }
        }
    }

    void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo&, uint16_t sub) override {
        Serial.printf("[ble] pedal subscribed (value %u)\n", sub);
    }
};

static ServerCallbacks serverCallbacks;
static WriteCallbacks writeCallbacks;

bool begin() {
    queue = xQueueCreate(16, sizeof(SwitchEvent));
    if (!queue) return false;

    if (!NimBLEDevice::init(SPARK_ADV_NAME)) return false;
    // Spark frames are longer than the 23-byte default MTU allows in one write.
    // The assembler would stitch fragments back together, but a large MTU means
    // there are none to stitch.
    NimBLEDevice::setMTU(255);

    server = NimBLEDevice::createServer();
    if (!server) return false;
    server->setCallbacks(&serverCallbacks);
    server->advertiseOnDisconnect(true);

    NimBLEService* svc = server->createService(NimBLEUUID(SPARK_SERVICE_UUID));
    if (!svc) return false;

    NimBLECharacteristic* write = svc->createCharacteristic(
        NimBLEUUID(SPARK_WRITE_UUID),
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    write->setCallbacks(&writeCallbacks);

    // Present but silent: the pedal subscribes to it and is content. Kept
    // readable, answering with an empty value, to match the macOS bridge.
    NimBLECharacteristic* notify = svc->createCharacteristic(
        NimBLEUUID(SPARK_NOTIFY_UUID),
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    notify->setValue((uint8_t*)nullptr, 0);
    notify->setCallbacks(&writeCallbacks);  // for the subscribe log line

    // No svc->start() here: in NimBLE 2.x services start with the server, which
    // happens implicitly when advertising starts.

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    // NimBLE 2.x does not advertise the device name implicitly any more.
    adv->setName(SPARK_ADV_NAME);
    adv->addServiceUUID(NimBLEUUID(SPARK_SERVICE_UUID));
    adv->enableScanResponse(true);
    if (!adv->start()) return false;

    Serial.printf("[ble] advertising as \"%s\", service %04X\n", SPARK_ADV_NAME,
                  SPARK_SERVICE_UUID);
    return true;
}

bool pop(SwitchEvent& e) { return xQueueReceive(queue, &e, 0) == pdTRUE; }

bool connected() { return linkUp; }

}  // namespace ble
