#ifndef RELAY_CONTROLLER_H
#define RELAY_CONTROLLER_H

#include "esp_err.h"
#include "kc868_a16_hw.h"
#include "antenna_switch.h"
#include <map>
#include <mutex>
#include <chrono>
#include "config_cache.h"

// Forward declaration
class CatParser;

class RelayController : public ConfigCache {
public:
    static constexpr int NUM_RELAYS = 16; // Total number of relays
    static constexpr int COOLDOWN_PERIOD_MS = 50;
    static constexpr int RELAYS_PER_RADIO = NUM_RELAYS / 2;


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
    [[nodiscard]] std::map<int, bool> get_all_relay_states() const;

    esp_err_t update_all_relay_states();

    esp_err_t set_relay_for_antenna(int relay_id, int band_number, RadioID radio, bool state);
    inline esp_err_t set_relay_for_antenna(int relay_id, int band_number, bool state) { // Default to Radio A
        return set_relay_for_antenna(relay_id, band_number, RadioID::A, state);
    }
    esp_err_t turn_off_all_relays_except(int relay_to_keep_on);
    
    [[nodiscard]] int get_last_selected_relay_for_band(int band_number) const;
    [[nodiscard]] bool is_correct_relay_set(int band_number) const;
    [[nodiscard]] int get_currently_selected_relay() const { return currently_selected_relay_; }
    [[nodiscard]] uint16_t get_relay_states() const;

private:
    // Members initialized in constructor, in order of initialization list
    int currently_selected_relay_;
    std::chrono::steady_clock::time_point last_relay_change_;
    CatParser& cat_parser_;  // Reference to check transmit status
    uint8_t current_mask_[2]; // Relay masks for Radio A (index 0) and B (index 1) - each mask is 8 bits

    // Other members
    std::map<int,int> last_selected_relay_for_band_[2]; // same indexing
    std::map<int, bool> relay_states_;
    std::mutex relay_mutex_;

    // Private methods
    // [[nodiscard]] bool is_transmitting() const; // This will be removed, AntennaSwitch is now the authority
    RelayController(); // Constructor
    [[nodiscard]] bool should_delay() const;
    bool is_transmitting() const;
    esp_err_t execute_relay_change(int relay_id, int band_number, RadioID radio, bool state);
};

#endif // RELAY_CONTROLLER_H
