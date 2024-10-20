

#include "antenna_switch.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "relay_controller.h"
#include <cstring>
#include <memory>
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_system.h"

static const char *TAG = "ANTENNA_SWITCH";
static antenna_switch_config_t current_config;
static std::unique_ptr<RelayController> relay_controller;

static esp_err_t get_ip_address(char* ip_addr, size_t max_len)
{
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL) {
        ESP_LOGE(TAG, "Failed to get netif handle");
        return ESP_FAIL;
    }

    esp_netif_ip_info_t ip_info;
    esp_err_t ret = esp_netif_get_ip_info(netif, &ip_info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get IP info: %s", esp_err_to_name(ret));
        return ret;
    }

    snprintf(ip_addr, max_len, IPSTR, IP2STR(&ip_info.ip));
    return ESP_OK;
}

esp_err_t antenna_switch_set_udp_host(const char* host)
{
    if (host == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(current_config.udp_host, host, sizeof(current_config.udp_host) - 1);
    current_config.udp_host[sizeof(current_config.udp_host) - 1] = '\0';

    // Update the UDP host in the RelayController
    if (relay_controller) {
        relay_controller->set_udp_host(host);
    }

    // Save the updated configuration to NVS
    return antenna_switch_set_config(&current_config);
}




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
        strcpy(current_config.udp_host, ""); // Clear the UDP host
        current_config.udp_port = 8888; // default UDP port
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error reading configuration from NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    nvs_close(nvs_handle);

    // Get the current IP address
    char ip_addr[16];
    err = get_ip_address(ip_addr, sizeof(ip_addr));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get IP address: %s", esp_err_to_name(err));
        return err;
    }

    // Update the UDP host with the current IP address
    antenna_switch_set_udp_host(ip_addr);

    // Initialize RelayController
    relay_controller = std::make_unique<RelayController>();
    err = relay_controller->init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error initializing RelayController: %s", esp_err_to_name(err));
        return err;
    }

    // Set UDP settings in RelayController
    err = relay_controller->update_udp_settings(current_config.udp_host, current_config.udp_port);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error setting UDP settings in RelayController: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Antenna switch initialized with UDP host: %s, port: %d", current_config.udp_host, current_config.udp_port);

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

    // Update UDP settings in the RelayController
    if (relay_controller) {
        err = relay_controller->update_udp_settings(config->udp_host, config->udp_port);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error updating UDP settings in RelayController: %s", esp_err_to_name(err));
            return err;
        }
    }

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
            
            // Find the first available antenna port for this band
            for (int j = 0; j < current_config.num_antenna_ports; j++) {
                if (current_config.bands[i].antenna_ports[j]) {
                    ESP_LOGI(TAG, "Selecting relay %d for band %d", j + 1, i);
                    // Use the RelayController to set the appropriate relay
                    return relay_controller->set_relay_for_antenna(j + 1, i);
                }
            }
            
            ESP_LOGW(TAG, "No available antenna port found for band %d", i);
            return ESP_ERR_NOT_FOUND;
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
esp_err_t antenna_switch_set_relay(int relay_id, bool state)
{
    ESP_LOGI(TAG, "Setting relay %d to %s", relay_id, state ? "ON" : "OFF");
    return relay_controller->set_relay(relay_id, state);
}

esp_err_t antenna_switch_get_relay_state(int relay_id, bool *state)
{
    if (state == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *state = relay_controller->get_relay_state(relay_id);
    return ESP_OK;
}

