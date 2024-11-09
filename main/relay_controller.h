#ifndef RELAY_CONTROLLER_H
#define RELAY_CONTROLLER_H

#include "esp_err.h"
#include "kc868_a16_hw.h"
#include <cstdint>
#include <map>
#include <chrono>
#include <mutex>

class RelayController {
public:
    static constexpr int NUM_RELAYS = 16;
    static constexpr int COOLDOWN_PERIOD_MS = 50;

    RelayController();
    ~RelayController();

    esp_err_t init();
    esp_err_t turn_off_all_relays();
    esp_err_t set_relay(int relay_id, bool state);
    bool get_relay_state(int relay_id) const;
    esp_err_t set_relay_for_antenna(int relay_id, int band_number);
    esp_err_t turn_off_all_relays_except(int relay_to_keep_on);
    
    int get_last_selected_relay_for_band(int band_number) const;
    bool is_correct_relay_set(int band_number) const;
    int get_currently_selected_relay() const { return currently_selected_relay_; }

private:
    std::map<int, int> last_selected_relay_for_band_;
    int currently_selected_relay_;
    std::mutex relay_mutex_;
    std::chrono::steady_clock::time_point last_relay_change_;
    
    bool should_delay() const;
    esp_err_t execute_relay_change(int relay_id, int band_number);
};

#endif // RELAY_CONTROLLER_H
