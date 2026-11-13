#include "ble_midi.h"

#include <Arduino.h>
#include <NimBLEDevice.h>

#include "ble_midi_parse.h"
#include "config.h"
#include "midi_out.h"

namespace blemidi {

static NimBLECharacteristic* chr = nullptr;
static BleMidiParser parser;  // only ever touched from the NimBLE host task
static uint16_t hostHandle = BLE_HS_CONN_HANDLE_NONE;

/// Sink for the parser: every decoded message goes straight to the DIN queue.
/// forward() rather than emit(), so it is never mirrored back to the host that
/// just sent it.
static void toDin(const uint8_t* msg, size_t len, void*) {
    midi::forward(msg, len);

    // Clock, active sensing and the rest of the real-time group can arrive 24
    // times a beat. Forward them, but do not narrate them.
    if (msg[0] >= 0xF8) return;

    const uint8_t kind = msg[0] & 0xF0;
    const uint8_t ch = (msg[0] & 0x0F) + 1;
    if (kind == 0xC0 && len >= 2)
        Serial.printf("[midi<] Program %u ch %u\n", msg[1], ch);
    else if (kind == 0xB0 && len >= 3)
        Serial.printf("[midi<] CC %u = %u ch %u\n", msg[1], msg[2], ch);
    else if (kind == 0x90 && len >= 3)
        Serial.printf("[midi<] Note %u = %u ch %u\n", msg[1], msg[2], ch);
    else if (msg[0] == 0xF3 && len >= 2)
        Serial.printf("[midi<] Song Select %u\n", msg[1]);
    else
        Serial.printf("[midi<] status %02X, %u bytes\n", msg[0], (unsigned)len);
}

class Callbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
        hostHandle = info.getConnHandle();
        const NimBLEAttValue v = c->getValue();
        parser.parse(v.data(), v.length(), toDin, nullptr);
    }

    void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo& info, uint16_t sub) override {
        if (sub) {
            hostHandle = info.getConnHandle();
            parser.reset();
            Serial.printf("[midi] host subscribed: %s\n",
                          info.getAddress().toString().c_str());
            // Ask for a tighter connection interval than the host's default. A
            // rejected request is not an error -- it just stays at whatever the
            // host picked, and a preset change does not notice the difference.
            NimBLEServer* s = NimBLEDevice::getServer();
            if (s)
                s->updateConnParams(info.getConnHandle(), BLE_MIDI_ITVL_MIN,
                                    BLE_MIDI_ITVL_MAX, BLE_MIDI_LATENCY,
                                    BLE_MIDI_TIMEOUT);
        } else if (info.getConnHandle() == hostHandle) {
            hostHandle = BLE_HS_CONN_HANDLE_NONE;
            Serial.println("[midi] host unsubscribed");
        }
    }
};

static Callbacks callbacks;

bool attach(NimBLEServer* server) {
    NimBLEService* svc = server->createService(NimBLEUUID(BLE_MIDI_SERVICE_UUID));
    if (!svc) return false;

    // The specification asks for write-without-response and notify. WRITE is
    // allowed as well because it costs nothing and a host that insists on an
    // acknowledged write would otherwise fail silently.
    chr = svc->createCharacteristic(NimBLEUUID(BLE_MIDI_CHAR_UUID),
                                    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE |
                                        NIMBLE_PROPERTY::WRITE_NR |
                                        NIMBLE_PROPERTY::NOTIFY);
    if (!chr) return false;

    // A read must return an empty value, per the specification.
    chr->setValue((uint8_t*)nullptr, 0);
    chr->setCallbacks(&callbacks);

    Serial.println("[midi] BLE-MIDI service ready");
    return true;
}

void send(const uint8_t* msg, size_t len) {
    if (!chr || hostHandle == BLE_HS_CONN_HANDLE_NONE) return;
    uint8_t packet[BLE_MIDI_MAX_MESSAGE + 2];
    const size_t n = bleMidiEncode(msg, len, millis(), packet, sizeof(packet));
    if (n) chr->notify(packet, n, hostHandle);
}

bool connected() { return hostHandle != BLE_HS_CONN_HANDLE_NONE; }

void onPeerGone(uint16_t connHandle) {
    if (connHandle != hostHandle) return;
    hostHandle = BLE_HS_CONN_HANDLE_NONE;
    // An unfinished SysEx must not merge into whatever the next host sends.
    parser.reset();
    Serial.println("[midi] host gone");
}

}  // namespace blemidi
