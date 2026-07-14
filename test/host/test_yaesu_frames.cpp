// Host-side unit test for the pure Yaesu CAT field decoders (yaesu_frames.h).
//
// Builds off-target with a plain C++17 compiler - no ESP-IDF, no hardware.
// From the repo root:
//
//   g++ -std=c++17 -Wall -Wextra -I components/cat_parser/include test/host/test_yaesu_frames.cpp -o /tmp/test_yaesu_frames && /tmp/test_yaesu_frames
//
// It tests the SAME code YaesuCat calls in firmware (the parser just adds stateful
// glue on top), so these fixtures guard the wire-format field offsets against drift.

#include "yaesu_frames.h"

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

// Build a canonical 25-char FT-891 IF payload ('IF' and ';' already stripped) from
// its parts, so the offsets are asserted by construction (P1=3, freq=9@3, mode@19).
static std::string make_if(const std::string& mem3, const std::string& freq9,
                           char mode, char vfo_status = '0') {
    std::string p;
    p += mem3;          // [0..2]  P1 memory channel
    p += freq9;         // [3..11] P2 frequency (9)
    p += '+';           // [12]    +/-
    p += "0000";        // [13..16]P3 clarifier offset
    p += '0';           // [17]    P4 clarifier on/off
    p += '0';           // [18]    P5 fixed 0
    p += mode;          // [19]    P6 mode
    p += vfo_status;    // [20]    P7 VFO/memory status
    p += '0';           // [21]    P8 CTCSS
    p += "00";          // [22..23]P9 fixed 00
    p += '0';           // [24]    P10 duplex
    return p;
}

static void test_tx_intent() {
    using yaesu::TxIntent;
    CHECK(yaesu::tx_intent("0") == TxIntent::Rx);
    CHECK(yaesu::tx_intent("1") == TxIntent::Tx);   // CAT-initiated TX
    CHECK(yaesu::tx_intent("2") == TxIntent::Tx);   // radio/PTT-initiated TX
    CHECK(yaesu::tx_intent("")  == TxIntent::NoChange);
    CHECK(yaesu::tx_intent("9") == TxIntent::NoChange);
    CHECK(yaesu::tx_intent("X") == TxIntent::NoChange);
}

static void test_mode_string() {
    CHECK(std::string_view(yaesu::mode_string('1')) == "LSB");
    CHECK(std::string_view(yaesu::mode_string('2')) == "USB");
    CHECK(std::string_view(yaesu::mode_string('3')) == "CW-U");
    CHECK(std::string_view(yaesu::mode_string('4')) == "FM");
    CHECK(std::string_view(yaesu::mode_string('5')) == "AM");
    CHECK(std::string_view(yaesu::mode_string('B')) == "FM-N");
    CHECK(std::string_view(yaesu::mode_string('D')) == "AM-N");
    CHECK(std::string_view(yaesu::mode_string('Z')) == "UNKNOWN");
}

static void test_parse_frequency() {
    // 9-digit FA/FB body, leading zeros allowed.
    CHECK(yaesu::parse_frequency("014250000") == std::optional<uint32_t>(14250000));
    CHECK(yaesu::parse_frequency("050313000") == std::optional<uint32_t>(50313000)); // 6m
    CHECK(yaesu::parse_frequency("000030000") == std::optional<uint32_t>(30000));     // low edge
    // Whole payload must be numeric; partial parse must fail.
    CHECK(yaesu::parse_frequency("12a45") == std::nullopt);
    CHECK(yaesu::parse_frequency("") == std::nullopt);
    // Out of uint32_t range -> failure, not a truncated value.
    CHECK(yaesu::parse_frequency("9999999999") == std::nullopt);
}

static void test_parse_if() {
    // Nominal: 14.250 MHz USB. Frequency must come from offset 3 (NOT include P1).
    const std::string ok = make_if("000", "014250000", '2');
    CHECK(ok.size() == 25);
    const auto f = yaesu::parse_if(ok);
    CHECK(f.has_value());
    if (f) {
        CHECK(f->frequency == 14250000);
        CHECK(f->mode_char == '2');
        CHECK(std::string_view(yaesu::mode_string(f->mode_char)) == "USB");
    }

    // A non-zero memory-channel P1 must not bleed into the frequency (offset proof).
    const std::string with_mem = make_if("123", "007000000", '1');
    const auto f2 = yaesu::parse_if(with_mem);
    CHECK(f2.has_value());
    if (f2) {
        CHECK(f2->frequency == 7000000);   // 7 MHz, not 1237000000
        CHECK(f2->mode_char == '1');
    }

    // Too short by one char -> reject (never read past the buffer).
    CHECK(yaesu::parse_if(ok.substr(0, 24)) == std::nullopt);

    // Non-numeric frequency field -> reject.
    const std::string bad_freq = make_if("000", "01425XX00", '2');
    CHECK(yaesu::parse_if(bad_freq) == std::nullopt);
}

static void test_parse_md_mode() {
    CHECK(yaesu::parse_md_mode("02") == std::optional<char>('2')); // MAIN RX + USB
    CHECK(yaesu::parse_md_mode("0B") == std::optional<char>('B')); // FM-N
    CHECK(yaesu::parse_md_mode("0")  == std::nullopt);             // too short
    CHECK(yaesu::parse_md_mode("")   == std::nullopt);
}

int main() {
    test_tx_intent();
    test_mode_string();
    test_parse_frequency();
    test_parse_if();
    test_parse_md_mode();

    if (g_failures == 0) {
        std::printf("OK: all Yaesu frame-decode tests passed\n");
        return 0;
    }
    std::printf("FAILED: %d check(s)\n", g_failures);
    return 1;
}
