#include "antenna_switch.h"
#include "config_manager.h"
#include "esp_log.h"
#include "relay_controller.h"
#include "wifi_manager.hpp"
#include <memory>
#include <nvs.h>
#include "esp_wifi.h"
#include "esp_system.h"

static auto TAG = "ANTENNA_SWITCH";
static RelayController* relay_controller = nullptr;

[[maybe_unused]] static esp_err_t get_ip_address(char *ip_addr, const size_t max_len) {
    if (!ip_addr || max_len < 16) {
        // IPv4 address max length is 15 chars + null terminator
        return ESP_ERR_INVALID_ARG;
    }

    return WifiManager::instance().get_ip_info(ip_addr, max_len);
}


esp_err_t antenna_switch_init() {
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

void antenna_switch_set_relay_controller(RelayController* controller) {
    relay_controller = controller;
}

esp_err_t antenna_switch_set_config(const antenna_switch_config_t *config) {
    if (config == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    return ConfigManager::instance().update_config(*config);
}

esp_err_t antenna_switch_get_config(antenna_switch_config_t *config) {
    if (config == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *config = ConfigManager::instance().get_config();
    return ESP_OK;
}

esp_err_t antenna_switch_set_frequency(const uint32_t frequency) {
    ESP_LOGV(TAG, "Setting antenna for frequency: %lu Hz", frequency);

    // we only need a reference to config here as we won't mutate it
    const auto &config = ConfigManager::instance().get_config();
    if (!config.auto_mode) {
        ESP_LOGW(TAG, "Automatic mode is disabled, not changing antenna");
        return ESP_OK;
    }

    constexpr size_t RADIO_IDX = 0;
    for (int i = 0; i < config.num_bands; i++) {
        const auto &band = config.bands[RADIO_IDX][i];
        if (frequency >= band.start_freq &&
            frequency <= band.end_freq) {
            // Find the first available antenna port for this band
            for (int j = 0; j < config.num_antenna_ports; j++) {
                if (band.antenna_ports[j]) {
                    ESP_LOGI(TAG, "Selecting relay %d for band %d for Radio A", j + 1, i);
                    // Use the RelayController to set the appropriate relay, turning it ON
                    if (!relay_controller) return ESP_ERR_INVALID_STATE;
                    return relay_controller->set_relay_for_antenna(j + 1, i, RadioID::A, true);
                }
            }

            ESP_LOGW(TAG, "No available antenna port found for band %d", i);
            return ESP_OK;
        }
    }

    ESP_LOGV(TAG, "Config does not support frequency: %lu Hz", frequency);
    return ESP_OK;
}

/**
 * Whether automatic band switching should be used
 * @param auto_mode
 * @return esp_err_t
 */
esp_err_t antenna_switch_set_auto_mode(const bool auto_mode) {
    ESP_LOGD(TAG, "Setting auto mode: %s", auto_mode ? "ON" : "OFF");

    auto config = ConfigManager::instance().get_config();
    config.auto_mode = auto_mode;

    return ConfigManager::instance().update_config(config);
}

esp_err_t antenna_switch_set_relay(const int relay_id, const bool state) {
    ESP_LOGI(TAG, "Setting relay %d to %s", relay_id, state ? "ON" : "OFF");

    if (relay_id < 1 || relay_id > RelayController::NUM_RELAYS) {
        ESP_LOGE(TAG, "Invalid relay ID: %d", relay_id);
        return ESP_ERR_INVALID_ARG;
    }

    if (!relay_controller) {
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
    return relay_controller->set_relay_for_antenna(relay_id, /*band_number=*/-1, radio, state);
}

esp_err_t antenna_switch_set_relay_radio_b(int relay_id, bool state) {
    const auto &cfg = ConfigManager::instance().get_config();
    if (cfg.radio_operation_mode == RADIO_OP_MODE_SINGLE_A && state) { // Only block if trying to turn ON when Radio B is disabled
        ESP_LOGW(TAG, "Radio B operations disabled (Single A mode), cannot turn ON relay %d for Radio B", relay_id);
        return ESP_OK; 
    }
    if (!relay_controller) {
        ESP_LOGE(TAG, "Relay controller not initialized for Radio B set relay");
        return ESP_ERR_INVALID_STATE;
    }
    // Ensure relay_id is within Radio B's range (e.g., 9-16 if RELAYS_PER_RADIO is 8)
    // or let set_relay_for_antenna determine the radio based on relay_id.
    // For clarity, explicitly setting RadioID::B.
    // The band_number is -1 as this is a direct relay set.
    return relay_controller->set_relay_for_antenna(relay_id, /*band_number=*/-1, RadioID::B, state);
}

esp_err_t antenna_switch_get_relay_state(const int relay_id, bool *state) {
    if (state == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!relay_controller) {
        ESP_LOGE(TAG, "Relay controller not initialized for get_relay_state");
        return ESP_ERR_INVALID_STATE;
    }
    *state = relay_controller->get_relay_state(relay_id);
    return ESP_OK;
}

esp_err_t antenna_switch_restart() {
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
