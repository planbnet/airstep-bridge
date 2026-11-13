// Host-side test for the BLE-MIDI packet parser. No Arduino, no hardware:
//
//   c++ -std=c++17 -I src test/host_blemidi.cpp src/ble_midi_parse.cpp -o /tmp/m && /tmp/m
//
// The cases worth testing are the ones where "split on the high bit" gives the
// wrong answer: timestamp bytes whose value looks like a status byte, running
// status with and without a timestamp, real-time arriving between the data bytes
// of another message, and SysEx continuing across a packet boundary.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "ble_midi_parse.h"

using Bytes = std::vector<uint8_t>;
using Messages = std::vector<Bytes>;

static void collect(const uint8_t* msg, size_t len, void* ctx) {
    static_cast<Messages*>(ctx)->push_back(Bytes(msg, msg + len));
}

/// Header byte for a 13-bit timestamp, as the sender would emit it.
static uint8_t header(uint16_t ts) { return 0x80 | ((ts >> 7) & 0x3F); }
/// Timestamp byte for the same.
static uint8_t stamp(uint16_t ts) { return 0x80 | (ts & 0x7F); }

static int checks = 0;
#define CHECK(cond)                                                \
    do {                                                           \
        ++checks;                                                  \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                              \
        }                                                          \
    } while (0)

static Messages run(const std::vector<Bytes>& packets) {
    Messages out;
    BleMidiParser p;
    for (const Bytes& packet : packets) p.parse(packet.data(), packet.size(), collect, &out);
    return out;
}

static int testDataLengths() {
    CHECK(midiDataLength(0x90) == 2);  // note on
    CHECK(midiDataLength(0xB0) == 2);  // control change
    CHECK(midiDataLength(0xC0) == 1);  // program change
    CHECK(midiDataLength(0xD0) == 1);  // channel pressure
    CHECK(midiDataLength(0xE0) == 2);  // pitch bend
    CHECK(midiDataLength(0xF2) == 2);  // song position pointer
    CHECK(midiDataLength(0xF3) == 1);  // song select
    CHECK(midiDataLength(0xF8) == 0);  // clock
    CHECK(midiDataLength(0x40) == -1); // data byte, not a status
    return 0;
}

static int testProgramChange() {
    // The whole point of the exercise: forScore opens a score, this arrives.
    const Messages m = run({{header(0), stamp(0), 0xC0, 106}});
    CHECK(m.size() == 1);
    CHECK(m[0] == Bytes({0xC0, 106}));
    return 0;
}

static int testTimestampLooksLikeStatus() {
    // A timestamp byte can take any value from 0x80 to 0xFF, so it is routinely
    // indistinguishable from a status byte by value alone. Only position tells
    // them apart. Here both header and timestamp read as 0xFF.
    const Messages m = run({{header(0x1FFF), stamp(0x1FFF), 0xC0, 106}});
    CHECK(m.size() == 1);
    CHECK(m[0] == Bytes({0xC0, 106}));
    return 0;
}

static int testMultipleMessagesOnePacket() {
    const Messages m = run({{header(0), stamp(0), 0xB0, 49, 127, stamp(5), 0xB0, 63, 127}});
    CHECK(m.size() == 2);
    CHECK(m[0] == Bytes({0xB0, 49, 127}));
    CHECK(m[1] == Bytes({0xB0, 63, 127}));
    return 0;
}

static int testRunningStatus() {
    // Status byte omitted, timestamp omitted too -- bare data bytes, which a
    // parser that assumes every message starts with a status byte would drop.
    const Messages m = run({{header(0), stamp(0), 0xB0, 49, 127, 50, 127}});
    CHECK(m.size() == 2);
    CHECK(m[0] == Bytes({0xB0, 49, 127}));
    CHECK(m[1] == Bytes({0xB0, 50, 127}));

    // Status byte omitted but the timestamp kept, which is equally legal.
    const Messages n = run({{header(0), stamp(0), 0x90, 60, 127, stamp(9), 62, 127}});
    CHECK(n.size() == 2);
    CHECK(n[0] == Bytes({0x90, 60, 127}));
    CHECK(n[1] == Bytes({0x90, 62, 127}));

    // Running status also carries across packets.
    const Messages o = run({{header(0), stamp(0), 0xB0, 49, 127}, {header(1), stamp(1), 50, 127}});
    CHECK(o.size() == 2);
    CHECK(o[1] == Bytes({0xB0, 50, 127}));
    return 0;
}

static int testSystemCommonCancelsRunningStatus() {
    // After a Song Select, bare data bytes have nothing to attach to and must be
    // dropped rather than silently becoming another Song Select.
    const Messages m = run({{header(0), stamp(0), 0xF3, 4, 7, 9}});
    CHECK(m.size() == 1);
    CHECK(m[0] == Bytes({0xF3, 4}));
    return 0;
}

static int testRealTimeInterleaved() {
    // Clock arriving between the two data bytes of a note-on. The note must
    // survive intact and the clock must come out on its own.
    const Messages m = run({{header(0), stamp(0), 0x90, 60, stamp(1), 0xF8, 127}});
    CHECK(m.size() == 2);
    CHECK(m[0] == Bytes({0xF8}));
    CHECK(m[1] == Bytes({0x90, 60, 127}));
    return 0;
}

static int testSysEx() {
    // Complete in one packet.
    const Messages m = run({{header(0), stamp(0), 0xF0, 0x7D, 0x01, 0x02, stamp(3), 0xF7}});
    CHECK(m.size() == 1);
    CHECK(m[0] == Bytes({0xF0, 0x7D, 0x01, 0x02, 0xF7}));

    // Split across packets: the continuation opens straight into data bytes with
    // no timestamp, which is the case that catches a positional parser out.
    const Messages n = run({{header(0), stamp(0), 0xF0, 0x7D, 0x01},
                            {header(1), 0x02, 0x03},
                            {header(2), 0x04, stamp(2), 0xF7}});
    CHECK(n.size() == 1);
    CHECK(n[0] == Bytes({0xF0, 0x7D, 0x01, 0x02, 0x03, 0x04, 0xF7}));

    // Real-time inside a SysEx comes out separately and leaves the SysEx whole.
    const Messages o = run({{header(0), stamp(0), 0xF0, 0x7D, stamp(1), 0xFE, 0x01, stamp(2), 0xF7}});
    CHECK(o.size() == 2);
    CHECK(o[0] == Bytes({0xFE}));
    CHECK(o[1] == Bytes({0xF0, 0x7D, 0x01, 0xF7}));
    return 0;
}

static int testUnterminatedSysExIsDropped() {
    // A SysEx that never ends must not swallow or merge with the next message.
    const Messages m = run({{header(0), stamp(0), 0xF0, 0x7D, 0x01},
                            {header(1), stamp(1), 0xC0, 106}});
    CHECK(m.size() == 1);
    CHECK(m[0] == Bytes({0xC0, 106}));
    return 0;
}

static int testGarbageIsIgnored() {
    // No header byte: not a BLE-MIDI packet at all.
    const Messages m = run({{0x00, 0xC0, 106}});
    CHECK(m.empty());

    // Empty packet, and a header with nothing after it.
    const Messages n = run({{}, {header(0)}});
    CHECK(n.empty());

    // Data bytes with no preceding status anywhere.
    const Messages o = run({{header(0), stamp(0), 0x40, 0x50}});
    CHECK(o.empty());
    return 0;
}

static int testResetClearsState() {
    BleMidiParser p;
    Messages out;
    const Bytes open = {header(0), stamp(0), 0xF0, 0x7D, 0x01};
    p.parse(open.data(), open.size(), collect, &out);
    p.reset();  // link dropped mid-SysEx

    const Bytes next = {header(1), stamp(1), 0xC0, 106};
    p.parse(next.data(), next.size(), collect, &out);
    CHECK(out.size() == 1);
    CHECK(out[0] == Bytes({0xC0, 106}));
    return 0;
}

static int testEncode() {
    uint8_t out[8];
    const uint8_t msg[] = {0xC0, 106};
    const size_t n = bleMidiEncode(msg, sizeof(msg), 0x1234, out, sizeof(out));
    CHECK(n == 4);
    CHECK(out[0] == (0x80 | ((0x1234 >> 7) & 0x3F)));
    CHECK(out[1] == (0x80 | (0x1234 & 0x7F)));
    CHECK(out[2] == 0xC0 && out[3] == 106);

    // Round trip: what we encode, we must decode back.
    Messages back;
    BleMidiParser p;
    p.parse(out, n, collect, &back);
    CHECK(back.size() == 1);
    CHECK(back[0] == Bytes({0xC0, 106}));

    // Timestamps above 13 bits wrap rather than corrupting the header.
    const size_t m = bleMidiEncode(msg, sizeof(msg), 0xFFFFFFFF, out, sizeof(out));
    CHECK(m == 4);
    CHECK((out[0] & 0xC0) == 0x80);  // bit 7 set, bit 6 clear
    CHECK((out[1] & 0x80) == 0x80);

    // Too small a buffer writes nothing at all.
    CHECK(bleMidiEncode(msg, sizeof(msg), 0, out, 3) == 0);
    return 0;
}

int main() {
    if (testDataLengths()) return 1;
    if (testProgramChange()) return 1;
    if (testTimestampLooksLikeStatus()) return 1;
    if (testMultipleMessagesOnePacket()) return 1;
    if (testRunningStatus()) return 1;
    if (testSystemCommonCancelsRunningStatus()) return 1;
    if (testRealTimeInterleaved()) return 1;
    if (testSysEx()) return 1;
    if (testUnterminatedSysExIsDropped()) return 1;
    if (testGarbageIsIgnored()) return 1;
    if (testResetClearsState()) return 1;
    if (testEncode()) return 1;
    printf("OK  %d checks\n", checks);
    return 0;
}
