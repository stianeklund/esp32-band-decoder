// Host-side unit test for the pure Kenwood CAT field decoders (kenwood_frames.h).
//
// Builds off-target with a plain C++17 compiler - no ESP-IDF, no hardware.
// From the repo root:
//
//   g++ -std=c++17 -Wall -Wextra -I components/cat_parser/include test/host/test_kenwood_frames.cpp -o /tmp/test_kenwood_frames && /tmp/test_kenwood_frames
//
// It tests the SAME code KenwoodCat calls in firmware (the parser just adds stateful
// glue on top), so these fixtures guard the wire-format field offsets against drift.
// The safety-relevant one is the IF TX flag at byte 26.

#include "kenwood_frames.h"

#include <cstdio>
#include <string>
#include <string_view>

static int g_failures = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);  \
            ++g_failures;                                                \
        }                                                                \
    } while (0)

// Build a 35-char TS-590SG IF payload ('IF' and ';' already stripped) with the
// fields our decoder reads placed at their exact offsets: freq [0..10], TX [26],
// mode [27], VFO [28]. The intermediate bytes are filler our decoder ignores.
static std::string make_if(const std::string& freq11, char tx, char mode, char vfo) {
    std::string p;
    p += freq11;              // [0..10]  P1 frequency (11)
    p += "000000000000000";   // [11..25] intermediate fields (15, filler)
    p += tx;                  // [26]     TX/RX flag
    p += mode;                // [27]     mode
    p += vfo;                 // [28]     VFO selection
    p += "000000";            // [29..34] trailing fields (6) -> length 35
    return p;
}

static void test_mode_string() {
    CHECK(std::string_view(kenwood::mode_string('1')) == "LSB");
    CHECK(std::string_view(kenwood::mode_string('2')) == "USB");
    CHECK(std::string_view(kenwood::mode_string('3')) == "CW-U");
    CHECK(std::string_view(kenwood::mode_string('4')) == "FM");
    CHECK(std::string_view(kenwood::mode_string('5')) == "AM");
    CHECK(std::string_view(kenwood::mode_string('6')) == "DIG-L");
    CHECK(std::string_view(kenwood::mode_string('7')) == "CW-L");
    CHECK(std::string_view(kenwood::mode_string('9')) == "DIG-U");
    CHECK(std::string_view(kenwood::mode_string('8')) == "UNKNOWN"); // no '8' mode
    CHECK(std::string_view(kenwood::mode_string('Z')) == "UNKNOWN");
}

static void test_parse_u32() {
    // 11-digit FA/FB frequency body, leading zeros allowed.
    CHECK(kenwood::parse_u32("00014250000") == std::optional<uint32_t>(14250000));
    CHECK(kenwood::parse_u32("00000030000") == std::optional<uint32_t>(30000));
    CHECK(kenwood::parse_u32("12a") == std::nullopt);
    CHECK(kenwood::parse_u32("") == std::nullopt);
    // 11 nines = 99,999,999,999 > UINT32_MAX -> reject, not truncate.
    CHECK(kenwood::parse_u32("99999999999") == std::nullopt);
}

static void test_parse_if() {
    // Nominal: 14.250 MHz, transmitting, USB, VFO A.
    const std::string tx_frame = make_if("00014250000", '1', '2', '0');
    CHECK(tx_frame.size() == 35);
    const auto f = kenwood::parse_if(tx_frame);
    CHECK(f.has_value());
    if (f) {
        CHECK(f->frequency == 14250000);
        CHECK(f->transmitting == true);   // byte 26 == '1'
        CHECK(f->mode_char == '2');
        CHECK(f->vfo == 0);
        CHECK(std::string_view(kenwood::mode_string(f->mode_char)) == "USB");
    }

    // Receiving, CW-U, VFO B.
    const auto r = kenwood::parse_if(make_if("00007000000", '0', '3', '1'));
    CHECK(r.has_value());
    if (r) {
        CHECK(r->frequency == 7000000);
        CHECK(r->transmitting == false);  // byte 26 == '0'
        CHECK(r->mode_char == '3');
        CHECK(r->vfo == 1);
    }

    // Memory VFO selector (byte 28 == '2').
    const auto m = kenwood::parse_if(make_if("00014074000", '0', '9', '2'));
    CHECK(m.has_value());
    if (m) {
        CHECK(m->vfo == 2);
    }

    // One byte too short -> reject (bytes 26/27/28 must be present).
    CHECK(kenwood::parse_if(tx_frame.substr(0, 34)) == std::nullopt);

    // Non-numeric frequency field -> reject.
    CHECK(kenwood::parse_if(make_if("000142XX000", '0', '2', '0')) == std::nullopt);
}

int main() {
    test_mode_string();
    test_parse_u32();
    test_parse_if();

    if (g_failures == 0) {
        std::printf("OK: all Kenwood frame-decode tests passed\n");
        return 0;
    }
    std::printf("FAILED: %d check(s)\n", g_failures);
    return 1;
}
