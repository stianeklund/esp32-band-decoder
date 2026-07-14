// Host-side unit test for the pure PTT antenna-swap decision logic
// (antenna_swap_logic.h). Builds off-target with a plain C++17 compiler - no
// ESP-IDF, no hardware. Build command (all one line):
//
//   g++ -std=c++17 -Wall -Wextra -I components/antenna_switch/include test/host/test_antenna_swap_logic.cpp -o /tmp/t && /tmp/t
//
// It exercises the SAME code the firmware calls in apply_radio_a_rx_to_tx_swap()
// and on_radio_a_tx_stop(): choose_tx_relay() (never transmit into the RX-only
// antenna) and plan_relay_swap() (recover a disconnected port). The final block
// replays the exact field-log failure - PTT toggling while CAT is stuck "on" -
// through a model of the atomic single-write swap and proves the port is NEVER
// left with zero relays energized.

#include "antenna_swap_logic.h"

#include <cstdint>
#include <cstdio>

using namespace antenna_logic;

static int g_failures = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);  \
            ++g_failures;                                                \
        }                                                                \
    } while (0)

static void test_choose_tx_relay() {
    // 6 ports, all enabled, RX antenna on relay 6.
    const bool ports_all[6] = {true, true, true, true, true, true};

    // Valid preferred (5) that is not the RX antenna -> honored.
    CHECK(choose_tx_relay(/*rx*/ 6, /*preferred*/ 5, 6, ports_all) == 5);

    // Preferred == RX antenna -> must be rejected; first non-RX port (1) chosen.
    CHECK(choose_tx_relay(/*rx*/ 6, /*preferred*/ 6, 6, ports_all) == 1);

    // No preference -> first available non-RX port.
    CHECK(choose_tx_relay(/*rx*/ 6, /*preferred*/ 0, 6, ports_all) == 1);

    // Preferred out of range / disabled -> fall back to scan.
    const bool ports_gap[6] = {false, false, true, false, false, true};
    CHECK(choose_tx_relay(/*rx*/ 6, /*preferred*/ 2, 6, ports_gap) == 3); // 2 disabled -> 3
    CHECK(choose_tx_relay(/*rx*/ 3, /*preferred*/ 0, 6, ports_gap) == 6); // only 3 and 6; 3 is RX -> 6

    // Only the RX antenna is available -> no distinct TX antenna (0).
    const bool ports_only_rx[6] = {false, false, false, false, false, true};
    CHECK(choose_tx_relay(/*rx*/ 6, /*preferred*/ 0, 6, ports_only_rx) == 0);
    CHECK(choose_tx_relay(/*rx*/ 6, /*preferred*/ 6, 6, ports_only_rx) == 0);

    // Degenerate inputs never crash / never pick something bogus.
    CHECK(choose_tx_relay(6, 0, 0, ports_all) == 0);
    CHECK(choose_tx_relay(6, 0, 6, nullptr) == 0);
}

static void test_plan_relay_swap() {
    // Normal RX(6) -> TX(5).
    auto p = plan_relay_swap(/*current*/ 6, /*target*/ 5);
    CHECK(p.needed && p.off_relay == 6 && p.on_relay == 5);

    // Self-heal: port stranded (0) -> still energize the target, nothing to turn off.
    p = plan_relay_swap(/*current*/ 0, /*target*/ 5);
    CHECK(p.needed && p.off_relay == 0 && p.on_relay == 5);

    // Already on target -> no-op.
    p = plan_relay_swap(/*current*/ 5, /*target*/ 5);
    CHECK(!p.needed);

    // No valid target -> no-op (never turns the current antenna off into nothing).
    p = plan_relay_swap(/*current*/ 5, /*target*/ 0);
    CHECK(!p.needed);

    // On some unexpected relay -> swap to the intended target.
    p = plan_relay_swap(/*current*/ 3, /*target*/ 5);
    CHECK(p.needed && p.off_relay == 3 && p.on_relay == 5);
}

// --- Model of RelayController::swap_antenna_relays's single atomic write. ---
// new_outputs = (current & ~off_bit) | on_bit, applied in one shot. Returns the
// new 16-bit relay mask (1 = relay ON). This is exactly the mask math the firmware
// uses, so if this can never produce an all-off Radio-A nibble, neither can the HW.
static uint16_t apply_atomic_swap(uint16_t current, const RelaySwap& plan) {
    if (!plan.needed) {
        return current;
    }
    uint16_t new_outputs = current;
    if (plan.off_relay != 0) {
        new_outputs = static_cast<uint16_t>(new_outputs & ~(1u << (plan.off_relay - 1)));
    }
    new_outputs = static_cast<uint16_t>(new_outputs | (1u << (plan.on_relay - 1)));
    return new_outputs;
}

// Count active relays in the Radio A range (relays 1..8 -> bits 0..7).
static int radio_a_active_count(uint16_t mask) {
    int n = 0;
    for (int b = 0; b < 8; b++) {
        if (mask & (1u << b)) {
            ++n;
        }
    }
    return n;
}
static int radio_a_active_relay(uint16_t mask) {
    for (int b = 0; b < 8; b++) {
        if (mask & (1u << b)) {
            return b + 1;
        }
    }
    return 0;
}

// Replay the field-log failure: many PTT key-up/key-down cycles while CAT is
// permanently stuck "on". Under the old two-write swap a blocked second write
// stranded the port at zero relays. With the atomic swap + self-heal, prove that
// after EVERY edge exactly one Radio-A relay is energized, and it is the correct
// one (TX on key-up, RX on key-down) - the port is never dead.
static void test_stuck_cat_ptt_cycle_never_strands_port() {
    const int RX = 6, TX = 5;
    const bool ports[6] = {true, true, true, true, true, true};

    // Start on the RX antenna, as the radio does at rest.
    uint16_t relays = static_cast<uint16_t>(1u << (RX - 1));
    CHECK(radio_a_active_relay(relays) == RX);

    // Even to simulate a worst-case start-from-stranded on the first key-up, seed
    // one iteration from an already-dead port and confirm it heals.
    uint16_t stranded = 0;
    {
        const int tx = choose_tx_relay(RX, /*preferred*/ TX, 6, ports);
        stranded = apply_atomic_swap(stranded, plan_relay_swap(radio_a_active_relay(stranded), tx));
        CHECK(radio_a_active_count(stranded) == 1);
        CHECK(radio_a_active_relay(stranded) == TX); // recovered onto TX
    }

    // 100 full PTT cycles. CAT is "stuck on" the entire time; it is irrelevant to
    // the swap because the swap is PTT-authoritative and trusted (never blocked).
    for (int i = 0; i < 100; i++) {
        // Key up: swap to TX.
        const int tx = choose_tx_relay(RX, /*preferred*/ TX, 6, ports);
        CHECK(tx == TX);
        relays = apply_atomic_swap(relays, plan_relay_swap(radio_a_active_relay(relays), tx));
        CHECK(radio_a_active_count(relays) == 1); // never zero, never two
        CHECK(radio_a_active_relay(relays) == TX);

        // Key down: swap back to RX.
        relays = apply_atomic_swap(relays, plan_relay_swap(radio_a_active_relay(relays), RX));
        CHECK(radio_a_active_count(relays) == 1); // never zero, never two
        CHECK(radio_a_active_relay(relays) == RX);
    }
}

int main() {
    test_choose_tx_relay();
    test_plan_relay_swap();
    test_stuck_cat_ptt_cycle_never_strands_port();

    if (g_failures == 0) {
        std::printf("OK: all antenna swap-logic tests passed\n");
        return 0;
    }
    std::printf("FAILED: %d check(s)\n", g_failures);
    return 1;
}
