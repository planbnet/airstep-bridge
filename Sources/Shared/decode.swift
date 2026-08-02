import Foundation

// Spark frame decoding, Mode 1.
//
// Wire format from the AIRSTEP Spk:
//   01 fe 00 00 53 fe <len> 00*9          <- 16-byte block header
//   f0 01 <seq> <chk> <cmd> <sub> <packed payload...> f7
//
// The payload is 7-bit packed: groups of 8 bytes where the first byte carries the
// high bit of the following 7.
//
// Commands seen in Mode 1 (verified against user-labelled captures):
//   01 38  change hardware preset, last payload byte = preset index 0..3  (A..D)
//   01 15  effect on/off, prefixed effect name + state flag               (E)
//   02 xx  handshake queries from the pedal, safe to ignore

struct SparkFrame {
    let cmd: UInt8
    let sub: UInt8
    let packed: [UInt8]
    let payload: [UInt8]
}

func unpack7bit(_ packed: [UInt8]) -> [UInt8] {
    var out: [UInt8] = []
    var i = 0
    while i < packed.count {
        let mask = packed[i]
        let end = min(i + 8, packed.count)
        var j = i + 1
        var bit = 0
        while j < end {
            out.append(packed[j] | (((mask >> UInt8(bit)) & 1) == 1 ? 0x80 : 0x00))
            j += 1
            bit += 1
        }
        i += 8
    }
    return out
}

func parseSparkFrame(_ data: [UInt8]) -> SparkFrame? {
    var b = data
    if b.count > 16, b[0] == 0x01, b[1] == 0xFE { b = Array(b.dropFirst(16)) }
    guard let start = b.firstIndex(of: 0xF0), b.count >= start + 6, b[start + 1] == 0x01,
          let end = b.lastIndex(of: 0xF7), end > start + 5
    else { return nil }
    let packed = Array(b[(start + 6)..<end])
    return SparkFrame(cmd: b[start + 4], sub: b[start + 5],
                      packed: packed, payload: unpack7bit(packed))
}

/// Longest printable-ASCII run — the effect name in an 01 15 frame.
func asciiRun(_ bytes: [UInt8], minLength: Int = 3) -> String? {
    var best = "", cur = ""
    for byte in bytes {
        if byte > 0x20, byte < 0x7F {
            cur.append(Character(UnicodeScalar(byte)))
        } else {
            if cur.count > best.count { best = cur }
            cur = ""
        }
    }
    if cur.count > best.count { best = cur }
    return best.count >= minLength ? best : nil
}

/// Effect on/off flag. Determined empirically: the last 0x43/0x42 in the packed
/// payload is the state byte (0x43 = on, 0x42 = off). It sits after the effect
/// name, so a 'B'/'C' inside the name cannot be mistaken for it.
func effectEnabled(_ frame: SparkFrame) -> Bool? {
    guard let flag = frame.packed.last(where: { $0 == 0x42 || $0 == 0x43 }) else { return nil }
    return flag == 0x43
}

enum Control {
    case preset(Int)                  // 01 38 -> footswitch A..D
    case effect(String, Bool?)        // 01 15 -> footswitch E (drive toggle)
    case query(UInt8, UInt8)
    case unknown(UInt8, UInt8)
}

func classify(_ f: SparkFrame) -> Control {
    switch (f.cmd, f.sub) {
    case (0x01, 0x38):
        return .preset(Int(f.payload.last ?? 0))
    case (0x01, 0x15):
        return .effect(asciiRun(f.payload) ?? "effect", effectEnabled(f))
    case (0x02, _):
        return .query(f.cmd, f.sub)
    default:
        return .unknown(f.cmd, f.sub)
    }
}
