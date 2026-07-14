#ifndef RELAY_CONTROLLER_H
#define RELAY_CONTROLLER_H

#include "esp_err.h"
#include "kc868_hw.h"
#include "antenna_switch.h"
#include <map>
#include <mutex>
#include "config_cache.h"

// Forward declaration
class CatParser;

class RelayController : public ConfigCache {
public:
    // Per-radio port/relay ceiling. Fixed at 8 on both boards: it is the Radio-A
    // port count, the Radio-B relay offset (relay 9 = Radio B port 1 on the A16),
    // and the modulo divisor in the interlock math. Do NOT derive it from
    // NUM_RELAYS -- on the A8 that would make it 4 and corrupt every A/B split.
    static constexpr int RELAYS_PER_RADIO = 8;
    // Total physical relays. A8 = 8 (Radio A only, relays 1-8); A16 = 16 (adds
    // Radio B on relays 9-16). With A8's NUM_RELAYS == RELAYS_PER_RADIO, the
    // Radio-B relay range (RELAYS_PER_RADIO..NUM_RELAYS) is empty.
    static constexpr int NUM_RELAYS = KC868_HW_NUM_RELAYS;


    // Delete copy constructor and assignment operator
    RelayController(const RelayController&) = delete;
    RelayController& operator=(const RelayController&) = delete;

    // Add singleton instance method
    static RelayController& instance() {
        static RelayController instance;
        return instance;
    }

    // Make destructor public but keep constructor private
    ~RelayController();

    esp_err_t init();
    esp_err_t turn_off_all_relays();
    esp_err_t set_relay(int relay_id, bool state);
    [[nodiscard]] bool get_relay_state(int relay_id) const;

    // Move the active antenna from one relay to another in a single hardware
    // write: off_relay goes low and on_relay goes high at the same instant, so the
    // antenna port is never momentarily disconnected (no gap where both are off).
    //
    // Two things make this different from set_relay():
    //  1. Atomic. set_relay() is one relay per call, so switching antennas needs
    //     two calls with a dead gap between them. This does both in one write.
    //  2. It is allowed to run during transmit. set_relay() refuses to move a
    //     transmitting radio's relay (the interlock that stops the web UI / MQTT /
    //     band changes from hot-switching under RF). But moving from the RX to the
    //     TX antenna at key-up IS the transmit action, so it must never be blocked
    //     by that interlock. Only AntennaSwitch's PTT key-up / key-down path calls
    //     this; every other caller still goes through the guarded set_relay().
    //
    // If the hardware write fails, it restores the previous relay and returns the
    // error, so a failed switch never leaves the port disconnected.
    //
    //   off_relay == 0 -> nothing to turn off (used to recover a port that is
    //                     currently on no relay at all).
    //   on_relay  == 0 -> rejected: this call must always end with an antenna live.
    esp_err_t swap_antenna_relays(int off_relay, int on_relay);

    esp_err_t set_relay_for_antenna(int relay_id, int band_number, RadioID radio, bool state);
    inline esp_err_t set_relay_for_antenna(int relay_id, int band_number, bool state) { // Default to Radio A
        return set_relay_for_antenna(relay_id, band_number, RadioID::A, state);
    }

    [[nodiscard]] int get_last_selected_relay_for_band(int band_number) const;
    [[nodiscard]] bool is_correct_relay_set(int band_number) const;
    [[nodiscard]] int get_currently_selected_relay() const { return currently_selected_relay_; }
    [[nodiscard]] uint16_t get_relay_states() const;

private:
    // Members initialized in constructor, in order of initialization list
    int currently_selected_relay_;
    CatParser& cat_parser_;  // Reference to check transmit status

    // Other members
    std::map<int,int> last_selected_relay_for_band_[2]; // per-radio: band -> last relay
    mutable std::mutex relay_mutex_;

    // Private methods
    RelayController(); // Constructor
    bool is_transmitting() const;
    esp_err_t execute_relay_change(int relay_id, int band_number, RadioID radio, bool state);
};

#endif // RELAY_CONTROLLER_H
