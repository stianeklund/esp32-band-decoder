#include "relay_controller.h"
#include "esp_log.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"
#include <chrono>

static const char* TAG = "RELAY_CONTROLLER";

RelayController::RelayController()
    : currently_selected_relay_(0), 
      last_relay_change_(std::chrono::steady_clock::now()) {
}

RelayController::~RelayController() = default;

esp_err_t RelayController::init() {
    ESP_LOGI(TAG, "Initializing relay controller");
    
    esp_err_t ret = kc868_a16_hw_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize KC868-A16 hardware: %s", esp_err_to_name(ret));
        return ret;
    }

    // Turn off all relays initially
    ret = turn_off_all_relays();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize relay states: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Relay controller initialized successfully");
    return ESP_OK;
}

void RelayController::set_tcp_host(const std::string &host) {
    if (tcp_host_ != host) {
        tcp_host_ = host;
        ESP_LOGD(TAG, "TCP host set to: %s", tcp_host_.c_str());
    }
}

esp_err_t RelayController::update_relay_states() {
    ESP_LOGV(TAG, "Updating relay states");

    if (const esp_err_t ret = update_all_relay_states(); ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to update relay states: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

esp_err_t RelayController::update_tcp_settings(const std::string &host, const uint16_t port) {
    if (host != tcp_host_ || port != tcp_port_) {
        tcp_host_ = host;
        tcp_port_ = port;

        if (tcp_client->check_connection_status()) {
            tcp_client->close();
        }

        esp_err_t ret = tcp_client->ensure_connected();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Error establishing TCP connection: %s", esp_err_to_name(ret));
            return ret;
        }

        // Verify connection by getting relay states
        ret = update_all_relay_states();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to verify connection with relay state query");
        }

        ESP_LOGI(TAG, "TCP settings updated. New host: %s, new port: %d", tcp_host_.c_str(), tcp_port_);
    }

    return ESP_OK;
}


void RelayController::set_tcp_port(const uint16_t port) {
    tcp_port_ = port;
}

std::string RelayController::get_tcp_host() const {
    return tcp_host_;
}

uint16_t RelayController::get_tcp_port() const {
    return tcp_port_;
}

esp_err_t RelayController::turn_off_all_relays() {
    ESP_LOGD(TAG, "Turning off all relays");
    
    std::lock_guard<std::mutex> lock(relay_mutex_);
    
    esp_err_t ret = kc868_a16_set_all_outputs(0);
    if (ret == ESP_OK) {
        currently_selected_relay_ = 0;
        last_relay_change_ = std::chrono::steady_clock::now();
    }
    return ret;
}

esp_err_t RelayController::set_relay(const int relay_id, const bool state) {
    if (relay_id < 1 || relay_id > NUM_RELAYS) {
        ESP_LOGE(TAG, "Invalid relay ID: %d", relay_id);
        return ESP_ERR_INVALID_ARG;
    }

    std::lock_guard<std::mutex> lock(relay_mutex_);
    
    bool current_state;
    esp_err_t ret = kc868_a16_get_output_state(relay_id - 1, &current_state);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get relay state: %s", esp_err_to_name(ret));
        return ret;
    }

    if (current_state == state) {
        ESP_LOGV(TAG, "Relay %d already in desired state", relay_id);
        return ESP_OK;
    }

    if (should_delay()) {
        vTaskDelay(pdMS_TO_TICKS(COOLDOWN_PERIOD_MS));
    }

    ret = kc868_a16_set_output(relay_id - 1, state);
    if (ret == ESP_OK) {
        last_relay_change_ = std::chrono::steady_clock::now();
        if (state) {
            currently_selected_relay_ = relay_id;
        } else if (currently_selected_relay_ == relay_id) {
            currently_selected_relay_ = 0;
        }
    }
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
    return relay_states_;
}

esp_err_t RelayController::update_all_relay_states() {
    ESP_LOGD(TAG, "Getting state of all relays");

    const std::string command = "RELAY-STATE-255";
    const esp_err_t ret = send_command(command, "OK", 500);

   if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get relay states: %s", esp_err_to_name(ret));
        return ret;
    }

    if (ret == ESP_OK) {
        std::stringstream ss;
        ss << "Current relay states: ";
        for (int i = 1; i <= NUM_RELAYS; ++i) {
            ss << i << ":" << (relay_states_[i] ? "ON" : "off") << " ";
        }
        ESP_LOGD(TAG, "%s", ss.str().c_str());
    }

    return ret;
}


esp_err_t RelayController::set_relay_for_antenna(int relay_id, int band_number) {
    if (relay_id < 1 || relay_id > NUM_RELAYS) {
        ESP_LOGE(TAG, "Invalid relay ID: %d", relay_id);
        return ESP_ERR_INVALID_ARG;
    }

    RelayChangeRequest current = latest_request_.load();
    if (current.relay_id == relay_id && current.band_number == band_number) {
        ESP_LOGV(TAG, "Skipping duplicate relay change request: relay=%d, band=%d", relay_id, band_number);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Setting new relay change request: relay=%d, band=%d", relay_id, band_number);
    latest_request_.store(RelayChangeRequest{relay_id, band_number});
    return ESP_OK;
}

void RelayController::log_network_diagnostics() const {
    // Log the current TCP settings
    ESP_LOGV(TAG, "Current TCP settings - Host: %s, Port: %d", tcp_host_.c_str(), tcp_port_);

    // Add network diagnostics
    char ip_info[64];
    esp_netif_ip_info_t ip_info_struct;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != nullptr && esp_netif_get_ip_info(netif, &ip_info_struct) == ESP_OK) {
        snprintf(ip_info, sizeof(ip_info), "IP: %ld.%ld.%ld.%ld, GW: %ld.%ld.%ld.%ld",
                 ip_info_struct.ip.addr & 0xff, (ip_info_struct.ip.addr >> 8) & 0xff,
                 (ip_info_struct.ip.addr >> 16) & 0xff, (ip_info_struct.ip.addr >> 24) & 0xff,
                 ip_info_struct.gw.addr & 0xff, (ip_info_struct.gw.addr >> 8) & 0xff,
                 (ip_info_struct.gw.addr >> 16) & 0xff, (ip_info_struct.gw.addr >> 24) & 0xff);
        ESP_LOGV(TAG, "Network info: %s", ip_info);
    } else {
        ESP_LOGW(TAG, "Failed to get network info");
    }
}

esp_err_t RelayController::turn_off_all_relays_except(const int relay_to_keep_on) {
    if (relay_to_keep_on < 1 || relay_to_keep_on > NUM_RELAYS) {
        ESP_LOGE(TAG, "Invalid relay ID: %d", relay_to_keep_on);
        return ESP_ERR_INVALID_ARG;
    }

    std::lock_guard<std::mutex> lock(relay_mutex_);

    if (should_delay()) {
        vTaskDelay(pdMS_TO_TICKS(COOLDOWN_PERIOD_MS));
    }

    const uint16_t relay_mask = 1 << (relay_to_keep_on - 1);
    esp_err_t ret = kc868_a16_set_all_outputs(relay_mask);
    
    if (ret == ESP_OK) {
        currently_selected_relay_ = relay_to_keep_on;
        last_relay_change_ = std::chrono::steady_clock::now();
        ESP_LOGI(TAG, "All relays turned off except relay %d", relay_to_keep_on);
    }
    
    return ret;
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
    return std::chrono::duration_cast<std::chrono::milliseconds>(now - last_relay_change_).count() < COOLDOWN_PERIOD_MS;
}

esp_err_t RelayController::execute_relay_change(int relay_id, int band_number) {
    std::lock_guard<std::mutex> lock(relay_mutex_);
    
    // If we're already on the correct relay, no need to change
    if (relay_id == currently_selected_relay_) {
        ESP_LOGD(TAG, "Relay %d already selected", relay_id);
        last_selected_relay_for_band_[band_number] = relay_id;
        return ESP_OK;
    }

    esp_err_t ret = turn_off_all_relays_except(relay_id);
    if (ret == ESP_OK) {
        last_selected_relay_for_band_[band_number] = relay_id;
        ESP_LOGI(TAG, "Successfully changed to relay %d for band %d", relay_id, band_number);
    }

    return ret;
}

void RelayController::tcp_task(void *pvParameters) {
    auto *controller = static_cast<RelayController *>(pvParameters);
    RelayChangeRequest last_processed{0, -1};
    
    const TickType_t TASK_DELAY = pdMS_TO_TICKS(20);
    const TickType_t CONNECTION_CHECK_INTERVAL = pdMS_TO_TICKS(5000);
    
    TickType_t last_connection_check = xTaskGetTickCount();

    while (true) {
        TickType_t current_tick = xTaskGetTickCount();
        
        // Connection check with proper yielding
        if ((current_tick - last_connection_check) >= CONNECTION_CHECK_INTERVAL) {
            esp_err_t conn_status = controller->tcp_client->ensure_connected();
            if (conn_status != ESP_OK) {
                ESP_LOGW(TAG, "Connection check failed: %s", esp_err_to_name(conn_status));
                controller->tcp_client->close();
                vTaskDelay(pdMS_TO_TICKS(1000));  // Yield during reconnection
                
                conn_status = controller->tcp_client->ensure_connected();
                if (conn_status != ESP_OK) {
                    ESP_LOGE(TAG, "Connection recovery failed: %s", esp_err_to_name(conn_status));
                }
            }
            last_connection_check = current_tick;
        }

        // Process relay change requests with proper yielding
        RelayChangeRequest current = controller->latest_request_.load();
        if (current != last_processed) {
            if (current.relay_id > 0) {
                esp_err_t ret = controller->execute_relay_change(current.relay_id, current.band_number);
                if (ret == ESP_OK) {
                    last_processed = current;
                } else {
                    ESP_LOGE(TAG, "Relay change failed: %s", esp_err_to_name(ret));
                    if (ret == ESP_ERR_TIMEOUT || ret == ESP_ERR_INVALID_STATE) {
                        ESP_LOGW(TAG, "Connection issue detected, forcing reconnection");
                        controller->tcp_client->close();
                        vTaskDelay(pdMS_TO_TICKS(1000));  // Yield during reconnection
                        
                        esp_err_t conn_status = controller->tcp_client->ensure_connected();
                        if (conn_status != ESP_OK) {
                            ESP_LOGE(TAG, "Reconnection failed: %s", esp_err_to_name(conn_status));
                        }
                    }
                }
            }
        }

        // Regular yield at end of loop
        vTaskDelay(TASK_DELAY);
    }
}

esp_err_t RelayController::parse_relay_state_response(const std::string &response) {
    // Find the last occurrence of RELAY-STATE-255 or RELAY-SET_ALL-255
    size_t last_state_pos = response.rfind("RELAY-");
    if (last_state_pos == std::string::npos) {
        ESP_LOGW(TAG, "No valid relay command found in response");
        return ESP_FAIL;
    }

    // Extract just the last response
    std::string last_response = response.substr(last_state_pos);
    const char *ptr = last_response.c_str();
    const char *const end_ptr = ptr + last_response.length();

    // Quick check for minimum valid length
    if ((end_ptr - ptr) < 18) {
        // Minimum "RELAY-STATE-255,0,0,OK"
        ESP_LOGW(TAG, "Response too short");
        return ESP_FAIL;
    }

    // Fast header check - only check critical characters
    if (ptr[0] != 'R' || ptr[5] != '-' ||
        ptr[6] != 'S') {
        // Matches both STATE and SET_ALL
        ESP_LOGW(TAG, "Invalid header format");
        return ESP_FAIL;
    }

    // Find the first comma after RELAY-STATE-255 or RELAY-SET_ALL-255
    // Skip past any line feeds or other characters
    ptr = strstr(ptr, "-255");
    if (!ptr || (end_ptr - ptr) < 4) {
        ESP_LOGW(TAG, "Missing -255 in response");
        return ESP_FAIL;
    }
    ptr += 4; // Skip "-255"

    // Skip any characters (including line feeds) until we find the first comma
    while (ptr < end_ptr && *ptr != ',') ptr++;
    if (!ptr || (end_ptr - ptr) < 4) {
        // Need at least ",0,0"
        ESP_LOGW(TAG, "Invalid format after header");
        return ESP_FAIL;
    }
    ptr++; // Skip the comma

    // Parse both numbers in one pass
    char *next;

    // Parse d1
    long val = strtol(ptr, &next, 10);
    if (next == ptr || val < 0 || val > 255 || *next != ',') {
        ESP_LOGW(TAG, "Invalid d1 value");
        return ESP_FAIL;
    }

    const uint8_t d1 = static_cast<uint8_t>(val);

    // Parse d0
    ptr = next + 1;
    val = strtol(ptr, &next, 10);
    if (next == ptr || val < 0 || val > 255) {
        ESP_LOGW(TAG, "Invalid d0 value");
        return ESP_FAIL;
    }

    const uint8_t d0 = static_cast<uint8_t>(val);

    // Quick check for OK (just verify 'O' and 'K' are present)
    ptr = next;
    while (ptr < end_ptr && (*ptr == ',' || *ptr <= ' ')) ptr++;
    if (ptr >= end_ptr - 1 || ptr[0] != 'O' || ptr[1] != 'K') {
        ESP_LOGW(TAG, "Missing OK confirmation");
        return ESP_FAIL;
    }

    // Update state with single bitfield operation
    relay_state_bitfield_ = (d1 << 8) | d0;

    // Fast currently selected relay calculation using hardware instructions
    currently_selected_relay_ = relay_state_bitfield_ ? __builtin_ffs(relay_state_bitfield_) : 0;

    // Update relay states with minimal operations
    uint16_t mask = 1;
    for (int i = 1; i <= NUM_RELAYS; ++i, mask <<= 1) {
        relay_states_[i] = (relay_state_bitfield_ & mask) != 0;
    }

    ESP_LOGV(TAG, "Parsed relay states: D1=%d, D0=%d, selected=%d",
             d1, d0, currently_selected_relay_);
    return ESP_OK;
}


esp_err_t RelayController::verify_relay_state(int expected_relay) {
    // Add small delay to allow relay to settle
    vTaskDelay(pdMS_TO_TICKS(20));
    const int VERIFY_ATTEMPTS = 2;
    const int VERIFY_TIMEOUT = 250;

    for (int i = 0; i < VERIFY_ATTEMPTS; i++) {
        // Flush any pending responses before sending new query
        char flush_buffer[128];
        while (tcp_client->receive_message(flush_buffer, sizeof(flush_buffer), 300) == ESP_OK) {
            ESP_LOGV(TAG, "Flushed pending response: %s", flush_buffer);
        }

        esp_err_t ret = send_command("RELAY-STATE-255", "OK", VERIFY_TIMEOUT);
        if (ret == ESP_OK) {
            if (currently_selected_relay_ == expected_relay) {
                ESP_LOGD(TAG, "Verified relay state: %d", currently_selected_relay_);
                return ESP_OK;
            }
            ESP_LOGW(TAG, "Unexpected relay state: got %d, expected %d",
                     currently_selected_relay_, expected_relay);
        }

        if (i < VERIFY_ATTEMPTS - 1) {
            vTaskDelay(pdMS_TO_TICKS(50));  // Wait between attempts
        }
    }
    return ESP_FAIL;
}

esp_err_t RelayController::send_command(const std::string &command,
                                      const std::string &expected_response,
                                      int timeout_ms,
                                      int max_retries) {
    static constexpr size_t RESPONSE_BUFFER_SIZE = 256;
    static char response[RESPONSE_BUFFER_SIZE];
    static constexpr char NEWLINE = '\n';
    static constexpr auto SET_ALL_CMD = "RELAY-SET_ALL";
    static constexpr auto STATE_CMD = "RELAY-STATE-255";
    static constexpr auto OK_RESPONSE = ",OK";
    
    // Optimize timeout for SET_ALL commands
    if (strstr(command.c_str(), SET_ALL_CMD) != nullptr) {
        timeout_ms = std::max(timeout_ms, 1000);
    }
    
    const TickType_t start_time = xTaskGetTickCount();
    std::lock_guard lock(command_mutex_);
    
    ESP_LOGD(TAG, "Starting command: %s", command.c_str());
    last_command_ = command;  // Consider if this is really needed

    // Check connection once before sending
    esp_err_t status = tcp_client->ensure_connected();
    if (status != ESP_OK) {
        ESP_LOGW(TAG, "Connection check failed: %s", esp_err_to_name(status));
        return status;
    }

    // Prepare command buffer with newline
    char cmd_buffer[128];
    const size_t cmd_len = std::min(command.length(), sizeof(cmd_buffer) - 2);
    memcpy(cmd_buffer, command.c_str(), cmd_len);
    cmd_buffer[cmd_len] = NEWLINE;
    cmd_buffer[cmd_len + 1] = '\0';

    // Send command
    status = tcp_client->send_message(cmd_buffer);
    if (status != ESP_OK) {
        ESP_LOGW(TAG, "Send failed: %s", esp_err_to_name(status));
        return status;
    }

    // Receive response
    status = tcp_client->receive_message(response, RESPONSE_BUFFER_SIZE, timeout_ms);
    const TickType_t end_time = xTaskGetTickCount();
    
    if (status != ESP_OK) {
        ESP_LOGW(TAG, "Receive failed: %s", esp_err_to_name(status));
        return status;
    }

    ESP_LOGD(TAG, "Command completed in %lu ms", pdTICKS_TO_MS(end_time - start_time));
    ESP_LOGV(TAG, "Raw response: '%s'", response);

    // Process response based on command type
    if (const char *cmd_str = command.c_str(); strstr(cmd_str, SET_ALL_CMD) != nullptr) {
        // For SET_ALL, we just need OK in the response
        if (strstr(response, OK_RESPONSE) != nullptr) {
            ESP_LOGD(TAG, "SET_ALL command confirmed");
            // Try to parse state but don't fail if we can't
            parse_relay_state_response(response);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "SET_ALL command did not receive OK confirmation");
        return ESP_FAIL;
    }
    else if (strstr(cmd_str, STATE_CMD) != nullptr) {
        // For state queries, just parse the state
        return parse_relay_state_response(response);
    }
    else if (!expected_response.empty()) {
        // For other commands, check for expected response
        return (strstr(response, expected_response.c_str()) != nullptr) ? ESP_OK : ESP_FAIL;
    }

    ESP_LOGW(TAG, "Unexpected response");
    return ESP_FAIL;
}
