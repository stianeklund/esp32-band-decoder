#ifndef YAESU_FRAMES_H
#define YAESU_FRAMES_H

// Pure, dependency-free Yaesu CAT field decoders.
//
// This header intentionally pulls in NO ESP-IDF headers so the field-offset logic
// can be unit-tested off-target with a plain host g++ build. YaesuCat calls these
// functions and adds the stateful glue (set_transmitting, handle_frequency_change,
// logging); the wire-format knowledge lives here where it can be tested in isolation.
//
// All inputs are the command PAYLOAD: the 2-char command code ('FA', 'IF', ...) and
// the trailing ';' have already been stripped by the base framing/dispatch.

#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>

namespace yaesu {

// TX command digit -> transmit intent. '0' = receive, '1' (CAT) / '2' (PTT) =
// transmit, anything else (including an empty payload) = leave state unchanged so
// garbage can never silently drop an asserted TX.
enum class TxIntent { Rx, Tx, NoChange };

inline TxIntent tx_intent(std::string_view payload) {
    if (payload.empty()) {
        return TxIntent::NoChange;
    }
    switch (payload[0]) {
        case '0': return TxIntent::Rx;
        case '1':
        case '2': return TxIntent::Tx;
        default:  return TxIntent::NoChange;
    }
}

// FT-891 operating-mode enum (IF P6 / MD P2) -> display string. Cosmetic only
// (feeds get_mode(); not used for band decode).
inline const char* mode_string(char mode_char) {
    switch (mode_char) {
        case '1': return "LSB";
        case '2': return "USB";
        case '3': return "CW-U";
        case '4': return "FM";
        case '5': return "AM";
        case '6': return "RTTY-L";
        case '7': return "CW-L";
        case '8': return "DATA-U";
        case '9': return "RTTY-U";
        case 'B': return "FM-N";
        case 'C': return "DATA-L";
        case 'D': return "AM-N";
        default:  return "UNKNOWN";
    }
}

// Parse a bare frequency payload (FA/FB body: decimal digits only, leading zeros
// allowed). Returns nullopt unless the ENTIRE payload is a valid number.
inline std::optional<uint32_t> parse_frequency(std::string_view digits) {
    uint32_t freq;
    const char* const end = digits.data() + digits.size();
    const auto r = std::from_chars(digits.data(), end, freq);
    if (r.ec == std::errc() && r.ptr == end) {
        return freq;
    }
    return std::nullopt;
}

// Decoded IF status frame.
struct IfFrame {
    uint32_t frequency; // P2, Hz
    char mode_char;     // P6 (feed to mode_string)
};

// Decode a Yaesu IF answer payload. Layout (0-based, 'IF' + ';' already stripped),
// 25 chars: [0..2] P1 mem-ch, [3..11] P2 freq(9), [12] +/-, [13..16] P3 clar,
// [17] P4, [18] P5, [19] P6 mode, [20] P7, [21] P8, [22..23] P9, [24] P10.
// There is NO TX field here (unlike Kenwood). Returns nullopt if too short or the
// frequency field is not numeric.
inline std::optional<IfFrame> parse_if(std::string_view payload) {
    if (payload.size() < 25) {
        return std::nullopt;
    }
    const auto freq = parse_frequency(payload.substr(3, 9));
    if (!freq) {
        return std::nullopt;
    }
    return IfFrame{*freq, payload[19]};
}

// Decode a Yaesu MD answer payload ("0" + mode char). Returns the mode char, or
// nullopt if too short.
inline std::optional<char> parse_md_mode(std::string_view payload) {
    if (payload.size() < 2) {
        return std::nullopt;
    }
    return payload[1];
}

} // namespace yaesu

#endif // YAESU_FRAMES_H
