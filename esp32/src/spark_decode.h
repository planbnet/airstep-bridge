#pragma once
#include <stddef.h>
#include <stdint.h>

// Spark BLE frame decoding, Mode 1.
//
// Wire format from the AIRSTEP Spk:
//   01 fe 00 00 53 fe <len> 00*9          <- 16-byte block header
//   f0 01 <seq> <chk> <cmd> <sub> <packed payload...> f7
//
// The payload is 7-bit packed: groups of 8 bytes where the first byte carries
// the high bit of the following 7. Because of that, every byte inside the
// payload is <= 0x7F, so 0xF0 and 0xF7 are unambiguous frame delimiters and the
// block header needs no special handling -- scanning for the delimiters skips
// it. This also makes reassembly across fragmented ATT writes safe.
//
// Commands seen in Mode 1 (verified against user-labelled captures):
//   01 38  change hardware preset, last payload byte = preset index 0..3 (A..D)
//   01 15  effect on/off, prefixed effect name + state flag              (E)
//   02 xx  handshake queries from the pedal, safe to ignore

static constexpr size_t SPARK_MAX_PAYLOAD = 256;

struct SparkFrame {
    uint8_t cmd;
    uint8_t sub;
    uint8_t packed[SPARK_MAX_PAYLOAD];
    size_t packedLen;
    uint8_t payload[SPARK_MAX_PAYLOAD];
    size_t payloadLen;
};

enum class ControlKind : uint8_t { Preset, Effect, Query, Unknown };

/// State byte carried by an 01 15 frame, when the pedal reported one.
enum class Tristate : uint8_t { Unknown, Off, On };

struct Control {
    ControlKind kind;
    int preset;       // Preset: 0..3 -> A..D
    Tristate state;   // Effect: drive on/off
    char name[24];    // Effect: the effect name, for logging
};

/// Undo the 7-bit packing. Returns the number of bytes written to `out`.
size_t unpack7bit(const uint8_t* packed, size_t len, uint8_t* out, size_t outCap);

/// Classify a parsed frame into a footswitch event or a frame to ignore.
Control classify(const SparkFrame& f);

/// Collects ATT writes and hands back whole f0..f7 frames.
///
/// The pedal normally delivers one frame per write, but nothing in the protocol
/// promises that, and a frame split across two writes would otherwise decode as
/// nothing or -- worse -- as a second press.
class FrameAssembler {
   public:
    void append(const uint8_t* data, size_t len);
    /// Extracts and consumes the next complete frame. Call until it returns false.
    bool nextFrame(SparkFrame& out);
    void reset() { len_ = 0; }

   private:
    static constexpr size_t kCap = 512;
    uint8_t buf_[kCap];
    size_t len_ = 0;
};
