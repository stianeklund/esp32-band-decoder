#include "relay_controller.h"
#include "esp_log.h"
#include "freertos/projdefs.h"
#include <algorithm>
#include <sstream>
#include <freertos/mpu_wrappers.h>
#include "freertos/task.h"

static const char *TAG = "RELAY_CONTROLLER";
const int maxRetries = 2;
const int retryDelay = 50;  // milliseconds


RelayController::RelayController()
    : currently_selected_relay_(0), last_band_change_time_(std::chrono::steady_clock::now()),
      udp_host_(""), udp_port_(8888) {
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

    // Set default states
    for (int i = 1; i <= NUM_RELAYS; ++i) {
        relay_states_[i] = false;
    }
    currently_selected_relay_ = 0;

    ESP_LOGI(TAG, "Relay controller initialized with UDP host: %s, port: %d", udp_host_.c_str(), udp_port_);
    return ESP_OK;
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

void RelayController::set_udp_host(const std::string& host) {
    udp_host_ = host;
}

void RelayController::set_udp_port(uint16_t port) {
    udp_port_ = port;
}

void RelayController::set_udp_host(const std::string& host) {
    udp_host_ = host;
    ESP_LOGI(TAG, "UDP host set to: %s", udp_host_.c_str());
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
    std::string expected_response = "RELAY-AOF-255,1,1,OK";
    
    esp_err_t ret = send_command_and_validate_response(command, expected_response);
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
    std::string expected_response = command + ",OK";

    esp_err_t ret = send_command_and_validate_response(command, expected_response);
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
    const int max_retries = 3;

    while (retry_count < max_retries) {
        // Re-initialize UDP client with current settings before sending
        ret = udp_client->init(udp_host_.c_str(), udp_port_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to re-initialize UDP client: %s", esp_err_to_name(ret));
            return ret;
        }

        ret = udp_client->send_message(command);
        ESP_LOGI(TAG, "Sending %s to: %s : %d", command.c_str(), udp_host_.c_str(), udp_port_);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to send get all relay states message: %s. Retry %d/%d", esp_err_to_name(ret), retry_count + 1, max_retries);
            retry_count++;
            vTaskDelay(pdMS_TO_TICKS(100)); // Wait 100ms before retrying
            continue;
        }

        ret = udp_client->receive_message(response, 2000); // 2 second timeout
        if (ret == ESP_OK) {
            break; // Successful response, exit the retry loop
        } else if (ret == ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "Timeout receiving response for get all relay states. Retry %d/%d", retry_count + 1, max_retries);
            retry_count++;
            vTaskDelay(pdMS_TO_TICKS(100)); // Wait 100ms before retrying
        } else {
            ESP_LOGE(TAG, "Failed to receive response for get all relay states: %s", esp_err_to_name(ret));
            return ret;
        }
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
    // The response format is expected to be like: "RELAY-STATE-255,10101010,OK"
    std::string state_string = response.substr(expected_response_prefix.length(), RelayController::NUM_RELAYS);
    if (state_string.length() != RelayController::NUM_RELAYS) {
        ESP_LOGE(TAG, "Invalid state string length: %zu", state_string.length());
        return ESP_FAIL;
    }

    for (int i = 0; i < RelayController::NUM_RELAYS; ++i) {
        relay_states_[i + 1] = (state_string[i] == '1');
        if (state_string[i] == '1') {
            currently_selected_relay_ = i + 1;
        }
    }

    return ESP_OK;
}

esp_err_t RelayController::send_command_and_validate_response(const std::string& command, const std::string& expected_response) {
    // Re-initialize UDP client with current settings before sending
    esp_err_t ret = udp_client->init(udp_host_.c_str(), udp_port_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to re-initialize UDP client: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = udp_client->send_message(command);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send command: %s", esp_err_to_name(ret));
        return ret;
    }

    std::string response;
    ret = udp_client->receive_message(response, 2000); // 2 second timeout
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to receive response: %s", esp_err_to_name(ret));
        return ret;
    }

    if (response != expected_response) {
        ESP_LOGE(TAG, "Unexpected response. Expected: %s, Received: %s", expected_response.c_str(), response.c_str());
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Command sent successfully: %s", command.c_str());
    ESP_LOGI(TAG, "Response received: %s", response.c_str());

    return ESP_OK;
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

    std::stringstream ss;
    ss << "ALL_OFF_EXCEPT " << relay_to_keep_on;
    std::string message = ss.str();
    ESP_LOGI(TAG, "Turn off all message: %s", message.c_str());

    esp_err_t ret = udp_client->send_message(message);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send turn off all relays except message: %s", esp_err_to_name(ret));
        return ret;
    }

    for (int i = 1; i <= NUM_RELAYS; ++i) {
        relay_states_[i] = (i == relay_to_keep_on);
    }
    currently_selected_relay_ = relay_to_keep_on;

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
