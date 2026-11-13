#include "ble_midi_parse.h"

#include <string.h>

int midiDataLength(uint8_t status) {
    if (status < 0x80) return -1;  // data byte, not a status byte
    if (status < 0xC0) return 2;   // 8x note off, 9x note on, Ax aftertouch, Bx CC
    if (status < 0xE0) return 1;   // Cx program change, Dx channel pressure
    if (status < 0xF0) return 2;   // Ex pitch bend
    switch (status) {
        case 0xF1: return 1;  // MTC quarter frame
        case 0xF2: return 2;  // song position pointer
        case 0xF3: return 1;  // song select -- what some sheet music apps send
        default: return 0;    // F4/F5/F6 and real-time F8..FF; F0/F7 are special
    }
}

void BleMidiParser::reset() {
    len_ = 0;
    status_ = 0;
    need_ = 0;
    running_ = 0;
    inSysEx_ = false;
}

void BleMidiParser::emit(Sink sink, void* ctx) {
    if (len_ && sink) sink(buf_, len_, ctx);
    len_ = 0;
}

void BleMidiParser::onStatus(uint8_t s, Sink sink, void* ctx) {
    // Real-time is a single byte that may appear anywhere, including between the
    // data bytes of another message or inside a SysEx. It leaves both the
    // message being assembled and running status alone.
    if (s >= 0xF8) {
        if (sink) sink(&s, 1, ctx);
        return;
    }

    if (s == 0xF0) {
        len_ = 0;
        inSysEx_ = true;
        running_ = 0;
        status_ = 0;
        need_ = 0;
        buf_[len_++] = s;
        return;
    }

    if (s == 0xF7) {
        if (inSysEx_) {
            if (len_ < BLE_MIDI_MAX_MESSAGE) buf_[len_++] = s;
            emit(sink, ctx);
            inSysEx_ = false;
        }
        return;  // a stray F7 outside a SysEx means nothing
    }

    // Any other status while a SysEx is open means the SysEx was never
    // terminated. Dropping the fragment beats gluing it onto this message.
    if (inSysEx_) {
        inSysEx_ = false;
        len_ = 0;
    }

    status_ = s;
    need_ = midiDataLength(s);
    running_ = (s < 0xF0) ? s : 0;  // system common cancels running status
    len_ = 0;
    buf_[len_++] = s;
    if (need_ == 0) emit(sink, ctx);
}

void BleMidiParser::onData(uint8_t d, Sink sink, void* ctx) {
    if (inSysEx_) {
        buf_[len_++] = d;
        // Long SysEx goes out in chunks. Anything that forwards the bytes in
        // order cannot tell the difference, and it bounds the buffer.
        if (len_ == BLE_MIDI_MAX_MESSAGE) emit(sink, ctx);
        return;
    }

    if (need_ == 0) {
        // Running status: another message of the same kind with its status byte
        // omitted. Without a previous channel message there is nothing to attach
        // this byte to, so it is dropped.
        if (running_ == 0) return;
        status_ = running_;
        need_ = midiDataLength(running_);
        len_ = 0;
        buf_[len_++] = running_;
    }

    if (len_ < BLE_MIDI_MAX_MESSAGE) buf_[len_++] = d;
    if (--need_ == 0) emit(sink, ctx);
}

void BleMidiParser::parse(const uint8_t* packet, size_t len, Sink sink, void* ctx) {
    // Every packet starts with a header byte. Without it this is not BLE-MIDI,
    // and guessing would turn noise into notes.
    if (len < 1 || !(packet[0] & 0x80)) return;

    // A high-bit byte is a timestamp, unless the byte before it already was --
    // then it is the status byte that timestamp belongs to. A SysEx continuation
    // packet opens straight into data bytes, which this handles without a case:
    // no timestamp is pending, and data bytes never set the high bit.
    bool timestampSeen = false;
    for (size_t i = 1; i < len; ++i) {
        const uint8_t b = packet[i];
        if (b & 0x80) {
            if (!timestampSeen) {
                timestampSeen = true;
                continue;
            }
            timestampSeen = false;
            onStatus(b, sink, ctx);
        } else {
            timestampSeen = false;
            onData(b, sink, ctx);
        }
    }
}

size_t bleMidiEncode(const uint8_t* msg, size_t len, uint32_t timestampMs,
                     uint8_t* out, size_t outCap) {
    if (len == 0 || outCap < len + 2) return 0;
    const uint16_t ts = (uint16_t)(timestampMs & 0x1FFF);  // 13 bits, wraps at 8.192 s
    out[0] = 0x80 | ((ts >> 7) & 0x3F);
    out[1] = 0x80 | (ts & 0x7F);
    memcpy(out + 2, msg, len);
    return len + 2;
}
