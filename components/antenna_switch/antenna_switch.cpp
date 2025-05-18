#include "antenna_switch.h"
#include "config_manager.h"
#include "esp_log.h"
#include "relay_controller.h"
#include "wifi_manager.hpp"
#include <memory>
#include <nvs.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "cat_parser.h"

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

esp_err_t AntennaSwitch::set_frequency(const uint32_t frequency) {
    ESP_LOGV(TAG, "Setting antenna for frequency: %lu Hz", frequency);

    // we only need a reference to config here as we won't mutate it
    const auto &config = ConfigManager::instance().get_config();
    if (!config.auto_mode) {
        ESP_LOGW(TAG, "Automatic mode is disabled, not changing antenna");
        return ESP_OK;
    }

    // Assuming frequency updates are primarily for Radio A's context in auto mode
    constexpr size_t radio_idx_numeric = static_cast<size_t>(RadioID::A);
    constexpr auto current_radio_context = RadioID::A;

    for (int i = 0; i < config.num_bands; i++) { // i is band_index
        const auto &band_config_for_radio = config.bands[radio_idx_numeric][i];
        if (frequency >= band_config_for_radio.start_freq &&
            frequency <= band_config_for_radio.end_freq) {
            
            int target_relay_id = 0;
            uint8_t preferred_antenna = config.last_used_antenna[radio_idx_numeric][i];

            // Check if preferred antenna is valid (1-based) and configured for this band
            if (preferred_antenna != 0 && 
                preferred_antenna <= config.num_antenna_ports &&
                band_config_for_radio.antenna_ports[preferred_antenna - 1]) {
                target_relay_id = preferred_antenna;
                ESP_LOGI(TAG, "Using preferred antenna %d for Radio %c, Band %d (Freq: %lu Hz)",
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

    auto config = ConfigManager::instance().get_config();
    config.auto_mode = auto_mode;

    return ConfigManager::instance().update_config(config);
}

esp_err_t AntennaSwitch::set_relay(const int relay_id, const bool state) {
    ESP_LOGI(TAG, "Setting relay %d to %s", relay_id, state ? "ON" : "OFF");

    if (relay_id < 1 || relay_id > RelayController::NUM_RELAYS) {
        ESP_LOGE(TAG, "Invalid relay ID: %d", relay_id);
        return ESP_ERR_INVALID_ARG;
    }

    if (!relay_controller_) {
        ESP_LOGE(TAG, "Relay controller not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    const auto &cfg = ConfigManager::instance().get_config();
    const RadioID radio = (relay_id <= RelayController::RELAYS_PER_RADIO)
                    ? RadioID::A
                    : RadioID::B;

    // If Radio B operations are disabled (Single A mode) and this request is for Radio B, ignore ON request.
    if (cfg.radio_operation_mode == RADIO_OP_MODE_SINGLE_A && radio == RadioID::B && state) {
        ESP_LOGW(TAG, "Radio B operations disabled (Single A mode); ignoring ON for relay %d", relay_id);
        return ESP_OK;
    }

    // All interlock and safety logic is now handled by RelayController::execute_relay_change
    // The band_number is -1 here as this is a direct relay set, not tied to a specific band's auto-selection.
    esp_err_t err = relay_controller_->set_relay_for_antenna(relay_id, /*band_number=*/-1, radio, state);

    // If turning a relay ON successfully, update last_used_antenna preference
    if (state && err == ESP_OK) {
        antenna_switch_config_t current_cfg; // Get a mutable copy
        if (get_config(&current_cfg) == ESP_OK) {
            // Use cat_parser_get_frequency() to determine the current band context.
            // This assumes the CAT frequency is relevant for the radio whose relay is being changed.
            uint32_t current_freq = cat_parser_get_frequency(); 
            int band_idx = -1;
            size_t radio_numeric_idx = static_cast<size_t>(radio);

            // Find band_idx for the current frequency on the specific radio
            for (int i = 0; i < current_cfg.num_bands; i++) {
                const auto& band_config_for_radio = current_cfg.bands[radio_numeric_idx][i];
                if (current_freq >= band_config_for_radio.start_freq && current_freq <= band_config_for_radio.end_freq) {
                    band_idx = i;
                    break;
                }
            }

            if (band_idx != -1) {
                // Check if the selected relay is actually configured for this band for this radio
                if (relay_id > 0 && relay_id <= current_cfg.num_antenna_ports &&
                    current_cfg.bands[radio_numeric_idx][band_idx].antenna_ports[relay_id - 1]) {

                    if (current_cfg.last_used_antenna[radio_numeric_idx][band_idx] != static_cast<uint8_t>(relay_id)) {
                        ESP_LOGI(TAG, "Updating last used antenna for Radio %c, Band %d to Relay %d (Freq: %lu Hz)",
                                 (radio == RadioID::A ? 'A' : 'B'), band_idx, relay_id, current_freq);
                        current_cfg.last_used_antenna[radio_numeric_idx][band_idx] = static_cast<uint8_t>(relay_id);
                        
                        // Persist this change by calling update_config which handles saving to NVS
                        esp_err_t save_err = ConfigManager::instance().update_config(current_cfg);
                        if (save_err != ESP_OK) {
                            ESP_LOGE(TAG, "Failed to save updated last_used_antenna: %s", esp_err_to_name(save_err));
                        }
                    }
                } else {
                    ESP_LOGW(TAG, "Relay %d is not configured for Radio %c, Band %d (Freq: %lu Hz). Not updating preference.",
                             relay_id, (radio == RadioID::A ? 'A' : 'B'), band_idx, current_freq);
                }
            } else {
                ESP_LOGD(TAG, "No current band found for Radio %c at Freq: %lu Hz. Not updating preference for relay %d.",
                         (radio == RadioID::A ? 'A' : 'B'), current_freq, relay_id);
            }
        }
    }
    return err;
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
    // or let set_relay_for_antenna determine the radio based on relay_id.
    // For clarity, explicitly setting RadioID::B.
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

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("antenna_switch", NVS_READWRITE, &nvs_handle);

    if (err == ESP_OK) {
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    vTaskDelay(pdMS_TO_TICKS(1000));

    esp_restart();
}

esp_err_t AntennaSwitch::set_relay_for_antenna(int relay_id, int band_number, RadioID radio, bool state) {
    if (!relay_controller_) {
        ESP_LOGE(TAG, "Relay controller not initialized for set_relay_for_antenna");
        return ESP_ERR_INVALID_STATE;
    }
    return relay_controller_->set_relay_for_antenna(relay_id, band_number, radio, state);
}
