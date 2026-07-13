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
