#include "ble_spark_emulator.h"

#include <Arduino.h>
#include <NimBLEDevice.h>

#include "config.h"

namespace spark {

static QueueHandle_t queue = nullptr;
static FrameAssembler assembler;  // only ever touched from the NimBLE host task
static uint16_t pedalHandle = BLE_HS_CONN_HANDLE_NONE;

static void push(uint8_t index, Tristate state, const char* name) {
    SwitchEvent e{};
    e.index = index;
    e.state = state;
    strncpy(e.name, name ? name : "", sizeof(e.name) - 1);
    xQueueSend(queue, &e, 0);
}

/// Whichever peer talks the Spark protocol is the pedal. With two centrals on
/// one server the connection handle alone no longer identifies anyone, and the
/// pedal announces itself twice over: it subscribes to FFC2 right after
/// connecting, and it writes to FFC1 on the first press.
static void claimPedal(NimBLEConnInfo& info) {
    if (pedalHandle == info.getConnHandle()) return;
    pedalHandle = info.getConnHandle();
    assembler.reset();
    Serial.printf("[spark] pedal is %s\n", info.getAddress().toString().c_str());
}

class WriteCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
        claimPedal(info);
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
                        Serial.printf("[spark] preset %d out of range -- not Mode 1?\n",
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

    void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo& info, uint16_t sub) override {
        Serial.printf("[spark] pedal subscribed (value %u)\n", sub);
        if (sub) claimPedal(info);
    }
};

static WriteCallbacks writeCallbacks;

bool attach(NimBLEServer* server) {
    queue = xQueueCreate(16, sizeof(SwitchEvent));
    if (!queue) return false;

    NimBLEService* svc = server->createService(NimBLEUUID(SPARK_SERVICE_UUID));
    if (!svc) return false;

    NimBLECharacteristic* write = svc->createCharacteristic(
        NimBLEUUID(SPARK_WRITE_UUID),
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    if (!write) return false;
    write->setCallbacks(&writeCallbacks);

    // Present but silent: the pedal subscribes to it and is content. Kept
    // readable, answering with an empty value, to match the macOS bridge.
    NimBLECharacteristic* notify = svc->createCharacteristic(
        NimBLEUUID(SPARK_NOTIFY_UUID),
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    if (!notify) return false;
    notify->setValue((uint8_t*)nullptr, 0);
    notify->setCallbacks(&writeCallbacks);  // for the subscribe log line

    Serial.println("[spark] Spark service ready");
    return true;
}

bool pop(SwitchEvent& e) { return xQueueReceive(queue, &e, 0) == pdTRUE; }

bool connected() { return pedalHandle != BLE_HS_CONN_HANDLE_NONE; }

void onPeerGone(uint16_t connHandle) {
    if (connHandle != pedalHandle) return;
    pedalHandle = BLE_HS_CONN_HANDLE_NONE;
    assembler.reset();
    Serial.println("[spark] pedal gone");
}

}  // namespace spark
