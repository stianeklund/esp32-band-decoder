#include "relay_controller.h"
#include "antenna_switch.h"
#include "websocket_server.h"
#include "config_manager.h"
#include "my_mqtt_client.h"
#include "cat_parser.h"
#include "esp_log.h"
#include "esp_timer.h" // Added for esp_timer_get_time()
#include "freertos/task.h"
#include <chrono>
#include "config_cache.h"

static constexpr const char* TAG = "RELAY_CONTROLLER";

RelayController::RelayController()
    : currently_selected_relay_(0),
      last_relay_change_(std::chrono::steady_clock::now()),
      cat_parser_(CatParser::instance()),
      current_mask_{0, 0} { // Initialize masks to all off

    esp_log_level_set(TAG, ESP_LOG_WARN);
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
    
    std::lock_guard lock(relay_mutex_);
    
    // Check if radio is transmitting - now protected by mutex
    if (cat_parser_.is_transmitting()) {
        ESP_LOGW(TAG, "Cannot change relays while transmitting");
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t ret = kc868_a16_set_all_outputs(0);
    if (ret == ESP_OK) {
        currently_selected_relay_ = 0;
        last_relay_change_ = std::chrono::steady_clock::now();
        current_mask_[0] = 0;
        current_mask_[1] = 0;
    }
    return ret;
}

esp_err_t RelayController::set_relay(const int relay_id, const bool state) {
    int64_t set_relay_func_begin_time = esp_timer_get_time();
    if (relay_id < 1 || relay_id > NUM_RELAYS) {
        ESP_LOGE(TAG, "Invalid relay ID: %d", relay_id);
        ESP_LOGD(TAG, "PROF: set_relay (total, invalid arg) took %lld us", esp_timer_get_time() - set_relay_func_begin_time);
        return ESP_ERR_INVALID_ARG;
    }
    std::lock_guard lock(relay_mutex_);
    
    // More selective transmission check: only block changes to the transmitting radio's own relays
    if (is_transmitting()) {
        const bool is_radio_a_relay = (relay_id >= 1 && relay_id <= RELAYS_PER_RADIO);
        const bool is_radio_b_relay = (relay_id > RELAYS_PER_RADIO && relay_id <= NUM_RELAYS);
        
        const bool is_cat_tx = cat_parser_.is_transmitting(); // Assume this is Radio A
        const bool is_mqtt_tx = MQTTClient::instance().is_transmitting(); // Could be either radio
        
        if ((is_cat_tx && is_radio_a_relay) || (is_mqtt_tx && (is_radio_a_relay || is_radio_b_relay))) {
            ESP_LOGW(TAG, "Cannot change relay %d while that radio is transmitting", relay_id);
            return ESP_ERR_INVALID_STATE;
        }
    }
    
    // Convert from 1-based to 0-based index for hardware
    const uint8_t hw_relay = relay_id - 1;
    
    ESP_LOGD(TAG, "Setting relay %d (hw: %d) to state %d", relay_id, hw_relay, state);
    
    if (should_delay()) {
        int64_t delay_start_time = esp_timer_get_time();
        vTaskDelay(pdMS_TO_TICKS(COOLDOWN_PERIOD_MS));
        ESP_LOGD(TAG, "PROF: vTaskDelay(COOLDOWN_PERIOD_MS) took %lld us", esp_timer_get_time() - delay_start_time);
    }

    // Note: kc868_a16_set_output handles the active-low conversion internally
    // MODIFICATION: Use kc868_a16_set_all_outputs to mirror execute_relay_change's I2C pattern
    int64_t hw_set_output_start_time = esp_timer_get_time();
    uint16_t current_logical_states = kc868_a16_get_all_outputs(); // Get current logical ON/OFF state (0-indexed for mask)
    uint16_t new_logical_states;

    if (state) { // if turning ON
        new_logical_states = current_logical_states | (1 << hw_relay); // hw_relay is already 0-indexed
    } else { // if turning OFF
        new_logical_states = current_logical_states & ~(1 << hw_relay); // hw_relay is already 0-indexed
    }
    
    ESP_LOGD(TAG, "RelayController::set_relay: current_logical_states=0x%04X, new_logical_states=0x%04X for relay_id %d (hw %d) to state %s",
             current_logical_states, new_logical_states, relay_id, hw_relay, state ? "ON" : "OFF");

    const esp_err_t ret = kc868_a16_set_all_outputs(new_logical_states);
    ESP_LOGD(TAG, "PROF: kc868_a16_set_all_outputs() took %lld us", esp_timer_get_time() - hw_set_output_start_time);

    if (ret == ESP_OK) {
        last_relay_change_ = std::chrono::steady_clock::now();
        // Update our internal state tracking for the specific relay that was targeted
        relay_states_[relay_id] = state; 
        
        // Update currently_selected_relay_
        // This logic might need review if set_relay is used for more than just antenna selection,
        // but for now, it mirrors the original intent for currently_selected_relay_
        if (state) {
            currently_selected_relay_ = relay_id;
        } else if (currently_selected_relay_ == relay_id) {
            // If the relay being turned OFF was the currently_selected_relay_,
            // set currently_selected_relay_ to 0 (no relay selected).
            // If another relay was active, this doesn't change it, which is consistent
            // with set_relay affecting only one relay's logical state.
            currently_selected_relay_ = 0;
        }
        // Note: The underlying kc868_a16_set_all_outputs updates its own comprehensive 'output_state' cache.
        // The relay_states_ map here is RelayController's higher-level view.
        
        // Broadcast relay state change to WebSocket clients
        if (websocket_server_is_running()) {
            WebSocketServer::instance().broadcast_relay_state_change(relay_id, state);
        }
    }
    ESP_LOGD(TAG, "PROF: set_relay (total) took %lld us", esp_timer_get_time() - set_relay_func_begin_time);
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
    std::lock_guard lock(relay_mutex_);
    return relay_states_;
}

esp_err_t RelayController::update_all_relay_states() {
    ESP_LOGD(TAG, "Getting state of all relays");
    std::lock_guard lock(relay_mutex_);

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
    // kc868_a16_get_all_outputs() returns logical state (1 = ON, 0 = OFF).
    // However, UI layer expects active-low format (0 = ON, 1 = OFF) for consistency
    // with the active-low checking pattern: if (!((states >> i) & 1))
    // So we intentionally invert to provide active-low format to callers.
    const uint16_t logical_states = kc868_a16_get_all_outputs();
    const uint16_t active_low_states = ~logical_states & 0xFFFF;

    if (active_low_states != 0xFFFF) {
        ESP_LOGD(TAG, "Relay states (active-low): 0x%04X", active_low_states);
    }
    return active_low_states;
}

esp_err_t RelayController::turn_off_all_relays_except(const int relay_to_keep_on) {
    if (relay_to_keep_on < 1 || relay_to_keep_on > NUM_RELAYS) {
        ESP_LOGE(TAG, "Invalid relay ID: %d", relay_to_keep_on);
        return ESP_ERR_INVALID_ARG;
    }

    std::lock_guard lock(relay_mutex_);

    // More selective transmission check: block if trying to change transmitting radio's relays
    if (is_transmitting()) {
        const bool is_cat_tx = cat_parser_.is_transmitting(); // Assume this is Radio A
        const bool is_mqtt_tx = MQTTClient::instance().is_transmitting(); // Could be either radio
        
        // If either radio is transmitting, we need to be more careful about bulk operations
        if (is_cat_tx || is_mqtt_tx) {
            ESP_LOGW(TAG, "Cannot perform bulk relay changes while transmitting");
            return ESP_ERR_INVALID_STATE;
        }
    }

    if (should_delay()) {
        vTaskDelay(pdMS_TO_TICKS(COOLDOWN_PERIOD_MS));
    }

    const uint16_t relay_mask = 1 << (relay_to_keep_on - 1);
    const esp_err_t ret = kc868_a16_set_all_outputs(relay_mask);
    
    if (ret == ESP_OK) {
        currently_selected_relay_ = relay_to_keep_on;
        relay_states_[relay_to_keep_on] = true;
        last_relay_change_ = std::chrono::steady_clock::now();
        ESP_LOGD(TAG, "All relays turned off except relay %d", relay_to_keep_on);
    }
    
    return ret;
}

int RelayController::get_last_selected_relay_for_band(const int band_number) const {
    const auto &mapA = last_selected_relay_for_band_[0];
    if (const auto it = mapA.find(band_number); it != mapA.end()) {
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


 bool RelayController::is_transmitting() const {
     return cat_parser_.is_transmitting() || MQTTClient::instance().is_transmitting();
 }

esp_err_t RelayController::execute_relay_change(const int relay_id, const int band_number, const RadioID radio, const bool state) {
    std::lock_guard lock(relay_mutex_);
    const auto &cfg = get_cached_config();

    // More selective transmission check: only block changes to the transmitting radio's own relays
    if (is_transmitting()) {
        // Determine which radio this relay belongs to
        const bool is_radio_a_relay = (relay_id >= 1 && relay_id <= RELAYS_PER_RADIO);
        const bool is_radio_b_relay = (relay_id > RELAYS_PER_RADIO && relay_id <= NUM_RELAYS);
        
        // Check if we're trying to change a relay for the transmitting radio
        const bool is_cat_tx = cat_parser_.is_transmitting(); // Assume this is Radio A
        const bool is_mqtt_tx = MQTTClient::instance().is_transmitting(); // Could be either radio
        
        if ((is_cat_tx && is_radio_a_relay) || (is_mqtt_tx && (is_radio_a_relay || is_radio_b_relay))) {
            ESP_LOGW(TAG, "Cannot change relay %d while that radio is transmitting", relay_id);
            return ESP_ERR_INVALID_STATE;
        }
        
        // Allow interlock operations on non-transmitting radio relays
        ESP_LOGD(TAG, "Allowing relay %d change for interlock while other radio transmits", relay_id);
    }

    const int current_radio_idx = static_cast<int>(radio);
    const int other_radio_idx = (current_radio_idx == 0) ? 1 : 0;
    const int port_index = (relay_id - 1) % RELAYS_PER_RADIO;

    if (state) { // Turning a relay ON
        switch (cfg.radio_operation_mode) {
            case RADIO_OP_MODE_SINGLE_A:
                if (radio == RadioID::B) {
                    ESP_LOGW(TAG, "Radio B operations disabled (Single A mode). Cannot turn ON relay %d for Radio B.", relay_id);
                    current_mask_[1] = 0; // Ensure Radio B is off
                    // Do not return error, just ignore the request for B as it's a valid config state.
                    // The hw_mask update below will ensure B is off.
                } else { // Radio A
                    current_mask_[0] = (1u << port_index); // Set Radio A
                }
                current_mask_[1] = 0;                  // Ensure Radio B is always off in Single A mode
                break;

            case RADIO_OP_MODE_ALTERNATING_AB:
                // Clear the other radio's mask first
                current_mask_[other_radio_idx] = 0;
                // Then set the current radio's relay (intra-radio exclusivity)
                current_mask_[current_radio_idx] = (1u << port_index);
                break;

            case RADIO_OP_MODE_CONCURRENT_AB:
                // Check for same antenna port conflict with the other radio
                if ((current_mask_[other_radio_idx] & (1u << port_index)) != 0) { // If other radio is using the same port
                    if (cfg.interlock_auto_resolves_conflict) {
                        ESP_LOGW(TAG, "Interlock (Concurrent Mode, Auto-Resolve): Port %d conflict. Clearing other radio's relay.", port_index + 1);
                        current_mask_[other_radio_idx] &= ~(1u << port_index); // Clear the conflicting bit for the other radio
                    } else {
                        ESP_LOGE(TAG, "CRITICAL (Concurrent Mode, Block): Attempt to use same antenna port %d. Operation blocked.", port_index + 1);
                        return ESP_ERR_INVALID_STATE; // Block operation
                    }
                }
                // Set the current radio's relay (intra-radio exclusivity)
                current_mask_[current_radio_idx] = (1u << port_index);
                break;
        }
    } else { // Turning a relay OFF
        current_mask_[current_radio_idx] &= ~(1u << port_index);
        // If in SINGLE_A mode and Radio A is turned off, Radio B (current_mask_[1]) should remain 0.
        if (cfg.radio_operation_mode == RADIO_OP_MODE_SINGLE_A) {
            current_mask_[1] = 0; // Explicitly ensure Radio B mask is zero
        }
    }

    // Combine masks and push to hardware
    // current_mask_[0] is for relays 1-8 (physical bits 0-7)
    // current_mask_[1] is for relays 9-16 (physical bits 8-15)
    const uint16_t hw_mask = current_mask_[0] | (static_cast<uint16_t>(current_mask_[1]) << RELAYS_PER_RADIO);

    if (should_delay()) {
        vTaskDelay(pdMS_TO_TICKS(COOLDOWN_PERIOD_MS));
    }

    const esp_err_t ret = kc868_a16_set_all_outputs(hw_mask);
    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "Successfully set hw_mask: 0x%04X (Radio A: 0x%02X, Radio B: 0x%02X)", hw_mask, current_mask_[0], current_mask_[1]);
        last_relay_change_ = std::chrono::steady_clock::now();
        if (state) { // Only update last selected if turning ON
            last_selected_relay_for_band_[current_radio_idx][band_number] = relay_id;
        }
        // Update the generic currently_selected_relay_ if Radio A is involved, primarily for single-radio context or backward compatibility views
        if (radio == RadioID::A) {
            currently_selected_relay_ = state ? relay_id : 0; // if turning off, and it was this relay, set to 0
        }
        
        // Broadcast relay state change to WebSocket clients
        if (websocket_server_is_running()) {
            WebSocketServer::instance().broadcast_relay_state_change(relay_id, state);
        }
    } else {
        ESP_LOGE(TAG, "Failed to set all outputs with mask 0x%04X. Error: %s", hw_mask, esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t RelayController::set_relay_for_antenna(int relay_id, int band_number, const RadioID radio, const bool state) {
    if (relay_id < 1 || relay_id > NUM_RELAYS) {
        ESP_LOGE(TAG, "Invalid relay ID: %d", relay_id);
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGD(TAG, "Setting relay %d for Radio %d to state %s (band %d)", relay_id, static_cast<int>(radio), state ? "ON" : "OFF", band_number);

    return execute_relay_change(relay_id, band_number, radio, state);
}

