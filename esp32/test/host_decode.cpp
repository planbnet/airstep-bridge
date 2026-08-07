// Host-side test for the frame decoder. No Arduino, no hardware:
//
//   c++ -std=c++17 -I src test/host_decode.cpp src/spark_decode.cpp -o /tmp/t && /tmp/t
//
// Covers the three things that are easy to get subtly wrong: the 7-bit
// unpacking, the effect state flag (whose 0x42 "off" value is also the letter
// 'B' that starts the effect name "Booster"), and reassembly of a frame split
// across two ATT writes.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "spark_decode.h"

using Bytes = std::vector<uint8_t>;

/// Inverse of unpack7bit: groups of 7 bytes behind a mask byte holding their
/// high bits.
static Bytes pack7bit(const Bytes& raw) {
    Bytes out;
    for (size_t i = 0; i < raw.size(); i += 7) {
        const size_t n = (i + 7 < raw.size()) ? 7 : raw.size() - i;
        uint8_t mask = 0;
        for (size_t j = 0; j < n; ++j)
            if (raw[i + j] & 0x80) mask |= (1u << j);
        out.push_back(mask);
        for (size_t j = 0; j < n; ++j) out.push_back(raw[i + j] & 0x7F);
    }
    return out;
}

/// A full wire message: 16-byte block header plus the f0..f7 frame.
static Bytes frame(uint8_t cmd, uint8_t sub, const Bytes& payload) {
    const Bytes packed = pack7bit(payload);
    Bytes out = {0x01, 0xFE, 0x00, 0x00, 0x53, 0xFE, (uint8_t)(packed.size() + 8)};
    out.resize(16, 0x00);
    out.insert(out.end(), {0xF0, 0x01, 0x00, 0x00, cmd, sub});
    out.insert(out.end(), packed.begin(), packed.end());
    out.push_back(0xF7);
    return out;
}

static int checks = 0;
#define CHECK(cond)                                                   \
    do {                                                              \
        ++checks;                                                     \
        if (!(cond)) {                                                \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);    \
            return 1;                                                 \
        }                                                             \
    } while (0)

static int testUnpack() {
    // High bits live in the mask byte, so a round trip must restore them.
    const Bytes raw = {0x00, 0x7F, 0x80, 0xFF, 0x42, 0x43, 0x01, 0x99, 0x21};
    const Bytes packed = pack7bit(raw);
    uint8_t out[64];
    const size_t n = unpack7bit(packed.data(), packed.size(), out, sizeof(out));
    CHECK(n == raw.size());
    CHECK(memcmp(out, raw.data(), n) == 0);

    // Truncated group: the mask is present but not all 7 bytes follow.
    const uint8_t partial[] = {0x03, 0x01, 0x02, 0x03};
    const size_t m = unpack7bit(partial, sizeof(partial), out, sizeof(out));
    CHECK(m == 3);
    CHECK(out[0] == 0x81 && out[1] == 0x82 && out[2] == 0x03);
    return 0;
}

static int testPresets() {
    for (int idx = 0; idx < 4; ++idx) {
        const Bytes msg = frame(0x01, 0x38, {0x00, 0x00, (uint8_t)idx});
        FrameAssembler fa;
        fa.append(msg.data(), msg.size());

        SparkFrame f;
        CHECK(fa.nextFrame(f));
        const Control c = classify(f);
        CHECK(c.kind == ControlKind::Preset);
        CHECK(c.preset == idx);
        CHECK(!fa.nextFrame(f));  // exactly one frame, no phantom second press
    }
    return 0;
}

/// Builds a realistic 01 15 payload: a length-prefixed effect name, a separator,
/// then the state flag. The separator matters -- 0x42 and 0x43 are the printable
/// characters 'B' and 'C', so a flag butted straight against the name would be
/// read as part of it.
static Bytes effectPayload(const char* name, int flag) {
    Bytes p = {0x00, 0x00, (uint8_t)strlen(name)};
    p.insert(p.end(), name, name + strlen(name));
    p.push_back(0x00);
    if (flag >= 0) p.push_back((uint8_t)flag);
    return p;
}

static int testEffectStateFlag() {
    // "Booster" begins with 'B' == 0x42, the very byte that means "off". The
    // state is the *last* 0x42/0x43 in the packed payload, which sits after the
    // name, so the letter must never win.
    for (const auto& [flag, want] : std::vector<std::pair<int, Tristate>>{
             {0x43, Tristate::On}, {0x42, Tristate::Off}}) {
        const Bytes msg = frame(0x01, 0x15, effectPayload("Booster", flag));

        FrameAssembler fa;
        fa.append(msg.data(), msg.size());
        SparkFrame f;
        CHECK(fa.nextFrame(f));
        const Control c = classify(f);
        CHECK(c.kind == ControlKind::Effect);
        CHECK(c.state == want);
        CHECK(strcmp(c.name, "Booster") == 0);
    }

    // No flag at all, and a name with no 0x42/0x43 in it: the state is genuinely
    // unknown and must be reported as such rather than defaulting to off.
    const Bytes msg = frame(0x01, 0x15, effectPayload("Tube Drive", -1));
    FrameAssembler fa;
    fa.append(msg.data(), msg.size());
    SparkFrame f;
    CHECK(fa.nextFrame(f));
    const Control c = classify(f);
    CHECK(c.kind == ControlKind::Effect);
    CHECK(c.state == Tristate::Unknown);
    // The name is the longest printable *run*, and a space breaks a run, so a
    // two-word effect reports only its longest word. Harmless -- the name is
    // logging only; switch E is identified by cmd/sub, never by name.
    CHECK(strcmp(c.name, "Drive") == 0);
    return 0;
}

static int testFragmentedWrite() {
    const Bytes msg = frame(0x01, 0x38, {0x00, 0x00, 0x02});

    // Split at every possible boundary; each must yield exactly one C press.
    for (size_t cut = 1; cut < msg.size(); ++cut) {
        FrameAssembler fa;
        fa.append(msg.data(), cut);
        SparkFrame f;
        // A partial frame must not decode into anything yet.
        const bool early = fa.nextFrame(f);
        fa.append(msg.data() + cut, msg.size() - cut);

        int events = 0;
        Control last{};
        if (early) { last = classify(f); ++events; }
        while (fa.nextFrame(f)) { last = classify(f); ++events; }

        CHECK(events == 1);
        CHECK(last.kind == ControlKind::Preset);
        CHECK(last.preset == 2);
    }
    return 0;
}

static int testTwoFramesOneWrite() {
    Bytes msg = frame(0x01, 0x38, {0x00});
    const Bytes second = frame(0x01, 0x38, {0x03});
    msg.insert(msg.end(), second.begin(), second.end());

    FrameAssembler fa;
    fa.append(msg.data(), msg.size());

    SparkFrame f;
    CHECK(fa.nextFrame(f));
    CHECK(classify(f).preset == 0);
    CHECK(fa.nextFrame(f));
    CHECK(classify(f).preset == 3);
    CHECK(!fa.nextFrame(f));
    return 0;
}

static int testJunkAndQueries() {
    FrameAssembler fa;

    // 03 00 00 00 4x chunk counters and other noise carry no f0/f7 and must not
    // wedge the buffer or produce events.
    const uint8_t junk[] = {0x03, 0x00, 0x00, 0x00, 0x41};
    fa.append(junk, sizeof(junk));
    SparkFrame f;
    CHECK(!fa.nextFrame(f));

    // A handshake query is a well-formed frame that classifies as ignorable.
    const Bytes q = frame(0x02, 0x10, {0x00});
    fa.append(q.data(), q.size());
    CHECK(fa.nextFrame(f));
    CHECK(classify(f).kind == ControlKind::Query);

    // Junk in front of a real frame is skipped, and the frame still arrives.
    fa.append(junk, sizeof(junk));
    const Bytes real = frame(0x01, 0x38, {0x01});
    fa.append(real.data(), real.size());
    CHECK(fa.nextFrame(f));
    CHECK(classify(f).preset == 1);
    return 0;
}

int main() {
    if (testUnpack()) return 1;
    if (testPresets()) return 1;
    if (testEffectStateFlag()) return 1;
    if (testFragmentedWrite()) return 1;
    if (testTwoFramesOneWrite()) return 1;
    if (testJunkAndQueries()) return 1;
    printf("OK  %d checks\n", checks);
    return 0;
}
