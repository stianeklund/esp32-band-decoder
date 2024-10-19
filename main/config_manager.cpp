#include "config_manager.h"
#include "antenna_switch.h"
#include "esp_log.h"
#include "nvs.h"
#include "driver/uart.h"
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

    // Try to load from NVS first
    esp_err_t ret = load_from_nvs();

    // If no config exists, create default
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "No configuration found in NVS, using defaults");

        // Set default configuration
        current_config_->num_bands = 10;
        current_config_->auto_mode = true;
        current_config_->num_antenna_ports = 6;
        strcpy(current_config_->tcp_host, "192.168.1.100"); // Default to a more typical remote host
        current_config_->tcp_port = 12090;
        current_config_->uart_baud_rate = 9600;
        current_config_->uart_parity = UART_PARITY_DISABLE;
        current_config_->uart_stop_bits = UART_STOP_BITS_1;
        current_config_->uart_flow_ctrl = UART_HW_FLOWCTRL_DISABLE;

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

    // Validate TCP host after loading/setting defaults
    if (strlen(current_config_->tcp_host) == 0) {
        ESP_LOGW(TAG, "TCP host is empty, setting default");
        strcpy(current_config_->tcp_host, "192.168.1.100");
        ret = save_to_nvs();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save default TCP host: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    ESP_LOGI(TAG, "Using TCP host: %s:%d", current_config_->tcp_host, current_config_->tcp_port);

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

    ret = nvs_set_blob(nvs_handle, "config", current_config_, sizeof(antenna_switch_config_t));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error saving configuration to NVS: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
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

    size_t required_size = sizeof(antenna_switch_config_t);
    ret = nvs_get_blob(nvs_handle, "config", current_config_, &required_size);

    nvs_close(nvs_handle);
    return ret;
}

void ConfigManager::add_observer(const std::function<void(const antenna_switch_config_t &)> &observer) {
    observers_.push_back(observer);
    // Immediately notify the new observer of current config
    observer(*current_config_);
}
