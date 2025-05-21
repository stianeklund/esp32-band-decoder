#include "antenna_switch.h"
#include "config_manager.h"
#include "esp_log.h"
#include "relay_controller.h"
#include "wifi_manager.hpp"
#include <memory>
#include <nvs.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "cat_parser.h" // Already present
#include "relay_controller.h" // For RelayController constants

static auto TAG = "ANTENNA_SWITCH";

[[maybe_unused]] static esp_err_t get_ip_address(char *ip_addr, const size_t max_len) {
    if (!ip_addr || max_len < 16) {
        // IPv4 address max length is 15 chars + null terminator
        return ESP_ERR_INVALID_ARG;
    }

    return WifiManager::instance().get_ip_info(ip_addr, max_len);
}

// Singleton implementation
AntennaSwitch& AntennaSwitch::instance() {
    static AntennaSwitch instance;
    return instance;
}

// Initialize the antenna switch
esp_err_t AntennaSwitch::init() {
    ESP_LOGI(TAG, "Initializing antenna switch");

    // Initialize configuration manager only
    if (const esp_err_t err = ConfigManager::instance().init(); err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize configuration manager: %s", esp_err_to_name(err));
        return err;
    }

    // Don't create or initialize the relay controller here
    // It will be initialized by SystemInitializer
    return ESP_OK;
}

void AntennaSwitch::set_relay_controller(RelayController* controller) {
    relay_controller_ = controller;
}

esp_err_t AntennaSwitch::set_config(const antenna_switch_config_t *config) {
    if (config == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    return ConfigManager::instance().update_config(*config);
}

esp_err_t AntennaSwitch::get_config(antenna_switch_config_t *config) {
    if (config == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *config = ConfigManager::instance().get_config();
    return ESP_OK;
}

const antenna_switch_config_t& AntennaSwitch::get_config_ref() const {
    return ConfigManager::instance().get_config();
}

// Callback from InputManager when hardware PTT A state changes
void AntennaSwitch::on_hw_ptt_a_state_change(const bool new_hw_ptt_a_value) {
    const bool old_hw_ptt_a_value = hw_ptt_a_active_.load(std::memory_order_relaxed);

    // Only proceed if the raw HW PTT A state has actually changed.
    if (old_hw_ptt_a_value == new_hw_ptt_a_value) {
        // ESP_LOGI(TAG, "Radio A: HW PTT raw state received (%s) is same as current. No action.", new_hw_ptt_a_value ? "ACTIVE" : "INACTIVE");
        return;
    }

    ESP_LOGV(TAG, "Radio A: HW PTT raw state changing from %s to %s",
             old_hw_ptt_a_value ? "ACTIVE" : "INACTIVE",
             new_hw_ptt_a_value ? "ACTIVE" : "INACTIVE");

    // Determine effective TX state BEFORE applying the new HW PTT state.
    // This uses the OLD hw_ptt_a_value and current CAT state.
    const bool cat_is_tx_a = CatParser::instance().is_transmitting(); // Assuming this is for Radio A
    const bool was_effectively_transmitting = old_hw_ptt_a_value || cat_is_tx_a;

    // Store the new HW PTT state
    hw_ptt_a_active_.store(new_hw_ptt_a_value, std::memory_order_relaxed);

    // Determine effective TX state AFTER applying the new HW PTT state.
    // This uses the NEW hw_ptt_a_value and current CAT state.
    const bool is_now_effectively_transmitting = new_hw_ptt_a_value || cat_is_tx_a;

    ESP_LOGV(TAG, "Radio A: Effective TX state check: was_eff_tx=%d, is_now_eff_tx=%d (new_hw_ptt=%d, cat_tx=%d)",
             was_effectively_transmitting, is_now_effectively_transmitting, new_hw_ptt_a_value, cat_is_tx_a);

    if (was_effectively_transmitting && !is_now_effectively_transmitting) {
        // Effective TX state changed from ON to OFF
        ESP_LOGV(TAG, "Radio A: Effective TX state changed from ON to OFF.");
        on_radio_a_tx_stop();
    } else if (!was_effectively_transmitting && is_now_effectively_transmitting) {
        // Effective TX state changed from OFF to ON
        ESP_LOGV(TAG, "Radio A: Effective TX state changed from OFF to ON.");
        on_radio_a_tx_start();
    } else {
        // Effective TX state did not change (e.g., was ON, still ON, or was OFF, still OFF)
        ESP_LOGV(TAG, "Radio A: HW PTT raw state changed, but effective TX state remains %s.",
                 is_now_effectively_transmitting ? "ON" : "OFF");
    }
}

// Callback from InputManager when hardware PTT B state changes
void AntennaSwitch::on_hw_ptt_b_state_change(const bool new_hw_ptt_b_value) {
    const bool old_hw_ptt_b_value = hw_ptt_b_active_.load(std::memory_order_relaxed);

    if (old_hw_ptt_b_value == new_hw_ptt_b_value) {
        // ESP_LOGI(TAG, "Radio B: HW PTT raw state received (%s) is same as current. No action.", new_hw_ptt_b_value ? "ACTIVE" : "INACTIVE");
        return;
    }

    ESP_LOGV(TAG, "Radio B: HW PTT raw state changing from %s to %s",
             old_hw_ptt_b_value ? "ACTIVE" : "INACTIVE",
             new_hw_ptt_b_value ? "ACTIVE" : "INACTIVE");

    // For Radio B, currently, its TX state is primarily driven by HW PTT.
    // If CatParser were to support independent TX state for Radio B, that check would be included here.
    const bool was_effectively_transmitting = old_hw_ptt_b_value; // Add || CatParser::instance().is_radio_b_transmitting() if applicable

    hw_ptt_b_active_.store(new_hw_ptt_b_value, std::memory_order_relaxed);

    const bool is_now_effectively_transmitting = new_hw_ptt_b_value; // Add || CatParser::instance().is_radio_b_transmitting() if applicable

    ESP_LOGV(TAG, "Radio B: Effective TX state check: was_eff_tx=%d, is_now_eff_tx=%d (new_hw_ptt=%d)",
             was_effectively_transmitting, is_now_effectively_transmitting, new_hw_ptt_b_value);

    if (was_effectively_transmitting && !is_now_effectively_transmitting) {
        ESP_LOGV(TAG, "Radio B: Effective TX state changed from ON to OFF.");
        on_radio_b_tx_stop();
    } else if (!was_effectively_transmitting && is_now_effectively_transmitting) {
        ESP_LOGV(TAG, "Radio B: Effective TX state changed from OFF to ON.");
        on_radio_b_tx_start();
    } else {
        ESP_LOGV(TAG, "Radio B: HW PTT raw state changed, but effective TX state remains %s.",
                 is_now_effectively_transmitting ? "ON" : "OFF");
    }
}

bool AntennaSwitch::is_radio_a_transmitting_effective() const {
    return hw_ptt_a_active_.load(std::memory_order_relaxed) || CatParser::instance().is_transmitting();
}

bool AntennaSwitch::is_radio_b_transmitting_effective() const {
    // For now, Radio B's TX state is determined by its hardware PTT.
    // If CatParser supported Radio B TX state:
    // return hw_ptt_b_active_.load(std::memory_order_relaxed) || CatParser::instance().is_radio_b_transmitting();
     return hw_ptt_b_active_.load(std::memory_order_relaxed);
}

esp_err_t AntennaSwitch::set_frequency(const uint32_t frequency) {
    ESP_LOGV(TAG, "Setting antenna for frequency: %lu Hz", frequency);

    // we only need a reference to config here as we won't mutate it
    const auto &config = ConfigManager::instance().get_config();
    if (!config.auto_mode) {
        ESP_LOGW(TAG, "Automatic mode is disabled, not changing antenna");
        return ESP_OK;
    }

    // Assuming frequency updates are primarily for Radio A's context in auto mode
    constexpr auto radio_idx_numeric = static_cast<size_t>(RadioID::A);
    constexpr auto current_radio_context = RadioID::A;

    for (int i = 0; i < config.num_bands; i++) { // i is band_index
        // ReSharper disable once CppTooWideScopeInitStatement
        const auto &band_config_for_radio = config.bands[radio_idx_numeric][i];
        if (frequency >= band_config_for_radio.start_freq &&
            frequency <= band_config_for_radio.end_freq) {
            
            int target_relay_id = 0;
            const uint8_t preferred_antenna = config.last_used_antenna[radio_idx_numeric][i];

            // Check if preferred antenna is valid (1-based) and configured for this band
            if (preferred_antenna != 0 && 
                preferred_antenna <= config.num_antenna_ports &&
                band_config_for_radio.antenna_ports[preferred_antenna - 1]) {
                target_relay_id = preferred_antenna;
                ESP_LOGD(TAG, "Using preferred antenna %d for Radio %c, Band %d (Freq: %lu Hz)",
                         target_relay_id, (current_radio_context == RadioID::A ? 'A' : 'B'), i, frequency);
            } else {
                // Fallback: Find the first available antenna port for this band
                for (int j = 0; j < config.num_antenna_ports; j++) { // j is port_index (0-based)
                    if (band_config_for_radio.antenna_ports[j]) {
                        target_relay_id = j + 1; // relay_id is 1-based
                        ESP_LOGD(TAG, "Using first available antenna %d for Radio %c, Band %d (Freq: %lu Hz)",
                                 target_relay_id, (current_radio_context == RadioID::A ? 'A' : 'B'), i, frequency);
                        break;
                    }
                }
            }

            if (target_relay_id != 0) {
                ESP_LOGD(TAG, "Auto-selecting relay %d for band %d (Radio %c) due to frequency %lu Hz",
                         target_relay_id, i, (current_radio_context == RadioID::A ? 'A' : 'B'), frequency);
                if (!relay_controller_) return ESP_ERR_INVALID_STATE;
                return relay_controller_->set_relay_for_antenna(target_relay_id, i, current_radio_context, true);
            }

            ESP_LOGW(TAG, "No available/preferred antenna port found for Radio %c, Band %d (Freq: %lu Hz, Preferred: %d)",
                     (current_radio_context == RadioID::A ? 'A' : 'B'), i, frequency, preferred_antenna);
            return ESP_OK; // No suitable antenna, but not an error in processing
        }
    }

    ESP_LOGV(TAG, "Config does not support frequency: %lu Hz for auto-switching on Radio %c", frequency, (current_radio_context == RadioID::A ? 'A' : 'B'));
    return ESP_OK;
}

/**
 * Whether automatic band switching should be used
 * @param auto_mode
 * @return esp_err_t
 */
esp_err_t AntennaSwitch::set_auto_mode(const bool auto_mode) {
    ESP_LOGD(TAG, "Setting auto mode: %s", auto_mode ? "ON" : "OFF");

    const auto config_ptr = std::make_unique<antenna_switch_config_t>();
    if (!config_ptr) {
        ESP_LOGE(TAG, "Failed to allocate memory for config in set_auto_mode");
        return ESP_ERR_NO_MEM;
    }
    // Populate the heap-allocated config with current settings
    *config_ptr = ConfigManager::instance().get_config(); // Copy from ConfigManager's version

    config_ptr->auto_mode = auto_mode;

    return ConfigManager::instance().update_config(*config_ptr);
}

int AntennaSwitch::get_active_relay_for_radio(RadioID radio) const {
    if (!relay_controller_) {
        ESP_LOGE(TAG, "Relay controller not initialized in get_active_relay_for_radio");
        return 0; // 0 indicates no relay active or error
    }

    int start_relay_id, end_relay_id;
    if (radio == RadioID::A) {
        start_relay_id = 1;
        end_relay_id = RelayController::RELAYS_PER_RADIO;
    } else { // RadioID::B
        start_relay_id = RelayController::RELAYS_PER_RADIO + 1;
        end_relay_id = RelayController::NUM_RELAYS;
    }

    for (int relay_id_iter = start_relay_id; relay_id_iter <= end_relay_id; ++relay_id_iter) {
        bool current_state = false;
        // Use the public get_relay_state which queries the controller
        // Note: get_relay_state in AntennaSwitch calls relay_controller_->get_relay_state
        current_state = relay_controller_->get_relay_state(relay_id_iter);
        if (current_state) { // If true, relay is ON and active.
            return relay_id_iter;
        }
    }
    return 0; // No active relay found for this radio
}

void AntennaSwitch::on_radio_a_tx_start() {
    ESP_LOGI(TAG, "Radio A TX: HW PTT A: %s, CAT TX A: %s. Applying interlock for Radio B.",
             hw_ptt_a_active_.load(std::memory_order_relaxed) ? "ACTIVE" : "INACTIVE",
             CatParser::instance().is_transmitting() ? "ON" : "OFF");
    const auto& config = ConfigManager::instance().get_config();

    if (config.radio_operation_mode == RADIO_OP_MODE_SINGLE_A) {
        ESP_LOGD(TAG, "Single Radio A mode, no interlock action needed for Radio B (due to Radio A TX).");
        return;
    }

    if (!relay_controller_) {
        ESP_LOGE(TAG, "Relay controller not initialized, cannot apply TX interlock for Radio B.");
        return;
    }

    // Check if Radio B is ALREADY considered interlocked by THIS mechanism
    if (pre_tx_active_relay_radio_b_ != 0) {
        ESP_LOGD(TAG, "Radio A TX start: Radio B interlock already in effect (relay %d previously stored). No new action.", pre_tx_active_relay_radio_b_);
        return;
    }

    // If not already interlocked by us, find out what Radio B is currently doing

    if (const int active_b_relay = get_active_relay_for_radio(RadioID::B); active_b_relay != 0) { // Radio B is using a relay
        // Before turning off B's relay, check for conflicts
        if (is_radio_b_transmitting_effective() && config.radio_operation_mode == RADIO_OP_MODE_CONCURRENT_AB && !config.interlock_auto_resolves_conflict) {
            ESP_LOGW(TAG, "Radio A TX start: Conflict! Radio B (relay %d) is also transmitting. Interlock resolution is manual. Radio B relay NOT changed.", active_b_relay);
            // Do NOT set pre_tx_active_relay_radio_b_ here, because we didn't turn off its relay.
        } else {
            // Ok, we need to turn off Radio B's relay. Store it first.
            pre_tx_active_relay_radio_b_ = active_b_relay;
            ESP_LOGI(TAG, "Radio B was using relay %d. Turning it OFF due to Radio A TX.", pre_tx_active_relay_radio_b_);
            esp_err_t err = relay_controller_->set_relay(pre_tx_active_relay_radio_b_, false);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to turn OFF Radio B relay %d: %s", pre_tx_active_relay_radio_b_, esp_err_to_name(err));
                pre_tx_active_relay_radio_b_ = 0; // Failed, so don't try to restore
            }
        }
    } else {
        ESP_LOGD(TAG, "Radio B was not using any relay. No interlock action needed for Radio A TX.");
        // pre_tx_active_relay_radio_b_ remains 0, so no restoration will be attempted.
    }
}

void AntennaSwitch::on_radio_a_tx_stop() {
    ESP_LOGI(TAG, "Radio A TX: HW PTT A: %s, CAT TX A: %s. Restoring Radio B state if applicable.",
             hw_ptt_a_active_.load(std::memory_order_relaxed) ? "ACTIVE" : "INACTIVE",
             CatParser::instance().is_transmitting() ? "ON" : "OFF");
    const auto& config = ConfigManager::instance().get_config();

    if (config.radio_operation_mode == RADIO_OP_MODE_SINGLE_A) {
        ESP_LOGD(TAG, "Single Radio A mode, no interlock restoration needed for Radio B.");
        return;
    }

    if (!relay_controller_) {
        ESP_LOGE(TAG, "Relay controller not initialized, cannot restore Radio B state.");
        return;
    }

    if (pre_tx_active_relay_radio_b_ != 0) {
        // Before restoring, ensure Radio B is not now transmitting itself
        if (is_radio_b_transmitting_effective()) {
            ESP_LOGW(TAG, "Radio B is currently transmitting (effective), not restoring its relay %d.", pre_tx_active_relay_radio_b_);
            // Decide if we clear pre_tx_active_relay_radio_b_ or retry later. For now, clear.
            pre_tx_active_relay_radio_b_ = 0; 
            return;
        }

        ESP_LOGI(TAG, "Restoring Radio B relay %d to ON.", pre_tx_active_relay_radio_b_);
        esp_err_t err = relay_controller_->set_relay(pre_tx_active_relay_radio_b_, true);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to restore Radio B relay %d: %s", pre_tx_active_relay_radio_b_, esp_err_to_name(err));
        }
        pre_tx_active_relay_radio_b_ = 0; // Clear after attempting restoration
    } else {
        ESP_LOGD(TAG, "No Radio B relay was stored (pre_tx_active_relay_radio_b_ is 0). No restoration action for Radio B.");
    }
}

void AntennaSwitch::on_radio_b_tx_start() {
    ESP_LOGI(TAG, "Radio B TX: HW PTT B: %s. Applying interlock for Radio A.",
             hw_ptt_b_active_.load(std::memory_order_relaxed) ? "ACTIVE" : "INACTIVE");
    const auto& config = ConfigManager::instance().get_config();

    if (config.radio_operation_mode == RADIO_OP_MODE_SINGLE_A) {
        ESP_LOGW(TAG, "Radio B TX reported in SINGLE_A mode. This is unexpected.");
        return;
    }
    if (!relay_controller_) {
        ESP_LOGE(TAG, "Relay controller not initialized, cannot apply TX interlock for Radio A.");
        return;
    }

    // Check if Radio A is ALREADY considered interlocked by THIS mechanism
    if (pre_tx_active_relay_radio_a_ != 0) {
        ESP_LOGD(TAG, "Radio B TX start: Radio A interlock already in effect (relay %d previously stored). No new action.", pre_tx_active_relay_radio_a_);
        return;
    }

    // If not already interlocked by us, find out what Radio A is currently doing
    int active_a_relay = get_active_relay_for_radio(RadioID::A);

    if (active_a_relay != 0) { // Radio A is using a relay
        // Before turning off A's relay, check for conflicts
        if (is_radio_a_transmitting_effective() && config.radio_operation_mode == RADIO_OP_MODE_CONCURRENT_AB && !config.interlock_auto_resolves_conflict) {
            ESP_LOGW(TAG, "Radio B TX start: Conflict! Radio A (relay %d) is also transmitting. Interlock resolution is manual. Radio A relay NOT changed.", active_a_relay);
            // Do NOT set pre_tx_active_relay_radio_a_ here, because we didn't turn off its relay.
        } else {
            // Ok, we need to turn off Radio A's relay. Store it first.
            pre_tx_active_relay_radio_a_ = active_a_relay;
            ESP_LOGI(TAG, "Radio A was using relay %d. Turning it OFF due to Radio B TX.", pre_tx_active_relay_radio_a_);
            esp_err_t err = relay_controller_->set_relay(pre_tx_active_relay_radio_a_, false);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to turn OFF Radio A relay %d: %s", pre_tx_active_relay_radio_a_, esp_err_to_name(err));
                pre_tx_active_relay_radio_a_ = 0; // Failed, so don't try to restore
            }
        }
    } else {
        ESP_LOGD(TAG, "Radio A was not using any relay. No interlock action needed for Radio B TX.");
        // pre_tx_active_relay_radio_a_ remains 0, so no restoration will be attempted.
    }
}

void AntennaSwitch::on_radio_b_tx_stop() {
    ESP_LOGI(TAG, "Radio B TX: HW PTT B: %s. Restoring Radio A state if applicable.",
             hw_ptt_b_active_.load(std::memory_order_relaxed) ? "ACTIVE" : "INACTIVE");
    const auto& config = ConfigManager::instance().get_config();

    if (config.radio_operation_mode == RADIO_OP_MODE_SINGLE_A) {
        return;
    }
    if (!relay_controller_) {
        ESP_LOGE(TAG, "Relay controller not initialized, cannot restore Radio A state.");
        return;
    }

    if (pre_tx_active_relay_radio_a_ != 0) {
        // Before restoring, ensure Radio A is not now transmitting itself
        if (is_radio_a_transmitting_effective()) {
             ESP_LOGW(TAG, "Radio A is currently transmitting (effective), cannot restore its previous relay %d after Radio B TX stop.", pre_tx_active_relay_radio_a_);
             // Decide if we clear pre_tx_active_relay_radio_a_ or retry later. For now, clear.
             pre_tx_active_relay_radio_a_ = 0;
             return;
        } else {
            ESP_LOGI(TAG, "Restoring Radio A relay %d to ON.", pre_tx_active_relay_radio_a_);
            esp_err_t err = relay_controller_->set_relay(pre_tx_active_relay_radio_a_, true);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to restore Radio A relay %d: %s", pre_tx_active_relay_radio_a_, esp_err_to_name(err));
            }
        }
        pre_tx_active_relay_radio_a_ = 0;
    }
    ESP_LOGW(TAG, "Note: Radio B TX detection and interlock is partially stubbed until Radio B TX state is available.");
}

esp_err_t AntennaSwitch::update_last_used_antenna_preference(int activated_relay_id, RadioID radio_of_activated_relay) {
    auto current_cfg_ptr = std::make_unique<antenna_switch_config_t>();
    if (!current_cfg_ptr) {
        ESP_LOGE(TAG, "Failed to allocate memory for config in update_last_used_antenna_preference; skipping preference update.");
        return ESP_ERR_NO_MEM;
    }

    if (get_config(current_cfg_ptr.get()) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get_config in update_last_used_antenna_preference; skipping preference update.");
        return ESP_FAIL;
    }

    uint32_t current_freq_for_preference = 0;
    if (radio_of_activated_relay == RadioID::A) {
        current_freq_for_preference = CatParser::instance().get_frequency();
    } else {
        // Placeholder: Get Radio B frequency if available
        // current_freq_for_preference = get_radio_b_frequency(); // This function doesn't exist yet
        ESP_LOGD(TAG, "Radio B frequency not available for preference update for relay %d.", activated_relay_id);
    }

    int band_idx = -1;
    const auto radio_numeric_idx = static_cast<size_t>(radio_of_activated_relay);

    // Determine band_idx based on frequency
    // Allow preference update for B even without freq if band is known (though this function is called after relay activation, band context might be from freq)
    if (current_freq_for_preference > 0) { // Only attempt to find band if frequency is known
        for (int i = 0; i < current_cfg_ptr->num_bands; i++) {
            const auto& band_config_for_radio_pref = current_cfg_ptr->bands[radio_numeric_idx][i];
            if (current_freq_for_preference >= band_config_for_radio_pref.start_freq &&
                current_freq_for_preference <= band_config_for_radio_pref.end_freq) {
                band_idx = i;
                break;
            }
        }
    } else if (radio_of_activated_relay == RadioID::B) {
        // If Radio B and no frequency, we cannot determine the band to update preference for.
        ESP_LOGD(TAG, "Cannot determine band for Radio B preference update without frequency for relay %d.", activated_relay_id);
    }

    // The check for band_idx == -1 should correctly determine if a band was found.
    // If band_idx is still -1 here, it means no band matched the frequency.
    if (band_idx == -1) { 
        ESP_LOGD(TAG, "No current band found for Radio %c (Freq: %lu Hz). Not updating preference for relay %d.",
                 (radio_of_activated_relay == RadioID::A ? 'A' : 'B'), current_freq_for_preference, activated_relay_id);
        return ESP_OK; // Not an error, just no band to update preference for
    }

    // Check if the selected relay is actually configured for this band for this radio
    // relay_id is 1-based, antenna_ports is 0-based.
    // The port_index for the band_config_t.antenna_ports array is 0-based and relative to the radio.
    int port_index_in_band_config = -1;
    if (radio_of_activated_relay == RadioID::A && activated_relay_id >= 1 && activated_relay_id <= RelayController::RELAYS_PER_RADIO) {
        port_index_in_band_config = activated_relay_id - 1; // e.g. relay 1 is port 0 for Radio A
    } else if (radio_of_activated_relay == RadioID::B && activated_relay_id > RelayController::RELAYS_PER_RADIO && activated_relay_id <= RelayController::NUM_RELAYS) {
        // e.g. relay 9 is port 0 for Radio B ( (9-1) % 8 = 0 if RELAYS_PER_RADIO is 8)
        port_index_in_band_config = (activated_relay_id - 1) % RelayController::RELAYS_PER_RADIO;
    }

    if (port_index_in_band_config == -1 || port_index_in_band_config >= current_cfg_ptr->num_antenna_ports ||
        !current_cfg_ptr->bands[radio_numeric_idx][band_idx].antenna_ports[port_index_in_band_config]) {
        ESP_LOGW(TAG, "Relay %d is not configured for Radio %c, Band %d (Freq: %lu Hz). Not updating preference.",
                 activated_relay_id, (radio_of_activated_relay == RadioID::A ? 'A' : 'B'), band_idx, current_freq_for_preference);
        return ESP_OK; // Not an error, just not a valid relay for this band's preference
    }

    if (current_cfg_ptr->last_used_antenna[radio_numeric_idx][band_idx] != static_cast<uint8_t>(activated_relay_id)) {
        ESP_LOGD(TAG, "Updating last used antenna for Radio %c, Band %d to Relay %d (Freq: %lu Hz)",
                 (radio_of_activated_relay == RadioID::A ? 'A' : 'B'), band_idx, activated_relay_id, current_freq_for_preference);
        current_cfg_ptr->last_used_antenna[radio_numeric_idx][band_idx] = static_cast<uint8_t>(activated_relay_id);

        const esp_err_t save_err = ConfigManager::instance().update_config(*current_cfg_ptr);
        if (save_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save updated last_used_antenna: %s", esp_err_to_name(save_err));
            return save_err; // Propagate the save error
        }
    }
    return ESP_OK;
}

// Helper to attempt restoration of relays deselected by auto-resolved port conflicts
void AntennaSwitch::attempt_restore_auto_resolved_radio_a_relay() {
    const auto& config = ConfigManager::instance().get_config();
    // Check if conditions for auto-restoration are met
    if (config.radio_operation_mode != RADIO_OP_MODE_CONCURRENT_AB ||
        !config.interlock_auto_resolves_conflict ||
        !config.auto_restore_on_conflict_resolution) {
        // If conditions are not met, and a relay was stored, log and clear it.
        if (auto_resolved_conflict_prev_a_relay_ != 0) {
            ESP_LOGD(TAG, "Auto-restore conditions not met (mode/interlock/auto_restore_setting). Clearing stored Radio A relay %d.", auto_resolved_conflict_prev_a_relay_);
            auto_resolved_conflict_prev_a_relay_ = 0;
        }
        return;
    }

    if (auto_resolved_conflict_prev_a_relay_ == 0) {
        // ESP_LOGD(TAG, "No Radio A relay stored from auto-resolved conflict to restore.");
        return;
    }

    // Check if Radio A is currently transmitting (should not restore if it is)
    if (is_radio_a_transmitting_effective()) {
        ESP_LOGW(TAG, "Radio A is currently transmitting. Cannot restore auto-resolved relay %d.", auto_resolved_conflict_prev_a_relay_);
        // Do not clear auto_resolved_conflict_prev_a_relay_ here, as TX state might change.
        return;
    }

    // Check if the port is now free (i.e., Radio B is not using it or is not transmitting on it)
    int active_b_relay = get_active_relay_for_radio(RadioID::B);
    int port_idx_of_stored_a_relay = (auto_resolved_conflict_prev_a_relay_ - 1) % RelayController::RELAYS_PER_RADIO;
    
    if (active_b_relay != 0) {
        int port_idx_of_active_b_relay = (active_b_relay - 1) % RelayController::RELAYS_PER_RADIO;
        if (port_idx_of_active_b_relay == port_idx_of_stored_a_relay) {
            // Port is still in use by Radio B. Check if Radio B is transmitting.
            if (is_radio_b_transmitting_effective()) {
                 ESP_LOGW(TAG, "Cannot restore Radio A relay %d. Port %d is in use by Radio B (relay %d) AND Radio B is transmitting.",
                         auto_resolved_conflict_prev_a_relay_, port_idx_of_stored_a_relay + 1, active_b_relay);
                return;
            }
            ESP_LOGD(TAG, "Port %d for Radio A relay %d is currently used by Radio B (relay %d), but B is not TXing. Attempting restore for A.",
                     port_idx_of_stored_a_relay + 1, auto_resolved_conflict_prev_a_relay_, active_b_relay);
        }
    }

    ESP_LOGI(TAG, "Attempting to restore Radio A relay %d (deselected by auto-resolved conflict).", auto_resolved_conflict_prev_a_relay_);
    // Use set_relay_for_antenna to ensure proper mode handling. Band_number can be -1 if not relevant for direct restore.
    esp_err_t err = set_relay_for_antenna(auto_resolved_conflict_prev_a_relay_, -1, RadioID::A, true);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Successfully restored Radio A relay %d.", auto_resolved_conflict_prev_a_relay_);
        auto_resolved_conflict_prev_a_relay_ = 0; // Clear after successful restoration
    } else {
        ESP_LOGE(TAG, "Failed to restore Radio A relay %d: %s. It might be restored later if conditions change.", auto_resolved_conflict_prev_a_relay_, esp_err_to_name(err));
    }
}

void AntennaSwitch::attempt_restore_auto_resolved_radio_b_relay() {
    const auto& config = ConfigManager::instance().get_config();
    if (config.radio_operation_mode != RADIO_OP_MODE_CONCURRENT_AB ||
        !config.interlock_auto_resolves_conflict ||
        !config.auto_restore_on_conflict_resolution) {
        if (auto_resolved_conflict_prev_b_relay_ != 0) {
            ESP_LOGD(TAG, "Auto-restore conditions not met (mode/interlock/auto_restore_setting). Clearing stored Radio B relay %d.", auto_resolved_conflict_prev_b_relay_);
            auto_resolved_conflict_prev_b_relay_ = 0;
        }
        return;
    }

    if (auto_resolved_conflict_prev_b_relay_ == 0) {
        // ESP_LOGD(TAG, "No Radio B relay stored from auto-resolved conflict to restore.");
        return;
    }

    // Don't restore if Radio B is transmitting
    if (is_radio_b_transmitting_effective()) {
        ESP_LOGW(TAG, "Radio B is currently transmitting. Cannot restore auto-resolved relay %d.", auto_resolved_conflict_prev_b_relay_);
        return;
    }

    // Get the port index of the stored Radio B relay we want to restore
    int port_idx_of_stored_b_relay = (auto_resolved_conflict_prev_b_relay_ - 1) % RelayController::RELAYS_PER_RADIO;

    // Check if Radio A is using the same port
    int active_a_relay = get_active_relay_for_radio(RadioID::A);
    if (active_a_relay != 0) {
        int port_idx_of_active_a_relay = (active_a_relay - 1) % RelayController::RELAYS_PER_RADIO;
        
        // If Radio A is using the same port, we cannot restore Radio B's relay
        if (port_idx_of_active_a_relay == port_idx_of_stored_b_relay) {
            ESP_LOGW(TAG, "Cannot restore Radio B relay %d. Port %d is still in use by Radio A (relay %d).",
                     auto_resolved_conflict_prev_b_relay_, port_idx_of_stored_b_relay + 1, active_a_relay);
                     
            // If Radio A is transmitting, just return (we'll try again later)
            if (is_radio_a_transmitting_effective()) {
                ESP_LOGW(TAG, "Additionally, Radio A is currently transmitting on this port.");
                return;
            }
            
            // Even if Radio A is not transmitting, we still can't restore B to the same port
            ESP_LOGD(TAG, "Radio A is not currently transmitting, but still using the port. Cannot restore Radio B relay.");
            return;
        }
    }

    // Radio A is not using the same port, so we can safely attempt to restore Radio B's relay
    ESP_LOGI(TAG, "Attempting to restore Radio B relay %d (deselected by auto-resolved conflict).", auto_resolved_conflict_prev_b_relay_);
    esp_err_t err = set_relay_for_antenna(auto_resolved_conflict_prev_b_relay_, -1, RadioID::B, true);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Successfully restored Radio B relay %d.", auto_resolved_conflict_prev_b_relay_);
        auto_resolved_conflict_prev_b_relay_ = 0; 
    } else {
        ESP_LOGE(TAG, "Failed to restore Radio B relay %d: %s. It might be restored later if conditions change.", auto_resolved_conflict_prev_b_relay_, esp_err_to_name(err));
    }
}


esp_err_t AntennaSwitch::set_relay(const int relay_id, const bool state) {
    ESP_LOGI(TAG, "AntennaSwitch: Setting relay %d to %s", relay_id, state ? "ON" : "OFF");

    if (relay_id < 1 || relay_id > RelayController::NUM_RELAYS) {
        ESP_LOGE(TAG, "Invalid relay ID: %d", relay_id);
        return ESP_ERR_INVALID_ARG;
    }

    if (!relay_controller_) {
        ESP_LOGE(TAG, "Relay controller not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    const auto& config = ConfigManager::instance().get_config(); // Used for interlock checks
    const bool is_this_radio_a_relay = (relay_id >= 1 && relay_id <= RelayController::RELAYS_PER_RADIO);
    const bool is_this_radio_b_relay = (relay_id > RelayController::RELAYS_PER_RADIO && relay_id <= RelayController::NUM_RELAYS);

    if (state) { // If trying to turn a relay ON
        if (is_this_radio_a_relay) {
            // Hot-switching protection for Radio A itself:
            if (is_radio_a_transmitting_effective()) {
                ESP_LOGW(TAG, "Cannot turn ON Radio A relay %d: Radio A is currently transmitting (effective).", relay_id);
                return ESP_ERR_INVALID_STATE;
            }
            // Interlock: Check if Radio B is transmitting
            if (config.radio_operation_mode != RADIO_OP_MODE_SINGLE_A && is_radio_b_transmitting_effective()) {
                 ESP_LOGW(TAG, "Cannot turn ON Radio A relay %d: Radio B is transmitting (effective).", relay_id);
                 return ESP_ERR_INVALID_STATE;
            }
        } else if (is_this_radio_b_relay) {
            if (config.radio_operation_mode == RADIO_OP_MODE_SINGLE_A) {
                ESP_LOGW(TAG, "Radio B operations disabled (Single A mode), cannot turn ON relay %d for Radio B.", relay_id);
                return ESP_OK; 
            }
            // Hot-switching protection for Radio B itself:
            if (is_radio_b_transmitting_effective()) {
                ESP_LOGW(TAG, "Cannot turn ON Radio B relay %d: Radio B is currently transmitting (effective).", relay_id);
                return ESP_ERR_INVALID_STATE;
            }
            // Interlock: Check if Radio A is transmitting
            if (is_radio_a_transmitting_effective()) {
                ESP_LOGW(TAG, "Cannot turn ON Radio B relay %d: Radio A is transmitting (effective).", relay_id);
                return ESP_ERR_INVALID_STATE;
            }
        }
    }

    // Determine which radio this relay belongs to for the call to set_relay_for_antenna
    const RadioID radio_context = is_this_radio_a_relay ? RadioID::A : RadioID::B;

    // The band_number is -1 here as this is a direct relay set, not tied to a specific band's auto-selection.
    // RelayController::set_relay_for_antenna contains the core logic for setting relays based on mode.
    const esp_err_t err = relay_controller_->set_relay_for_antenna(relay_id, /*band_number=*/-1, radio_context, state);

    // If turning a relay ON successfully, attempt to update last_used_antenna preference
    if (state && err == ESP_OK) {
        const esp_err_t pref_err = update_last_used_antenna_preference(relay_id, radio_context);

        if (pref_err != ESP_OK && pref_err != ESP_ERR_NO_MEM && pref_err != ESP_FAIL) {
            ESP_LOGE(TAG, "Error updating antenna preference for relay %d, radio %c: %s. Relay operation itself was successful.",
                     relay_id, (radio_context == RadioID::A ? 'A' : 'B'), esp_err_to_name(pref_err));
        }
    }
    return err; // Return status of the primary relay operation
}

esp_err_t AntennaSwitch::set_relay_radio_b(int relay_id, bool state) {
    const auto &cfg = ConfigManager::instance().get_config();
    if (cfg.radio_operation_mode == RADIO_OP_MODE_SINGLE_A && state) { // Only block if trying to turn ON when Radio B is disabled
        ESP_LOGW(TAG, "Radio B operations disabled (Single A mode), cannot turn ON relay %d for Radio B", relay_id);
        return ESP_OK; 
    }
    if (!relay_controller_) {
        ESP_LOGE(TAG, "Relay controller not initialized for Radio B set relay");
        return ESP_ERR_INVALID_STATE;
    }
    // Ensure relay_id is within Radio B's range (e.g., 9-16 if RELAYS_PER_RADIO is 8)
    if (relay_id <= RelayController::RELAYS_PER_RADIO || relay_id > RelayController::NUM_RELAYS) {
        ESP_LOGE(TAG, "Invalid relay_id %d for Radio B operation in set_relay_radio_b.", relay_id);
        return ESP_ERR_INVALID_ARG;
    }

    if (state) { // If trying to turn ON Radio B relay
        // Hot-switching protection for Radio B itself
        if (is_radio_b_transmitting_effective()) {
            ESP_LOGW(TAG, "Cannot turn ON Radio B relay %d: Radio B is currently transmitting (effective).", relay_id);
            return ESP_ERR_INVALID_STATE;
        }
        // Interlock: Check if Radio A is transmitting
        if (is_radio_a_transmitting_effective()) {
            ESP_LOGW(TAG, "Radio A is transmitting (effective), cannot turn ON relay %d for Radio B.", relay_id);
            return ESP_ERR_INVALID_STATE;
        }
    }
    // The band_number is -1 as this is a direct relay set.
    return relay_controller_->set_relay_for_antenna(relay_id, /*band_number=*/-1, RadioID::B, state);
}

esp_err_t AntennaSwitch::get_relay_state(const int relay_id, bool *state) {
    if (state == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!relay_controller_) {
        ESP_LOGE(TAG, "Relay controller not initialized for get_relay_state");
        return ESP_ERR_INVALID_STATE;
    }
    *state = relay_controller_->get_relay_state(relay_id);
    return ESP_OK;
}

esp_err_t AntennaSwitch::restart() {
    ESP_LOGI(TAG, "Restarting device...");

    // Attempt to flush any pending configuration changes to NVS
    ESP_LOGI(TAG, "Flushing pending configuration to NVS before restart...");
    esp_err_t flush_err = ConfigManager::instance().flush_pending_save(pdMS_TO_TICKS(1000)); // Wait up to 1 second
    if (flush_err == ESP_OK) {
        ESP_LOGI(TAG, "Pending configuration successfully flushed to NVS.");
    } else if (flush_err == ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "Timeout flushing configuration to NVS before restart. Changes might not be saved.");
    } else {
        ESP_LOGE(TAG, "Error flushing configuration to NVS: %s. Changes might not be saved.", esp_err_to_name(flush_err));
    }

    // Small delay to allow logging to complete and any other brief OS tasks
    vTaskDelay(pdMS_TO_TICKS(200));

    esp_restart();
    return ESP_OK; // esp_restart() does not return
}

esp_err_t AntennaSwitch::set_relay_for_antenna(int relay_id, int band_number, RadioID radio, bool state) {
    if (!relay_controller_) {
        ESP_LOGE(TAG, "Relay controller not initialized for set_relay_for_antenna");
        return ESP_ERR_INVALID_STATE;
    }
    const auto& config = ConfigManager::instance().get_config(); // Get config early

    // For conflict restoration logic tracking, get the port index we're trying to activate
    int activating_radio_port_idx = -1;
    if (state) {
        activating_radio_port_idx = (relay_id - 1) % RelayController::RELAYS_PER_RADIO;
    }

    // Anticipate and store other radio's state if a port conflict will be auto-resolved
    if (state && config.radio_operation_mode == RADIO_OP_MODE_CONCURRENT_AB && config.interlock_auto_resolves_conflict) {
        RadioID other_radio_id = (radio == RadioID::A) ? RadioID::B : RadioID::A;
        int active_other_relay = get_active_relay_for_radio(other_radio_id);

        if (active_other_relay != 0) {
            // Port indices are 0-based from the perspective of a single radio's available ports
            int other_radio_active_port_idx = (active_other_relay - 1) % RelayController::RELAYS_PER_RADIO;

            if (activating_radio_port_idx == other_radio_active_port_idx) {
                // Conflict will be auto-resolved by RelayController, deactivating other_radio's relay.
                // Store this relay to allow restoration later when the conflict is resolved
                if (other_radio_id == RadioID::B) {
                    if (auto_resolved_conflict_prev_b_relay_ == 0) { // Only if not already set
                        ESP_LOGI(TAG, "Anticipating Radio B relay %d (port %d) deactivation by Radio A (activating relay %d, port %d). Storing for auto-restoration.",
                                 active_other_relay, other_radio_active_port_idx + 1, relay_id, activating_radio_port_idx + 1);
                        auto_resolved_conflict_prev_b_relay_ = active_other_relay;
                    }
                } else { // other_radio_id == RadioID::A
                    if (auto_resolved_conflict_prev_a_relay_ == 0) {
                        ESP_LOGI(TAG, "Anticipating Radio A relay %d (port %d) deactivation by Radio B (activating relay %d, port %d). Storing for auto-restoration.",
                                 active_other_relay, other_radio_active_port_idx + 1, relay_id, activating_radio_port_idx + 1);
                        auto_resolved_conflict_prev_a_relay_ = active_other_relay;
                    }
                }
            }
        }
    }

    // Add TX interlock checks here too, if setting a relay ON.
    if (state == true) { // Trying to activate an antenna
        // const auto& config = ConfigManager::instance().get_config(); // Already fetched
        if (radio == RadioID::A) {
            // Hot-switching protection for Radio A itself:
            if (is_radio_a_transmitting_effective()) {
                ESP_LOGW(TAG, "Cannot set antenna for Radio A; Radio A is currently transmitting (effective).");
                return ESP_ERR_INVALID_STATE;
            }
            // Interlock: Check if Radio B is transmitting
            if (config.radio_operation_mode != RADIO_OP_MODE_SINGLE_A && is_radio_b_transmitting_effective()) {
               ESP_LOGW(TAG, "Cannot set antenna for Radio A; Radio B is transmitting (effective).");
               return ESP_ERR_INVALID_STATE;
            }
        } else { // RadioID::B
            if (config.radio_operation_mode == RADIO_OP_MODE_SINGLE_A) {
                 ESP_LOGW(TAG, "Cannot set antenna for Radio B; Single A mode active.");
                 if (state) return ESP_OK; // Acknowledge but don't act on ON request in single A mode
            }
            // Hot-switching protection for Radio B itself:
            if (is_radio_b_transmitting_effective()) {
               ESP_LOGW(TAG, "Cannot set antenna for Radio B; Radio B is currently transmitting (effective).");
               return ESP_ERR_INVALID_STATE;
            }
            // Interlock: Check if Radio A is transmitting
            if (is_radio_a_transmitting_effective()) {
                ESP_LOGW(TAG, "Cannot set antenna for Radio B; Radio A is transmitting (effective).");
                return ESP_ERR_INVALID_STATE;
            }
        }
    }
    
    // Execute the relay change
    esp_err_t ret = relay_controller_->set_relay_for_antenna(relay_id, band_number, radio, state);
    
    // If relay activation was successful, check if we need to restore the other radio's relays
    if (ret == ESP_OK && state && radio == RadioID::A && 
        config.radio_operation_mode == RADIO_OP_MODE_CONCURRENT_AB && 
        config.interlock_auto_resolves_conflict && 
        config.auto_restore_on_conflict_resolution) {
        
        // Check if Radio B has a stored relay that was auto-resolved earlier
        if (auto_resolved_conflict_prev_b_relay_ != 0) {
            // Get the port index of the previously disabled Radio B relay
            int stored_b_port_idx = (auto_resolved_conflict_prev_b_relay_ - 1) % RelayController::RELAYS_PER_RADIO;
            
            // If Radio A's new port is different from the one that caused the conflict,
            // we can attempt to restore Radio B's relay
            if (activating_radio_port_idx != stored_b_port_idx) {
                ESP_LOGI(TAG, "Radio A switched to port %d, which no longer conflicts with stored Radio B port %d. Attempting restoration.",
                         activating_radio_port_idx + 1, stored_b_port_idx + 1);
                attempt_restore_auto_resolved_radio_b_relay();
            }
        }
    }
    
    return ret;
}
