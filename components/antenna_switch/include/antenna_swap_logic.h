#ifndef ANTENNA_SWAP_LOGIC_H
#define ANTENNA_SWAP_LOGIC_H

// Decisions for the PTT antenna swap, kept as plain functions with no ESP-IDF or
// hardware dependencies. This lets the host unit tests run the exact same code the
// firmware runs (see test/host/test_antenna_swap_logic.cpp), just like
// kenwood_frames.h does for the CAT decoders.
//
// There are two decisions here:
//   1. choose_tx_relay: which relay is the transmit antenna for a band. It must
//      never pick the receive-only antenna -- transmitting into an RX antenna can
//      destroy the receiver front-end / preamp.
//   2. plan_relay_swap: how to get from the relay that is on now to the relay we
//      want on. It also handles the case where NO relay is on (the port got left
//      disconnected) by simply switching the wanted relay on.

#include <cstdint>

namespace antenna_logic {

// Pick the transmit-antenna relay for a band.
//
// Prefers the band's last-used transmit antenna if it is still a valid choice,
// otherwise takes the first enabled port that is not the receive antenna. It will
// never return rx_relay, because keying up into a receive-only antenna can damage
// the radio.
//
//   rx_relay            relay number (1-based) of the receive antenna (0 = none)
//   preferred           relay number (1-based) last used for TX on this band (0 = none)
//   num_antenna_ports   how many entries of antenna_ports[] are valid
//   antenna_ports       enabled flag per port; antenna_ports[0] is relay 1
//
// Returns the transmit relay number (1-based), or 0 if the band has no transmit
// antenna that is different from the receive antenna.
inline int choose_tx_relay(int rx_relay, uint8_t preferred, int num_antenna_ports,
                           const bool *antenna_ports) {
    if (antenna_ports == nullptr || num_antenna_ports <= 0) {
        return 0;
    }
    if (preferred != 0 && static_cast<int>(preferred) != rx_relay &&
        static_cast<int>(preferred) <= num_antenna_ports && antenna_ports[preferred - 1]) {
        return preferred;
    }
    for (int j = 0; j < num_antenna_ports; j++) {
        if (antenna_ports[j] && (j + 1) != rx_relay) {
            return j + 1;
        }
    }
    return 0;
}

// The plan for one antenna move: turn off_relay off and on_relay on. off_relay of
// 0 means there is nothing to turn off (no relay was on), so the move just turns
// on_relay on. When needed is false, nothing should be done -- we are already on
// the relay we want, or there is no valid relay to switch to.
struct RelaySwap {
    int off_relay; // relay to turn off (0 = none is on)
    int on_relay;  // relay to turn on (the one we want)
    bool needed;   // false = already correct, or no valid target: do nothing
};

// Work out how to get onto target_relay from whatever relay is on now.
//   target_relay  == 0            -> no relay to switch to: do nothing.
//   current_relay == target_relay -> already there: do nothing.
//   current_relay == 0            -> nothing is on: just switch target on.
//   otherwise                     -> turn current off and target on together.
// The current_relay == 0 case is what lets a disconnected port fix itself on the
// next key-up/key-down instead of staying dead.
inline RelaySwap plan_relay_swap(int current_relay, int target_relay) {
    if (target_relay == 0) {
        return {0, 0, false};
    }
    if (current_relay == target_relay) {
        return {0, 0, false};
    }
    return {current_relay, target_relay, true};
}

} // namespace antenna_logic

#endif // ANTENNA_SWAP_LOGIC_H
