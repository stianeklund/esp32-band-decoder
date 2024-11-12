#include "relay_controller.h"
#include "esp_log.h"
#include "freertos/task.h"
#include <chrono>

static const char* TAG = "RELAY_CONTROLLER";

RelayController::RelayController()
    : currently_selected_relay_(0), 
      last_relay_change_(std::chrono::steady_clock::now()) {
}

RelayController::~RelayController() = default;

esp_err_t RelayController::init() {
    ESP_LOGI(TAG, "Initializing relay controller");
    
    esp_err_t ret = kc868_a16_hw_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize KC868-A16 hardware: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = turn_off_all_relays();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize relay states: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Relay controller initialized successfully");
    return ESP_OK;
}

esp_err_t RelayController::turn_off_all_relays() {
    ESP_LOGD(TAG, "Turning off all relays");
    
    std::lock_guard<std::mutex> lock(relay_mutex_);
    
    esp_err_t ret = kc868_a16_set_all_outputs(0);
    if (ret == ESP_OK) {
        currently_selected_relay_ = 0;
        last_relay_change_ = std::chrono::steady_clock::now();
    }
    return ret;
}

esp_err_t RelayController::set_relay(const int relay_id, const bool state) {
    if (relay_id < 1 || relay_id > NUM_RELAYS) {
        ESP_LOGE(TAG, "Invalid relay ID: %d", relay_id);
        return ESP_ERR_INVALID_ARG;
    }

    std::lock_guard<std::mutex> lock(relay_mutex_);
    
    // Convert from 1-based to 0-based index for hardware
    const uint8_t hw_relay = relay_id - 1;
    
    ESP_LOGD(TAG, "Setting relay %d (hw: %d) to state %d", relay_id, hw_relay, state);
    
    if (should_delay()) {
        vTaskDelay(pdMS_TO_TICKS(COOLDOWN_PERIOD_MS));
    }

    // Note: kc868_a16_set_output handles the active-low conversion internally
    esp_err_t ret = kc868_a16_set_output(hw_relay, state);
    if (ret == ESP_OK) {
        last_relay_change_ = std::chrono::steady_clock::now();
        // Update our internal state tracking with the logical state (not inverted)
        relay_states_[relay_id] = state;
        
        // Update currently_selected_relay_ only if this is part of antenna selection
        if (state) {
            currently_selected_relay_ = relay_id;
        } else if (currently_selected_relay_ == relay_id) {
            currently_selected_relay_ = 0;
        }
    }
    return ret;
}

bool RelayController::get_relay_state(const int relay_id) const {
    if (relay_id < 1 || relay_id > NUM_RELAYS) {
        ESP_LOGE(TAG, "Invalid relay ID: %d", relay_id);
        return false;
    }

    bool state;
    if (kc868_a16_get_output_state(relay_id - 1, &state) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get relay state");
        return false;
    }
    return state;
}

std::map<int, bool> RelayController::get_all_relay_states() const {
    return relay_states_;
}

esp_err_t RelayController::update_all_relay_states() {
    ESP_LOGD(TAG, "Getting state of all relays");

    const uint16_t current_outputs = kc868_a16_get_all_outputs();
    
    // Update relay states map
    uint16_t mask = 1;
    for (int i = 1; i <= NUM_RELAYS; ++i, mask <<= 1) {
        relay_states_[i] = (current_outputs & mask) != 0;
    }

    // Log current states if debug logging enabled
    if (esp_log_level_get(TAG) >= ESP_LOG_DEBUG) {
        std::stringstream ss;
        ss << "Current relay states: ";
        for (int i = 1; i <= NUM_RELAYS; ++i) {
            ss << i << ":" << (relay_states_[i] ? "ON" : "off") << " ";
        }
        ESP_LOGD(TAG, "%s", ss.str().c_str());
    }

    return ESP_OK;
}



uint16_t RelayController::get_relay_states() const {
    uint16_t raw_states = kc868_a16_get_all_outputs();
    ESP_LOGD(TAG, "Raw states from hardware: 0x%04X", raw_states);
    uint16_t states = ~raw_states & 0xFFFF;
    ESP_LOGD(TAG, "Inverted states: 0x%04X", states);
    return states;
}

esp_err_t RelayController::turn_off_all_relays_except(const int relay_to_keep_on) {
    if (relay_to_keep_on < 1 || relay_to_keep_on > NUM_RELAYS) {
        ESP_LOGE(TAG, "Invalid relay ID: %d", relay_to_keep_on);
        return ESP_ERR_INVALID_ARG;
    }

    std::lock_guard<std::mutex> lock(relay_mutex_);

    if (should_delay()) {
        vTaskDelay(pdMS_TO_TICKS(COOLDOWN_PERIOD_MS));
    }

    const uint16_t relay_mask = 1 << (relay_to_keep_on - 1);
    const esp_err_t ret = kc868_a16_set_all_outputs(relay_mask);
    
    if (ret == ESP_OK) {
        currently_selected_relay_ = relay_to_keep_on;
        relay_states_[relay_to_keep_on] = true;
        last_relay_change_ = std::chrono::steady_clock::now();
        ESP_LOGI(TAG, "All relays turned off except relay %d", relay_to_keep_on);
    }
    
    return ret;
}

int RelayController::get_last_selected_relay_for_band(const int band_number) const {
    if (const auto it = last_selected_relay_for_band_.find(band_number); it != last_selected_relay_for_band_.end()) {
        return it->second;
    }
    return 0;
}

bool RelayController::is_correct_relay_set(int band_number) const {
    int last_selected_relay = get_last_selected_relay_for_band(band_number);
    return currently_selected_relay_ == last_selected_relay && last_selected_relay != 0;
}

bool RelayController::should_delay() const {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now - last_relay_change_).count() < COOLDOWN_PERIOD_MS;
}


 esp_err_t RelayController::execute_relay_change(const int relay_id, const int band_number) {
     std::lock_guard<std::mutex> lock(relay_mutex_);

     // If we're already on the correct relay, no need to change
     if (relay_id == currently_selected_relay_) {
         ESP_LOGI(TAG, "Relay %d already selected", relay_id);
         last_selected_relay_for_band_[band_number] = relay_id;
         return ESP_OK;
     }

     if (should_delay()) {
         vTaskDelay(pdMS_TO_TICKS(COOLDOWN_PERIOD_MS));
     }

     const uint16_t relay_mask = 1 << (relay_id - 1);
     esp_err_t ret = kc868_a16_set_all_outputs(relay_mask);

     if (ret == ESP_OK) {
         currently_selected_relay_ = relay_id;
         relay_states_[relay_id] = true;
         last_relay_change_ = std::chrono::steady_clock::now();
         last_selected_relay_for_band_[band_number] = relay_id;
         ESP_LOGI(TAG, "Successfully changed to relay %d for band %d", relay_id, band_number);
     }

     return ret;
 }

esp_err_t RelayController::set_relay_for_antenna(int relay_id, int band_number) {
    if (relay_id < 1 || relay_id > NUM_RELAYS) {
        ESP_LOGE(TAG, "Invalid relay ID: %d", relay_id);
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGD(TAG, "Calling Execute relay change");

    return execute_relay_change(relay_id, band_number);
}

