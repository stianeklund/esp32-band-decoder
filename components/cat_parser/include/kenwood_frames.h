#ifndef KENWOOD_FRAMES_H
#define KENWOOD_FRAMES_H

// Pure, dependency-free Kenwood (TS-590SG) CAT field decoders.
//
// Like yaesu_frames.h, this header pulls in NO ESP-IDF headers so the field-offset
// logic can be unit-tested off-target with a plain host g++ build. KenwoodCat calls
// these and adds the stateful glue (set_transmitting, handle_frequency_change,
// VFO bookkeeping, logging); the wire-format knowledge lives here where it can be
// tested in isolation.
//
// All inputs are the command PAYLOAD: the 2-char command code ('FA', 'IF', ...) and
// the trailing ';' have already been stripped by the base framing/dispatch.

#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>

namespace kenwood {

// TS-590SG operating-mode enum (IF byte 27) -> display string. Cosmetic only.
inline const char* mode_string(char mode_char) {
    switch (mode_char) {
        case '1': return "LSB";
        case '2': return "USB";
        case '3': return "CW-U";
        case '4': return "FM";
        case '5': return "AM";
        case '6': return "DIG-L";
        case '7': return "CW-L";
        case '9': return "DIG-U";
        default:  return "UNKNOWN";
    }
}

// Parse a bare numeric payload (FA/FB frequency body, AP port count: decimal digits
// only, leading zeros allowed). Returns nullopt unless the ENTIRE payload is a valid
// number that fits uint32_t.
inline std::optional<uint32_t> parse_u32(std::string_view digits) {
    uint32_t value;
    const char* const end = digits.data() + digits.size();
    const auto r = std::from_chars(digits.data(), end, value);
    if (r.ec == std::errc() && r.ptr == end) {
        return value;
    }
    return std::nullopt;
}

// Decoded IF status frame.
struct IfFrame {
    uint32_t frequency;  // bytes [0..10], Hz
    bool transmitting;   // byte 26 == '1'  (TS-590SG TX/RX flag)
    char mode_char;      // byte 27 (feed to mode_string)
    uint8_t vfo;         // byte 28 - '0': 0 = VFO A, 1 = VFO B, 2 = Memory
};

// Decode a Kenwood IF answer payload ('IF' + ';' already stripped). The TS-590SG
// frame is fixed-width; require at least 35 chars so bytes 26/27/28 are present.
// Unlike Yaesu, the TX state lives IN this frame (byte 26). Returns nullopt if the
// frame is too short or the frequency field is not numeric.
inline std::optional<IfFrame> parse_if(std::string_view payload) {
    if (payload.size() < 35) {
        return std::nullopt;
    }
    const auto freq = parse_u32(payload.substr(0, 11));
    if (!freq) {
        return std::nullopt;
    }
    return IfFrame{
        *freq,
        payload[26] == '1',
        payload[27],
        static_cast<uint8_t>(payload[28] - '0'),
    };
}

} // namespace kenwood

#endif // KENWOOD_FRAMES_H
