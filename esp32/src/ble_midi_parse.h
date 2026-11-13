#pragma once
#include <stddef.h>
#include <stdint.h>

// BLE-MIDI packet decoding, kept free of Arduino and NimBLE so it can be tested
// on the host -- same split as spark_decode vs. ble_spark_emulator.
//
// "MIDI over Bluetooth Low Energy" is Apple's specification, adopted by the MIDI
// Association. One service, one characteristic, and a packet that is just MIDI
// bytes with timestamps woven in:
//
//   header     1 0 t t t t t t      bit 7 set, bit 6 clear, timestamp bits 12..7
//   timestamp  1 t t t t t t t      bit 7 set, timestamp bits 6..0
//   <status + data bytes>
//   [timestamp] <next message> ...
//
// The timestamp is 13 bits of milliseconds and wraps every 8.192 s. We throw it
// away on receive: it exists to let a sender batch several messages with their
// original spacing, and a preset change does not care about 2 ms of jitter.
//
// The awkward part is that timestamp bytes and MIDI status bytes both have bit 7
// set, so they can only be told apart by position -- a timestamp always precedes
// the status byte it belongs to. Two further wrinkles make a naive "split on
// high bit" parser wrong:
//
//   - Running status: after a channel message, following messages may omit the
//     status byte, and then the timestamp byte is optional too. So a packet can
//     legitimately contain bare data bytes.
//   - SysEx streams across packets. A continuation packet is a header followed
//     by raw data bytes, and only the closing F7 gets a timestamp.
//
// Both fall out of one rule: inside a packet body, a byte with bit 7 set is a
// timestamp unless a timestamp was just seen, in which case it is a status byte.

/// Data bytes that follow `status`. Real-time and F4/F5/F6 take none; SysEx is
/// variable and handled separately. Returns -1 if `status` is not a status byte.
int midiDataLength(uint8_t status);

/// Longest message handed to the sink in one piece. SysEx longer than this is
/// split across several calls, which is harmless for anything that forwards the
/// bytes onward in order.
static constexpr size_t BLE_MIDI_MAX_MESSAGE = 64;

/// Decodes BLE-MIDI packets into whole MIDI messages.
///
/// State persists between packets on purpose: running status and an unfinished
/// SysEx both carry across packet boundaries. Call reset() when the link drops,
/// so a half-received SysEx cannot merge into whatever arrives next.
class BleMidiParser {
   public:
    /// Receives one complete MIDI message. SysEx may arrive in several chunks.
    using Sink = void (*)(const uint8_t* msg, size_t len, void* ctx);

    void reset();
    void parse(const uint8_t* packet, size_t len, Sink sink, void* ctx);

   private:
    void onStatus(uint8_t status, Sink sink, void* ctx);
    void onData(uint8_t data, Sink sink, void* ctx);
    void emit(Sink sink, void* ctx);

    uint8_t buf_[BLE_MIDI_MAX_MESSAGE];
    size_t len_ = 0;
    /// Status byte of the message being assembled, 0 when none is.
    uint8_t status_ = 0;
    /// Data bytes still outstanding for status_.
    int need_ = 0;
    /// Last channel-message status, for running status. Cleared by system common.
    uint8_t running_ = 0;
    bool inSysEx_ = false;
};

/// Wraps one MIDI message in a BLE-MIDI packet, for the pedal -> host direction.
/// `timestampMs` is free-running milliseconds; only the low 13 bits are sent.
/// Returns the number of bytes written to `out`, or 0 if it would not fit.
size_t bleMidiEncode(const uint8_t* msg, size_t len, uint32_t timestampMs,
                     uint8_t* out, size_t outCap);
