#include "config_manager.h"
#include "antenna_switch.h"
#include "esp_log.h"
#include "nvs.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include <cstring>
#include <sys/param.h>

static auto TAG = "CONFIG_MANAGER";

// Initialize static member
ConfigManager *ConfigManager::instance_ = nullptr;

ConfigManager::ConfigManager() {
    current_config_ = new antenna_switch_config_t();
    if (instance_ == nullptr) {
        instance_ = this;
    }
}

ConfigManager::~ConfigManager() {
    delete current_config_;
}

ConfigManager &ConfigManager::instance() {
    static ConfigManager instance;
    return instance;
}

esp_err_t ConfigManager::init() const {
    ESP_LOGI(TAG, "Initializing configuration manager");

    // Try to load from NVS
    esp_err_t ret = load_from_nvs();

    // If no config exists _or_ the blob is the wrong size, create default
    if (ret == ESP_ERR_NVS_NOT_FOUND || ret == ESP_ERR_NVS_INVALID_LENGTH) {
        ESP_LOGW(TAG, "No configuration found in NVS, using defaults");

        // Set default configuration
        current_config_->num_bands = 10;
        current_config_->auto_mode = true;
        current_config_->num_antenna_ports = 6;
        current_config_->uart_baud_rate = 9600;
        current_config_->uart_parity = UART_PARITY_DISABLE;
        current_config_->uart_stop_bits = UART_STOP_BITS_1;
        current_config_->uart_flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
        current_config_->uart_rx_pin = GPIO_NUM_32; // HT1
        current_config_->uart_tx_pin = GPIO_NUM_33; // HT2

        // Whether to allow data from both uart and MQTT at the same time
        current_config_->allow_concurrent_data_sources = true;  // First come first serve

        // MQTT defaults
        current_config_->mqtt_enabled = true;  // default to enabled
        current_config_->mqtt_port = 1883;     // default MQTT port
        strncpy(current_config_->mqtt_broker, "mqtt://localhost", sizeof(current_config_->mqtt_broker));
        strncpy(current_config_->mqtt_rig_id, "rig1", sizeof(current_config_->mqtt_rig_id));
        strncpy(current_config_->mqtt_username, "mqtt", sizeof(current_config_->mqtt_username));
        strncpy(current_config_->mqtt_password, "mqtt", sizeof(current_config_->mqtt_password));
        strncpy(current_config_->mqtt_client_id, "core-mosquitto", sizeof(current_config_->mqtt_client_id));
        strncpy(current_config_->mqtt_username, "mqtt", sizeof(current_config_->mqtt_username));
        strncpy(current_config_->mqtt_password, "mqtt", sizeof(current_config_->mqtt_password));
        strncpy(current_config_->mqtt_topic, "omnirig/frequent/radio_info", sizeof(current_config_->mqtt_topic));
        
        // Default for radio operation mode and interlock
        current_config_->radio_operation_mode = RADIO_OP_MODE_SINGLE_A; // Default to Radio A only
        current_config_->interlock_auto_resolves_conflict = true; // Default to true for safety if concurrent mode is chosen

        // Set default bands for both radios
        const char* default_band_names[10] = {
            "160m", "80m", "40m", "30m", "20m", "17m", "15m", "12m", "10m", "6m"
        };
        uint32_t default_start[10] = {
            1800000, 3500000, 7000000, 10100000, 14000000, 18068000, 21000000, 24890000, 28000000, 50000000
        };
        uint32_t default_end[10] = {
            2000000, 4000000, 7300000, 10150000, 14350000, 18168000, 21450000, 24990000, 29700000, 54000000
        };
        for (int r = 0; r < 2; ++r) {
            for (int i = 0; i < 10; ++i) {
                strncpy(current_config_->bands[r][i].description, default_band_names[i], sizeof(current_config_->bands[r][i].description));
                current_config_->bands[r][i].start_freq = default_start[i];
                current_config_->bands[r][i].end_freq = default_end[i];
                for (int j = 0; j < MAX_ANTENNA_PORTS; ++j) {
                    current_config_->bands[r][i].antenna_ports[j] = (j == 0); // Only first port enabled by default
                }
            }
        }

        // Save default configuration
        ret = save_to_nvs();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save default configuration: %s", esp_err_to_name(ret));
            return ret;
        }
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error loading configuration: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

esp_err_t ConfigManager::update_config(const antenna_switch_config_t &new_config) {
    ESP_LOGI(TAG, "Updating configuration");

    // Validate configuration
    if (new_config.num_bands <= 0 || new_config.num_bands > MAX_BANDS) {
        ESP_LOGE(TAG, "Invalid number of bands: %d", new_config.num_bands);
        return ESP_ERR_INVALID_ARG;
    }

    if (new_config.num_antenna_ports <= 0 || new_config.num_antenna_ports > MAX_ANTENNA_PORTS) {
        ESP_LOGE(TAG, "Invalid number of antenna ports: %d", new_config.num_antenna_ports);
        return ESP_ERR_INVALID_ARG;
    }

    // Update configuration
    *current_config_ = new_config;

    // Save new configuration to NVS
    if (const esp_err_t ret = save_to_nvs(); ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save configuration: %s", esp_err_to_name(ret));
        return ret;
    }

    // Notify observers
    for (const auto &observer: observers_) {
        observer(*current_config_);
    }

    return ESP_OK;
}

esp_err_t ConfigManager::save_to_nvs() const {
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open("antenna_switch", NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(ret));
        return ret;
    }

    // Save all config except bands
    ret = nvs_set_blob(nvs_handle, "config", current_config_, sizeof(antenna_switch_config_t) - sizeof(current_config_->bands));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error saving configuration to NVS: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    // Save per-radio band-tables
    for (int r = 0; r < 2; ++r) {
        for (int i = 0; i < current_config_->num_bands; ++i) {
            char key[40];
            snprintf(key, sizeof(key), "band_r%d_%d_desc", r, i);
            nvs_set_str(nvs_handle, key, current_config_->bands[r][i].description);

            snprintf(key, sizeof(key), "band_r%d_%d_start", r, i);
            nvs_set_u32(nvs_handle, key, current_config_->bands[r][i].start_freq);

            snprintf(key, sizeof(key), "band_r%d_%d_end", r, i);
            nvs_set_u32(nvs_handle, key, current_config_->bands[r][i].end_freq);

            for (int j = 0; j < MAX_ANTENNA_PORTS; ++j) {
                snprintf(key, sizeof(key), "band_r%d_%d_port%d", r, i, j);
                nvs_set_u8(nvs_handle, key, current_config_->bands[r][i].antenna_ports[j] ? 1 : 0);
            }
        }
    }

    ret = nvs_commit(nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error committing NVS changes: %s", esp_err_to_name(ret));
    }

    nvs_close(nvs_handle);
    return ret;
}

esp_err_t ConfigManager::load_from_nvs() const {
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open("antenna_switch", NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(ret));
        return ret;
    }

    // Load all config except bands
    size_t required_size = sizeof(antenna_switch_config_t) - sizeof(current_config_->bands);
    ret = nvs_get_blob(nvs_handle, "config", current_config_, &required_size);
    if (ret != ESP_OK) {
        nvs_close(nvs_handle);
        return ret;
    }

    // Load per-radio band-tables
    for (int r = 0; r < 2; ++r) {
        for (int i = 0; i < current_config_->num_bands; ++i) {
            char key[40];
            size_t len = sizeof(current_config_->bands[r][i].description);
            snprintf(key, sizeof(key), "band_r%d_%d_desc", r, i);
            nvs_get_str(nvs_handle, key, current_config_->bands[r][i].description, &len);

            snprintf(key, sizeof(key), "band_r%d_%d_start", r, i);
            nvs_get_u32(nvs_handle, key, &current_config_->bands[r][i].start_freq);

            snprintf(key, sizeof(key), "band_r%d_%d_end", r, i);
            nvs_get_u32(nvs_handle, key, &current_config_->bands[r][i].end_freq);

            for (int j = 0; j < MAX_ANTENNA_PORTS; ++j) {
                snprintf(key, sizeof(key), "band_r%d_%d_port%d", r, i, j);
                uint8_t val = 0;
                nvs_get_u8(nvs_handle, key, &val);
                current_config_->bands[r][i].antenna_ports[j] = (val != 0);
            }
        }
    }

    nvs_close(nvs_handle);
    return ESP_OK;
}

void ConfigManager::add_observer(const std::function<void(const antenna_switch_config_t &)> &observer) {
    observers_.push_back(observer);
    // Immediately notify the new observer of current config
    observer(*current_config_);
}
