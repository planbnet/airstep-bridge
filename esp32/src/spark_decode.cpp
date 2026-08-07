#include "spark_decode.h"

#include <string.h>

size_t unpack7bit(const uint8_t* packed, size_t len, uint8_t* out, size_t outCap) {
    size_t n = 0;
    for (size_t i = 0; i < len; i += 8) {
        const uint8_t mask = packed[i];
        const size_t end = (i + 8 < len) ? i + 8 : len;
        for (size_t j = i + 1, bit = 0; j < end; ++j, ++bit) {
            if (n >= outCap) return n;
            out[n++] = packed[j] | (((mask >> bit) & 1) ? 0x80 : 0x00);
        }
    }
    return n;
}

/// Longest printable-ASCII run -- the effect name in an 01 15 frame.
static void asciiRun(const uint8_t* bytes, size_t len, char* out, size_t outCap,
                     size_t minLength = 3) {
    size_t bestStart = 0, bestLen = 0, curStart = 0, curLen = 0;
    for (size_t i = 0; i < len; ++i) {
        if (bytes[i] > 0x20 && bytes[i] < 0x7F) {
            if (curLen == 0) curStart = i;
            ++curLen;
        } else {
            if (curLen > bestLen) { bestLen = curLen; bestStart = curStart; }
            curLen = 0;
        }
    }
    if (curLen > bestLen) { bestLen = curLen; bestStart = curStart; }

    if (bestLen < minLength) { out[0] = '\0'; return; }
    if (bestLen > outCap - 1) bestLen = outCap - 1;
    memcpy(out, bytes + bestStart, bestLen);
    out[bestLen] = '\0';
}

/// Effect on/off flag. Determined empirically: the last 0x43/0x42 in the packed
/// payload is the state byte (0x43 = on, 0x42 = off). It sits after the effect
/// name, so a 'B'/'C' inside the name cannot be mistaken for it.
static Tristate effectEnabled(const SparkFrame& f) {
    for (size_t i = f.packedLen; i > 0; --i) {
        const uint8_t b = f.packed[i - 1];
        if (b == 0x43) return Tristate::On;
        if (b == 0x42) return Tristate::Off;
    }
    return Tristate::Unknown;
}

Control classify(const SparkFrame& f) {
    Control c{};
    c.name[0] = '\0';
    if (f.cmd == 0x01 && f.sub == 0x38) {
        c.kind = ControlKind::Preset;
        c.preset = f.payloadLen ? f.payload[f.payloadLen - 1] : 0;
    } else if (f.cmd == 0x01 && f.sub == 0x15) {
        c.kind = ControlKind::Effect;
        c.state = effectEnabled(f);
        asciiRun(f.payload, f.payloadLen, c.name, sizeof(c.name));
        if (c.name[0] == '\0') strcpy(c.name, "effect");
    } else if (f.cmd == 0x02) {
        c.kind = ControlKind::Query;
    } else {
        c.kind = ControlKind::Unknown;
    }
    return c;
}

void FrameAssembler::append(const uint8_t* data, size_t len) {
    if (len > kCap) { data += len - kCap; len = kCap; }
    if (len_ + len > kCap) {
        // Nothing parseable arrived in half a kilobyte, so whatever is buffered
        // is junk or a lost terminator. Start over rather than dropping the new
        // bytes, which are the ones more likely to be a real frame.
        len_ = 0;
    }
    memcpy(buf_ + len_, data, len);
    len_ += len;
}

bool FrameAssembler::nextFrame(SparkFrame& out) {
    // Drop anything ahead of a start delimiter.
    size_t start = 0;
    while (start < len_ && buf_[start] != 0xF0) ++start;
    if (start > 0) {
        memmove(buf_, buf_ + start, len_ - start);
        len_ -= start;
    }
    if (len_ < 7) return false;  // f0 01 seq chk cmd sub ... f7

    size_t end = 1;
    while (end < len_ && buf_[end] != 0xF7) ++end;
    if (end >= len_) return false;  // terminator not here yet

    const bool valid = buf_[1] == 0x01 && end > 5;
    if (valid) {
        out.cmd = buf_[4];
        out.sub = buf_[5];
        out.packedLen = end - 6;
        if (out.packedLen > SPARK_MAX_PAYLOAD) out.packedLen = SPARK_MAX_PAYLOAD;
        memcpy(out.packed, buf_ + 6, out.packedLen);
        out.payloadLen =
            unpack7bit(out.packed, out.packedLen, out.payload, SPARK_MAX_PAYLOAD);
    }

    // Consume through the terminator either way, so a malformed frame cannot
    // wedge the buffer.
    const size_t consumed = end + 1;
    memmove(buf_, buf_ + consumed, len_ - consumed);
    len_ -= consumed;

    return valid ? true : nextFrame(out);
}
