

#include "antenna_switch.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "relay_controller.h"
#include <cstring>

static const char *TAG = "ANTENNA_SWITCH";

static antenna_switch_config_t current_config;

esp_err_t antenna_switch_init()
{
    ESP_LOGI(TAG, "Initializing antenna switch");
    
    // Load configuration from NVS
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("antenna_switch", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return err;
    }

    size_t required_size = sizeof(antenna_switch_config_t);
    err = nvs_get_blob(nvs_handle, "config", &current_config, &required_size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No configuration found in NVS, using defaults");
        // Set default configuration
        current_config.num_bands = 10;
        current_config.auto_mode = true;
        current_config.num_antenna_ports = 6; // default
        
        // Try to save the default configuration
        err = nvs_set_blob(nvs_handle, "config", &current_config, sizeof(antenna_switch_config_t));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save default configuration to NVS: %s", esp_err_to_name(err));
        } else {
            err = nvs_commit(nvs_handle);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to commit default configuration to NVS: %s", esp_err_to_name(err));
            } else {
                ESP_LOGI(TAG, "Default configuration saved to NVS");
            }
        }
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error reading configuration from NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    } else {
        ESP_LOGI(TAG, "Configuration loaded from NVS successfully");
        ESP_LOGI(TAG, "Loaded config: num_bands=%d, auto_mode=%d, num_antenna_ports=%d",
                 current_config.num_bands, current_config.auto_mode, current_config.num_antenna_ports);
    }

    nvs_close(nvs_handle);
    return ESP_OK;
}

esp_err_t antenna_switch_set_config(const antenna_switch_config_t *config)
{
    ESP_LOGI(TAG, "Setting antenna switch configuration");
    
    if (config == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(&current_config, config, sizeof(antenna_switch_config_t));

    // Save configuration to NVS
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("antenna_switch", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Saving config: num_bands=%d, auto_mode=%d, num_antenna_ports=%d",
             current_config.num_bands, current_config.auto_mode, current_config.num_antenna_ports);

    err = nvs_set_blob(nvs_handle, "config", &current_config, sizeof(antenna_switch_config_t));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving configuration to NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error committing NVS changes: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Configuration saved to NVS successfully");
    }

    nvs_close(nvs_handle);
    return err;
}

esp_err_t antenna_switch_get_config(antenna_switch_config_t *config)
{
    if (config == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    std::memcpy(config, &current_config, sizeof(antenna_switch_config_t));
    return ESP_OK;
}

esp_err_t antenna_switch_set_frequency(uint32_t frequency)
{
    ESP_LOGI(TAG, "Setting antenna for frequency: %lu Hz", frequency);

    if (!current_config.auto_mode) {
        ESP_LOGI(TAG, "Auto mode is disabled, not changing antenna");
        return ESP_OK;
    }

    for (int i = 0; i < current_config.num_bands; i++) {
        if (frequency >= current_config.bands[i].start_freq && 
            frequency <= current_config.bands[i].end_freq) {
            ESP_LOGI(TAG, "Selecting antenna for band %d", i);
            return relay_controller_set_antenna(current_config.bands[i].antenna_ports);
        }
    }

    ESP_LOGW(TAG, "No matching band found for frequency: %lu Hz", frequency);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t antenna_switch_set_auto_mode(bool auto_mode)
{
    ESP_LOGI(TAG, "Setting auto mode: %s", auto_mode ? "ON" : "OFF");
    current_config.auto_mode = auto_mode;

    // Save the auto mode setting to NVS
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("antenna_switch", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(nvs_handle, "config", &current_config, sizeof(antenna_switch_config_t));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving configuration to NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error committing NVS changes: %s", esp_err_to_name(err));
    }

    nvs_close(nvs_handle);
    return err;
}
