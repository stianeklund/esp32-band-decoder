#include "relay_controller.h"
#include "esp_log.h"
#include "freertos/projdefs.h"
#include <algorithm>
#include <sstream>
#include <freertos/mpu_wrappers.h>
#include "freertos/task.h"
#include "esp_netif.h"
#include "esp_wifi.h"

static const char *TAG = "RELAY_CONTROLLER";
const int maxRetries = 3;
const int retryDelay = 100;  // milliseconds


RelayController::RelayController()
    : currently_selected_relay_(0), last_band_change_time_(std::chrono::steady_clock::now()),
      udp_host_(""), udp_port_() {
    for (int i = 1; i <= NUM_RELAYS; ++i) {
        relay_states_[i] = false;
    }
    udp_client = std::make_unique<UDPClient>();
}

RelayController::~RelayController() = default;

esp_err_t RelayController::init() {
    ESP_LOGI(TAG, "Initializing relay controller");

    if (udp_host_.empty()) {
        ESP_LOGE(TAG, "UDP host not set. Please set the UDP host before initializing.");
        return ESP_ERR_INVALID_STATE;
    }

    // Initialize UDP client
    esp_err_t ret = udp_client->init(udp_host_.c_str(), udp_port_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error initializing UDP client: %s", esp_err_to_name(ret));
        return ret;
    }

    for (int i = 1; i <= NUM_RELAYS; ++i) {
        relay_states_[i] = false;
    }
    currently_selected_relay_ = 0;

    ESP_LOGI(TAG, "Relay controller initialized with UDP host: %s, port: %d", udp_host_.c_str(), udp_port_);
    return ESP_OK;
}

void RelayController::set_udp_host(const std::string& host) {
    udp_host_ = host;
    ESP_LOGI(TAG, "UDP host set to: %s", udp_host_.c_str());
    if (udp_client) {
        esp_err_t ret = udp_client->init(udp_host_.c_str(), udp_port_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Error reinitializing UDP client: %s", esp_err_to_name(ret));
        }
    }
}

esp_err_t RelayController::update_relay_states() {
    ESP_LOGI(TAG, "Updating relay states");

    esp_err_t ret = update_all_relay_states();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to update relay states: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

esp_err_t RelayController::update_udp_settings(const std::string& host, uint16_t port) {
    if (host != udp_host_ || port != udp_port_) {
        udp_host_ = host;
        udp_port_ = port;

        // Re-initialize UDP client with new settings
        esp_err_t ret = udp_client->init(udp_host_.c_str(), udp_port_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Error re-initializing UDP client: %s", esp_err_to_name(ret));
            return ret;
        }

        ESP_LOGI(TAG, "UDP settings updated. New host: %s, new port: %d", udp_host_.c_str(), udp_port_);
    } else {
        ESP_LOGI(TAG, "UDP settings unchanged. Host: %s, port: %d", udp_host_.c_str(), udp_port_);
    }

    return ESP_OK;
}


void RelayController::set_udp_port(uint16_t port) {
    udp_port_ = port;
}

std::string RelayController::get_udp_host() const {
    return udp_host_;
}

uint16_t RelayController::get_udp_port() const {
    return udp_port_;
}

esp_err_t RelayController::turn_off_all_relays() {
    ESP_LOGI(TAG, "Turning off all relays");

    std::string command = "RELAY-AOF-255,1,1";
    std::string expected_response_suffix = "255,1,1,OK";
    
    esp_err_t ret = send_command_and_validate_response(command, expected_response_suffix);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to turn off all relays: %s", esp_err_to_name(ret));
        return ret;
    }

    // Update local state
    for (int i = 1; i <= NUM_RELAYS; ++i) {
        relay_states_[i] = false;
    }
    currently_selected_relay_ = 0;

    return ESP_OK;
}

esp_err_t RelayController::set_relay(int relayId, bool state) {
    if (relayId < 1 || relayId > NUM_RELAYS) {
        ESP_LOGE(TAG, "Invalid relay ID: %d", relayId);
        return ESP_ERR_INVALID_ARG;
    }

    // If the relay is already in the desired state, do nothing.
    if (relay_states_[relayId] == state) {
        ESP_LOGI(TAG, "Relay %d already in desired state", relayId);
        return ESP_OK;
    }

    std::string command = "RELAY-SET-255," + std::to_string(relayId) + "," + (state ? "1" : "0");
    std::string expected_response_suffix = "255," + std::to_string(relayId) + "," + (state ? "1" : "0") + ",OK";

    esp_err_t ret = send_command_and_validate_response(command, expected_response_suffix);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set relay %d to state %d: %s", relayId, state, esp_err_to_name(ret));
        return ret;
    }

    // Update local state
    relay_states_[relayId] = state;
    if (state) {
        currently_selected_relay_ = relayId;
    } else if (currently_selected_relay_ == relayId) {
        currently_selected_relay_ = 0;
    }

    return ESP_OK;
}

bool RelayController::get_relay_state(int relay_id) const {
    if (relay_id < 1 || relay_id > NUM_RELAYS) {
        ESP_LOGE(TAG, "Invalid relay ID: %d", relay_id);
        return false;
    }
    return relay_states_.at(relay_id);
}

std::map<int, bool> RelayController::get_all_relay_states() const {
    return relay_states_;
}

esp_err_t RelayController::update_all_relay_states() {
    ESP_LOGI(TAG, "Getting state of all relays");

    std::string command = "RELAY-STATE-255";
    std::string expected_response_prefix = "RELAY-STATE-255,";
    
    std::string response;
    esp_err_t ret = ESP_OK;
    int retry_count = 0;
    const int max_retries = 5;
    const int retry_delay_ms = 500;

    while (retry_count < max_retries) {
        // Re-initialize UDP client with current settings before sending
        ret = udp_client->init(udp_host_.c_str(), udp_port_);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to re-initialize UDP client: %s. Retry %d/%d", esp_err_to_name(ret), retry_count + 1, max_retries);
            retry_count++;
            vTaskDelay(pdMS_TO_TICKS(retry_delay_ms));
            continue;
        }

        ret = udp_client->send_message(command);
        ESP_LOGI(TAG, "Sending %s to: %s : %d", command.c_str(), udp_host_.c_str(), udp_port_);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to send get all relay states message: %s. Retry %d/%d", esp_err_to_name(ret), retry_count + 1, max_retries);
            retry_count++;
            vTaskDelay(pdMS_TO_TICKS(retry_delay_ms));
            continue;
        }

        // Implement a custom receive with timeout
        const int receive_timeout_ms = 3000;
        const int receive_interval_ms = 100;
        int elapsed_time = 0;

        while (elapsed_time < receive_timeout_ms) {
            ret = udp_client->receive_message(response, 0); // Non-blocking receive
            if (ret == ESP_OK) {
                break; // Successful response
            } else if (ret != ESP_ERR_TIMEOUT) {
                ESP_LOGW(TAG, "Error receiving response: %s. Retry %d/%d", esp_err_to_name(ret), retry_count + 1, max_retries);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(receive_interval_ms));
            elapsed_time += receive_interval_ms;
        }

        if (ret == ESP_OK) {
            break; // Successful response, exit the retry loop
        }

        ESP_LOGW(TAG, "Timeout receiving response for get all relay states. Retry %d/%d", retry_count + 1, max_retries);
        retry_count++;
        vTaskDelay(pdMS_TO_TICKS(retry_delay_ms));
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get relay states after %d retries", max_retries);
        return ret;
    }

    if (response.substr(0, expected_response_prefix.length()) != expected_response_prefix) {
        ESP_LOGE(TAG, "Unexpected response format: %s", response.c_str());
        return ESP_FAIL;
    }

    // Parse the response and update local state
    // The response format is expected to be like: "RELAY-STATE-255,0,5,OK"
    std::vector<std::string> parts;
    std::stringstream ss(response);
    std::string item;
    while (std::getline(ss, item, ',')) {
        parts.push_back(item);
    }

    if (parts.size() != 4 || parts[0] != "RELAY-STATE-255" || parts[3] != "OK") {
        ESP_LOGE(TAG, "Invalid response format: %s", response.c_str());
        return ESP_FAIL;
    }

    uint8_t d1 = std::stoi(parts[1]);
    uint8_t d0 = std::stoi(parts[2]);

    for (int i = 0; i < 8; ++i) {
        relay_states_[i + 1] = (d0 & (1 << i)) != 0;
        relay_states_[i + 9] = (d1 & (1 << i)) != 0;
    }

    // Update currently_selected_relay_
    currently_selected_relay_ = 0;
    for (int i = 1; i <= NUM_RELAYS; ++i) {
        if (relay_states_[i]) {
            currently_selected_relay_ = i;
            break;
        }
    }

    ESP_LOGI(TAG, "Updated relay states: D1=%d, D0=%d", d1, d0);

    return ESP_OK;
}

esp_err_t RelayController::send_command_and_validate_response(const std::string& command, const std::string& expected_response_suffix) {
    const int max_retries = 3;
    const int retry_delay_ms = 500;
    const int receive_timeout_ms = 2000;

    for (int retry = 0; retry < max_retries; ++retry) {
        // Re-initialize UDP client with current settings before sending
        esp_err_t ret = udp_client->init(udp_host_.c_str(), udp_port_);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to re-initialize UDP client: %s. Retry %d/%d", esp_err_to_name(ret), retry + 1, max_retries);
            vTaskDelay(pdMS_TO_TICKS(retry_delay_ms));
            continue;
        }

        ret = udp_client->send_message(command);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to send command: %s. Retry %d/%d", esp_err_to_name(ret), retry + 1, max_retries);
            vTaskDelay(pdMS_TO_TICKS(retry_delay_ms));
            continue;
        }

        ESP_LOGI(TAG, "Command sent: %s", command.c_str());

        std::string response;
        ret = udp_client->receive_message(response, receive_timeout_ms);
        ESP_LOGI(TAG, "UDP Response received: '%s'. Expected response suffix: '%s'", response.c_str(), expected_response_suffix.c_str());
        
        if (ret == ESP_OK) {
            if (response.length() >= expected_response_suffix.length() &&
                response.compare(response.length() - expected_response_suffix.length(), expected_response_suffix.length(), expected_response_suffix) == 0) {
                ESP_LOGI(TAG, "Command successful. Response matches expected suffix.");
                return ESP_OK;
            } else {
                ESP_LOGW(TAG, "Unexpected response. Expected suffix: '%s', Received: '%s'. Retry %d/%d", 
                         expected_response_suffix.c_str(), response.c_str(), retry + 1, max_retries);
            }
        } else if (ret == ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "Timeout waiting for response. Retry %d/%d", retry + 1, max_retries);
        } else {
            ESP_LOGW(TAG, "Failed to receive response: %s. Retry %d/%d", esp_err_to_name(ret), retry + 1, max_retries);
        }

        // Log the current UDP settings
        ESP_LOGI(TAG, "Current UDP settings - Host: %s, Port: %d", udp_host_.c_str(), udp_port_);

        // Add network diagnostics
        char ip_info[64];
        esp_netif_ip_info_t ip_info_struct;
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif != NULL && esp_netif_get_ip_info(netif, &ip_info_struct) == ESP_OK) {
            snprintf(ip_info, sizeof(ip_info), "IP: %ld.%ld.%ld.%ld, GW: %ld.%ld.%ld.%ld",
                     ip_info_struct.ip.addr & 0xff, (ip_info_struct.ip.addr >> 8) & 0xff,
                     (ip_info_struct.ip.addr >> 16) & 0xff, (ip_info_struct.ip.addr >> 24) & 0xff,
                     ip_info_struct.gw.addr & 0xff, (ip_info_struct.gw.addr >> 8) & 0xff,
                     (ip_info_struct.gw.addr >> 16) & 0xff, (ip_info_struct.gw.addr >> 24) & 0xff);
            ESP_LOGI(TAG, "Network info: %s", ip_info);
        } else {
            ESP_LOGW(TAG, "Failed to get network info");
        }

        // Add a small delay before retrying
        vTaskDelay(pdMS_TO_TICKS(retry_delay_ms));
    }

    ESP_LOGE(TAG, "Failed to send command and validate response after %d retries", max_retries);
    return ESP_FAIL;
}

esp_err_t RelayController::set_relay_for_antenna(int relay_id, int band_number) {
    if (relay_id < 1 || relay_id > NUM_RELAYS) {
        ESP_LOGE(TAG, "Invalid relay ID: %d", relay_id);
        return ESP_ERR_INVALID_ARG;
    }

    if (currently_selected_relay_ == relay_id && relay_states_[relay_id]) {
        ESP_LOGI(TAG, "Relay %d already set. No change needed.", relay_id);
        return ESP_OK;
    }

    if (should_delay()) {
        vTaskDelay(pdMS_TO_TICKS(COOLDOWN_PERIOD_MS));
    }

    return execute_relay_change(relay_id, band_number);
}

esp_err_t RelayController::turn_off_all_relays_except(int relay_to_keep_on) {
    if (relay_to_keep_on < 1 || relay_to_keep_on > NUM_RELAYS) {
        ESP_LOGE(TAG, "Invalid relay ID: %d", relay_to_keep_on);
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t d1 = 0, d0 = 0;
    if (relay_to_keep_on <= 8) {
        d0 = 1 << (relay_to_keep_on - 1);
    } else {
        d1 = 1 << (relay_to_keep_on - 9);
    }

    std::stringstream ss;
    ss << "RELAY-SET_ALL-255," << static_cast<int>(d1) << "," << static_cast<int>(d0);
    std::string command = ss.str();
    std::string expected_response_suffix = "255," + std::to_string(d1) + "," + std::to_string(d0) + ",OK";

    ESP_LOGI(TAG, "Sending command: %s", command.c_str());

    esp_err_t ret = send_command_and_validate_response(command, expected_response_suffix);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set all relays: %s", esp_err_to_name(ret));
        return ret;
    }

    for (int i = 1; i <= NUM_RELAYS; ++i) {
        relay_states_[i] = (i == relay_to_keep_on);
    }
    currently_selected_relay_ = relay_to_keep_on;

    ESP_LOGI(TAG, "All relays turned off except relay %d", relay_to_keep_on);
    return ESP_OK;
}

int RelayController::get_last_selected_relay_for_band(int band_number) const {
    auto it = last_selected_relay_for_band_.find(band_number);
    if (it != last_selected_relay_for_band_.end()) {
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
    return std::chrono::duration_cast<std::chrono::milliseconds>(now - last_band_change_time_).count() < COOLDOWN_PERIOD_MS;
}

esp_err_t RelayController::execute_relay_change(int relay_id, int band_number) {
    esp_err_t ret = turn_off_all_relays_except(relay_id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set relay %d", relay_id);
        return ret;
    }

    if (relay_states_[relay_id]) {
        ESP_LOGI(TAG, "Relay %d successfully set", relay_id);
        last_selected_relay_for_band_[band_number] = relay_id;
        last_band_change_time_ = std::chrono::steady_clock::now();
    } else {
        ESP_LOGE(TAG, "Failed to set relay %d to true, or relay already on", relay_id);
    }

    return ESP_OK;
}
