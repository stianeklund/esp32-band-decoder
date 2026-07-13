#include "websocket_handlers.h"
#include "websocket_server.h"
#include "antenna_switch.h"
#include "relay_controller.h"
#include "cat_parser.h"
#include "my_mqtt_client.h"
#include "html_content.h"
#include "esp_log.h"

const char* WebSocketHandlers::TAG = "WEBSOCKET_HANDLERS";

esp_err_t WebSocketHandlers::handle_status_request(int sockfd, const char* request_id) {
    ESP_LOGI(TAG, "Handling STATUS request from fd=%d, request_id=%s", sockfd, request_id ? request_id : "none");
    
    cJSON *data = create_status_json();
    if (!data) {
        ESP_LOGE(TAG, "Failed to create status JSON data");
        return send_error_response(sockfd, request_id, "Failed to create status data");
    }
    
    ESP_LOGI(TAG, "Status data created successfully, sending response...");
    esp_err_t ret = send_json_response(sockfd, request_id, data);
    cJSON_Delete(data);
    return ret;
}

esp_err_t WebSocketHandlers::handle_relay_control_request(int sockfd, const char* request_id, const cJSON* data) {
    ESP_LOGD(TAG, "Handling relay control request from fd=%d", sockfd);
    
    if (!data) {
        return send_error_response(sockfd, request_id, "Missing data for relay control");
    }
    
    int relay_id;
    bool state;
    esp_err_t ret = validate_relay_control_data(data, &relay_id, &state);
    if (ret != ESP_OK) {
        return send_error_response(sockfd, request_id, "Invalid relay control data");
    }
    
    ESP_LOGD(TAG, "Setting relay %d to %s", relay_id, state ? "ON" : "OFF");
    ret = AntennaSwitch::instance().set_relay(relay_id, state);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set relay: %s", esp_err_to_name(ret));
        return send_error_response(sockfd, request_id, "Failed to set relay");
    }
    
    // Small delay to allow hardware to update
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // Get actual state after setting
    bool current_state;
    ret = AntennaSwitch::instance().get_relay_state(relay_id, &current_state);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get relay state after setting: %s", esp_err_to_name(ret));
        current_state = state; // Use requested state as fallback
    }
    
    // Create response with actual state
    cJSON *response_data = cJSON_CreateObject();
    if (!response_data) {
        return send_error_response(sockfd, request_id, "Failed to create response");
    }
    
    cJSON_AddBoolToObject(response_data, "state", current_state);
    ret = send_json_response(sockfd, request_id, response_data);
    
    cJSON_Delete(response_data);
    
    // Broadcast relay state change event
    WebSocketServer::instance().broadcast_relay_state_change(relay_id, current_state);
    
    return ret;
}

esp_err_t WebSocketHandlers::handle_antenna_switch_request(int sockfd, const char* request_id, const cJSON* data) {
    ESP_LOGD(TAG, "Handling antenna switch request from fd=%d", sockfd);
    
    if (!data) {
        return send_error_response(sockfd, request_id, "Missing data for antenna switch");
    }
    
    char radio[8] = {0};
    char action[16] = {0};
    int antenna_number = 0;
    esp_err_t ret = validate_antenna_switch_data(data, radio, action, &antenna_number);
    if (ret != ESP_OK) {
        return send_error_response(sockfd, request_id, "Invalid antenna switch data");
    }
    
    // Validate radio parameter - optimize with character check first
    RadioID radio_id;
    if (radio[0] == 'A' && radio[1] == '\0') {
        radio_id = RadioID::A;
    } else if (radio[0] == 'B' && radio[1] == '\0') {
        radio_id = RadioID::B;
    } else {
        return send_error_response(sockfd, request_id, "Invalid radio (must be 'A' or 'B')");
    }
    
    // Check if this is a direct antenna selection or next/previous action
    bool is_direct_selection = (antenna_number > 0);
    bool next_antenna = false;
    
    if (!is_direct_selection) {
        // Validate action parameter for next/previous - optimize string comparison
        if (action[0] == 'n' && strcmp(action, "next") == 0) {
            next_antenna = true;
        } else if (action[0] == 'p' && strcmp(action, "previous") == 0) {
            next_antenna = false;
        } else {
            return send_error_response(sockfd, request_id, "Invalid action (must be 'next', 'previous', or specify 'antenna' number)");
        }
    }
    
    // Use cached configuration for better performance
    const auto& config = AntennaSwitch::instance().get_config_ref();
    
    // Get current frequency based on radio
    uint32_t current_freq = 0;
    if (radio_id == RadioID::A) {
        current_freq = CatParser::instance().get_frequency();
    }
    // TODO: Add Radio B frequency support when implemented
    
    if (current_freq == 0) {
        ESP_LOGW(TAG, "No frequency available for radio %s", radio);
        return send_error_response(sockfd, request_id, "No frequency available for specified radio");
    }
    
    // Find available antennas for current frequency
    std::vector<int> available_antennas;
    available_antennas.reserve(MAX_ANTENNA_PORTS);
    const char* band_name = "Unknown";
    
    const int radio_idx = static_cast<int>(radio_id);
    const int port_offset = (radio_id == RadioID::A) ? 0 : RelayController::RELAYS_PER_RADIO;
    
    for (int band_idx = 0; band_idx < config.num_bands; band_idx++) {
        const auto &band_cfg = config.bands[radio_idx][band_idx];
        if (current_freq >= band_cfg.start_freq && current_freq <= band_cfg.end_freq) {
            band_name = band_cfg.description;
            
            // Collect available ports
            for (int port_idx = 0; port_idx < RelayController::RELAYS_PER_RADIO && port_idx < MAX_ANTENNA_PORTS; port_idx++) {
                if (band_cfg.antenna_ports[port_idx]) {
                    available_antennas.emplace_back(port_offset + port_idx + 1);
                }
            }
            break;
        }
    }
    
    if (available_antennas.empty()) {
        ESP_LOGW(TAG, "No antennas available for frequency %lu Hz", current_freq);
        return send_error_response(sockfd, request_id, "No antennas available for current frequency");
    }
    
    // Determine target antenna
    int new_antenna;
    int current_active = AntennaSwitch::instance().get_active_relay_for_radio(radio_id);
    
    if (is_direct_selection) {
        // Direct antenna selection - validate antenna is available
        new_antenna = antenna_number;
        
        // Check if requested antenna is in the available list
        bool antenna_available = false;
        for (int available_ant : available_antennas) {
            if (available_ant == new_antenna) {
                antenna_available = true;
                break;
            }
        }
        
        if (!antenna_available) {
            ESP_LOGW(TAG, "Requested antenna %d not available for current frequency %lu Hz", new_antenna, current_freq);
            return send_error_response(sockfd, request_id, "Requested antenna not available for current frequency");
        }
    } else {
        // Next/previous selection - find current index and calculate next
        int current_index = -1;
        
        for (size_t i = 0; i < available_antennas.size(); i++) {
            if (available_antennas[i] == current_active) {
                current_index = static_cast<int>(i);
                break;
            }
        }
        
        // Calculate next antenna index with wraparound
        int new_index;
        if (current_index == -1) {
            new_index = 0; // Use first available if current not found
        } else {
            if (next_antenna) {
                new_index = (current_index + 1) % static_cast<int>(available_antennas.size());
            } else {
                new_index = (current_index - 1 + static_cast<int>(available_antennas.size())) % static_cast<int>(available_antennas.size());
            }
        }
        
        new_antenna = available_antennas[new_index];
    }
    
    // Switch to the new antenna
    ret = AntennaSwitch::instance().set_relay(new_antenna, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to switch to antenna %d: %s", new_antenna, esp_err_to_name(ret));
        return send_error_response(sockfd, request_id, "Failed to switch antenna");
    }
    
    // Create response data
    cJSON *response_data = cJSON_CreateObject();
    if (!response_data) {
        return send_error_response(sockfd, request_id, "Failed to create response");
    }
    
    cJSON_AddStringToObject(response_data, "status", "success");
    cJSON_AddStringToObject(response_data, "radio", radio);
    cJSON_AddNumberToObject(response_data, "frequency", current_freq);
    cJSON_AddNumberToObject(response_data, "frequency_mhz", current_freq / 1000000.0);
    cJSON_AddStringToObject(response_data, "band", band_name);
    cJSON_AddNumberToObject(response_data, "previous_antenna", current_active);
    cJSON_AddNumberToObject(response_data, "new_antenna", new_antenna);
    
    cJSON *antennas_array = cJSON_CreateArray();
    for (int antenna : available_antennas) {
        cJSON_AddItemToArray(antennas_array, cJSON_CreateNumber(antenna));
    }
    cJSON_AddItemToObject(response_data, "available_antennas", antennas_array);
    
    ret = send_json_response(sockfd, request_id, response_data);
    cJSON_Delete(response_data);
    
    ESP_LOGI(TAG, "Antenna switched: Radio %s from %d to %d", radio, current_active, new_antenna);
    
    return ret;
}

esp_err_t WebSocketHandlers::handle_config_get_request(int sockfd, const char* request_id) {
    ESP_LOGD(TAG, "Handling config get request from fd=%d", sockfd);
    
    // For WebSocket, we'll return basic config info to avoid large payloads
    // Full config can be retrieved via HTTP if needed
    cJSON *data = create_config_basic_json();
    if (!data) {
        return send_error_response(sockfd, request_id, "Failed to create config data");
    }
    
    esp_err_t ret = send_json_response(sockfd, request_id, data);
    cJSON_Delete(data);
    return ret;
}

esp_err_t WebSocketHandlers::handle_config_set_request(int sockfd, const char* request_id, const cJSON* data) {
    ESP_LOGD(TAG, "Handling config set request from fd=%d", sockfd);
    
    // For security and complexity reasons, full config updates should be done via HTTP
    // WebSocket can handle basic settings only
    return send_error_response(sockfd, request_id, "Config updates not supported via WebSocket. Use HTTP API.");
}

esp_err_t WebSocketHandlers::handle_relay_names_request(int sockfd, const char* request_id) {
    ESP_LOGI(TAG, "Handling RELAY_NAMES request from fd=%d, request_id=%s", sockfd, request_id ? request_id : "none");
    
    cJSON *data = create_relay_names_json();
    if (!data) {
        ESP_LOGE(TAG, "Failed to create relay names JSON data");
        return send_error_response(sockfd, request_id, "Failed to create relay names data");
    }
    
    ESP_LOGI(TAG, "Relay names data created successfully, sending response...");
    esp_err_t ret = send_json_response(sockfd, request_id, data);
    cJSON_Delete(data);
    return ret;
}

esp_err_t WebSocketHandlers::handle_config_basic_request(int sockfd, const char* request_id) {
    ESP_LOGD(TAG, "Handling basic config request from fd=%d", sockfd);
    
    cJSON *data = create_config_basic_json();
    if (!data) {
        return send_error_response(sockfd, request_id, "Failed to create basic config data");
    }
    
    esp_err_t ret = send_json_response(sockfd, request_id, data);
    cJSON_Delete(data);
    return ret;
}

esp_err_t WebSocketHandlers::handle_subscribe_request(int sockfd, const char* request_id, const cJSON* data) {
    ESP_LOGI(TAG, "Handling SUBSCRIBE request from fd=%d, request_id=%s", sockfd, request_id ? request_id : "none");
    
    // For now, return success - subscription management can be enhanced later
    // All clients are subscribed to all events by default
    ESP_LOGI(TAG, "Sending subscribe success response...");
    return send_success_response(sockfd, request_id, "Subscribed to events");
}

esp_err_t WebSocketHandlers::handle_unsubscribe_request(int sockfd, const char* request_id, const cJSON* data) {
    ESP_LOGD(TAG, "Handling unsubscribe request from fd=%d", sockfd);
    
    // For now, return success - subscription management can be enhanced later
    return send_success_response(sockfd, request_id, "Unsubscribed from events");
}

esp_err_t WebSocketHandlers::send_json_response(int sockfd, const char* request_id, const cJSON* data) {
    ESP_LOGV(TAG, "Sending JSON response to fd=%d, request_id=%s", sockfd, request_id ? request_id : "none");

    if (!data) {
        ESP_LOGE(TAG, "Cannot send response: data is null");
        return ESP_ERR_INVALID_ARG;
    }

    // Acquire WebSocketServer's response buffer mutex (friend class has access)
    auto& ws_server = WebSocketServer::instance();
    if (xSemaphoreTakeRecursive(ws_server.m_response_buffer_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire response buffer mutex for JSON response");
        return ESP_ERR_TIMEOUT;
    }

    // Use static buffer for JSON serialization - saves malloc/free overhead
    static char json_response_buffer[WS_MAX_MESSAGE_SIZE];  // Protected by ws_server.m_response_buffer_mutex

    // Use cJSON_PrintPreallocated for zero-allocation serialization
    if (!cJSON_PrintPreallocated(const_cast<cJSON*>(data), json_response_buffer, sizeof(json_response_buffer), false)) {
        ESP_LOGE(TAG, "JSON response too large for buffer (max: %zu bytes)", sizeof(json_response_buffer));
        xSemaphoreGiveRecursive(ws_server.m_response_buffer_mutex);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGV(TAG, "Sending WebSocket response: %.*s", 200, json_response_buffer); // Truncate long responses
    esp_err_t ret = ws_server.send_response(sockfd, request_id, WS_MSG_RESPONSE, json_response_buffer);
    xSemaphoreGiveRecursive(ws_server.m_response_buffer_mutex);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send WebSocket response: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGV(TAG, "WebSocket response sent successfully");
    }

    return ret;
}

esp_err_t WebSocketHandlers::send_success_response(int sockfd, const char* request_id, const char* message) {
    // Stack-allocated buffer for thread safety (small enough for stack)
    char success_buffer[512];

    int len;
    if (message) {
        // Success response with message
        len = snprintf(success_buffer, sizeof(success_buffer),
                      "{\"status\":\"success\",\"message\":\"%s\"}",
                      message);
    } else {
        // Success response without message
        len = snprintf(success_buffer, sizeof(success_buffer),
                      "{\"status\":\"success\"}");
    }

    if (len >= sizeof(success_buffer)) {
        ESP_LOGE(TAG, "Success message too large for buffer (%d >= %zu)", len, sizeof(success_buffer));
        return ESP_ERR_NO_MEM;
    }
    
    return WebSocketServer::instance().send_response(sockfd, request_id, WS_MSG_RESPONSE, success_buffer);
}

esp_err_t WebSocketHandlers::send_error_response(int sockfd, const char* request_id, const char* error_msg) {
    // Use pre-allocated template for error response - 80-90% CPU saving vs cJSON
    static char error_buffer[512];
    
    const char* safe_error_msg = error_msg ? error_msg : "Unknown error";
    
    // Efficient sprintf template instead of cJSON operations
    int len = snprintf(error_buffer, sizeof(error_buffer),
                      "{\"status\":\"error\",\"message\":\"%s\"}",
                      safe_error_msg);
    
    if (len >= sizeof(error_buffer)) {
        ESP_LOGE(TAG, "Error message too large for buffer (%d >= %zu)", len, sizeof(error_buffer));
        return ESP_ERR_NO_MEM;
    }
    
    return WebSocketServer::instance().send_response(sockfd, request_id, WS_MSG_ERROR, error_buffer);
}

cJSON* WebSocketHandlers::create_status_json() {
    // Get current frequency from CAT parser. current_freq is the true IF frequency
    // (used for band/antenna lookups); display_freq is corrected by the transverter
    // offset when transverter mode is active (what the user should see).
    const uint32_t current_freq = CatParser::instance().get_frequency();
    const bool transverter_active = CatParser::instance().is_transverter_active();
    const uint32_t display_freq = CatParser::instance().get_display_frequency();
    const bool is_transmitting = CatParser::instance().is_transmitting();
    
    // Use cached configuration for better performance
    const auto& config = AntennaSwitch::instance().get_config_ref();
    
    // Determine active antenna for Radio A
    uint16_t relay_states = RelayController::instance().get_relay_states();
    int active_antenna_a_num = 0;
    
    for (int i = 0; i < RelayController::RELAYS_PER_RADIO; i++) {
        if (!((relay_states >> i) & 1)) { // Active low
            active_antenna_a_num = i + 1;
            break;
        }
    }
    
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return nullptr;
    }
    
    cJSON_AddNumberToObject(root, "frequency", display_freq);
    cJSON_AddNumberToObject(root, "frequency_mhz", display_freq / 1000000.0);
    cJSON_AddBoolToObject(root, "transverter_active", transverter_active);
    cJSON_AddNumberToObject(root, "transverter_offset_hz",
                            CatParser::instance().get_transverter_offset_hz());
    char antenna_name_buffer[32];
    if (active_antenna_a_num) {
        snprintf(antenna_name_buffer, sizeof(antenna_name_buffer), "Antenna %d", active_antenna_a_num);
        cJSON_AddStringToObject(root, "antenna", antenna_name_buffer);
    } else {
        cJSON_AddStringToObject(root, "antenna", "None");
    }
    cJSON_AddBoolToObject(root, "transmitting", is_transmitting);
    
    // Determine data source for Radio A - optimize with early evaluation
    const char* data_source_a = "None";
    if (MQTTClient::instance().has_serial_data()) {
        data_source_a = "Serial";
    } else if (config.mqtt_enabled && MQTTClient::instance().get_current_frequency() > 0) {
        data_source_a = "MQTT";
    }
    cJSON_AddStringToObject(root, "data_source", data_source_a);
    
    // Find available antennas for current frequency - reserve space to avoid reallocations
    std::vector<int> available_antennas_a;
    available_antennas_a.reserve(RelayController::RELAYS_PER_RADIO); // Max 8 antennas per radio
    // While transverter mode is active the antenna switch is bypassed (no port
    // supports the transverter band), so report no available antennas.
    if (!transverter_active && current_freq > 0) {
        for (int band_idx = 0; band_idx < config.num_bands; band_idx++) {
            const auto &band_cfg = config.bands[0][band_idx]; // Radio A
            if (current_freq >= band_cfg.start_freq && current_freq <= band_cfg.end_freq) {
                for (int port_idx = 0; port_idx < RelayController::RELAYS_PER_RADIO; port_idx++) {
                    if (port_idx < MAX_ANTENNA_PORTS && band_cfg.antenna_ports[port_idx]) {
                        available_antennas_a.push_back(port_idx + 1);
                    }
                }
                break;
            }
        }
    }
    
    cJSON *antennas_a_json = cJSON_CreateArray();
    for (int antenna : available_antennas_a) {
        cJSON_AddItemToArray(antennas_a_json, cJSON_CreateNumber(antenna));
    }
    cJSON_AddItemToObject(root, "available_antennas", antennas_a_json);
    
    // Add Radio B status if applicable
    if (config.radio_operation_mode != RADIO_OP_MODE_SINGLE_A) {
        int active_antenna_b_num = 0;
        for (int i = RelayController::RELAYS_PER_RADIO; i < RelayController::NUM_RELAYS; i++) {
            if (!((relay_states >> i) & 1)) { // Active low
                active_antenna_b_num = i + 1;
                break;
            }
        }
        char antenna_b_name_buffer[32];
        if (active_antenna_b_num) {
            snprintf(antenna_b_name_buffer, sizeof(antenna_b_name_buffer), "Antenna %d", active_antenna_b_num);
            cJSON_AddStringToObject(root, "antenna_b", antenna_b_name_buffer);
        } else {
            cJSON_AddStringToObject(root, "antenna_b", "None");
        }
        
        cJSON_AddNumberToObject(root, "frequency_b", 0); // Placeholder
        cJSON_AddBoolToObject(root, "transmitting_b", false); // Placeholder
        cJSON_AddStringToObject(root, "data_source_b", data_source_a);
        
        cJSON *antennas_b_json = cJSON_CreateArray();
        cJSON_AddItemToObject(root, "available_antennas_b", antennas_b_json);
    }
    
    return root;
}

cJSON* WebSocketHandlers::create_relay_names_json() {
    const auto& config = AntennaSwitch::instance().get_config_ref();
    
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return nullptr;
    }
    
    for (int i = 0; i < 16; i++) {
        char relay_key[4];
        snprintf(relay_key, sizeof(relay_key), "%d", i + 1);
        
        const char* relay_name;
        if (config.relay_names[i][0] != '\0') {
            relay_name = config.relay_names[i];
        } else {
            static char default_name[16];
            snprintf(default_name, sizeof(default_name), "Relay %d", i + 1);
            relay_name = default_name;
        }
        
        cJSON_AddStringToObject(root, relay_key, relay_name);
    }
    
    return root;
}

cJSON* WebSocketHandlers::create_config_basic_json() {
    const auto& config = AntennaSwitch::instance().get_config_ref();
    
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return nullptr;
    }
    
    cJSON_AddBoolToObject(root, "auto_mode", config.auto_mode);
    cJSON_AddNumberToObject(root, "num_bands", config.num_bands);
    cJSON_AddNumberToObject(root, "num_antenna_ports", config.num_antenna_ports);
    cJSON_AddBoolToObject(root, "mqtt_enabled", config.mqtt_enabled);
    
    // Add radio operation mode
    const char* radio_mode_str;
    switch (config.radio_operation_mode) {
        case RADIO_OP_MODE_SINGLE_A:
            radio_mode_str = "SINGLE_A";
            break;
        case RADIO_OP_MODE_ALTERNATING_AB:
            radio_mode_str = "ALTERNATING_AB";
            break;
        case RADIO_OP_MODE_CONCURRENT_AB:
            radio_mode_str = "CONCURRENT_AB";
            break;
        default:
            radio_mode_str = "SINGLE_A";
            break;
    }
    cJSON_AddStringToObject(root, "radio_operation_mode", radio_mode_str);
    
    return root;
}

esp_err_t WebSocketHandlers::validate_relay_control_data(const cJSON* data, int* relay_id, bool* state) {
    if (!data || !relay_id || !state) {
        return ESP_ERR_INVALID_ARG;
    }
    
    const cJSON *relay = cJSON_GetObjectItem(data, "relay");
    const cJSON *state_json = cJSON_GetObjectItem(data, "state");
    
    if (!cJSON_IsNumber(relay) || !cJSON_IsBool(state_json)) {
        ESP_LOGE(TAG, "Invalid relay or state in request");
        return ESP_FAIL;
    }
    
    *relay_id = relay->valueint;
    *state = cJSON_IsTrue(state_json);
    
    if (*relay_id < 1 || *relay_id > RelayController::NUM_RELAYS) {
        ESP_LOGE(TAG, "Invalid relay ID: %d", *relay_id);
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

esp_err_t WebSocketHandlers::validate_antenna_switch_data(const cJSON* data, char* radio, char* action, int* antenna_number) {
    if (!data || !radio || !action) {
        return ESP_ERR_INVALID_ARG;
    }
    
    const cJSON *radio_json = cJSON_GetObjectItem(data, "radio");
    
    if (!cJSON_IsString(radio_json)) {
        ESP_LOGE(TAG, "Invalid or missing radio parameter in request");
        return ESP_FAIL;
    }
    
    strncpy(radio, radio_json->valuestring, 7);
    radio[7] = '\0';
    
    // Check for antenna number (direct selection)
    const cJSON *antenna_json = cJSON_GetObjectItem(data, "antenna");
    if (cJSON_IsNumber(antenna_json)) {
        // Direct antenna selection
        if (antenna_number) {
            *antenna_number = antenna_json->valueint;
        }
        // Clear action for direct selection
        action[0] = '\0';
        ESP_LOGD(TAG, "Direct antenna selection: radio=%s, antenna=%d", radio, antenna_json->valueint);
        return ESP_OK;
    }
    
    // Check for action (next/previous)
    const cJSON *action_json = cJSON_GetObjectItem(data, "action");
    if (!cJSON_IsString(action_json)) {
        ESP_LOGE(TAG, "Missing both 'action' and 'antenna' parameters in request");
        return ESP_FAIL;
    }
    
    strncpy(action, action_json->valuestring, 15);
    action[15] = '\0';
    
    if (antenna_number) {
        *antenna_number = 0; // No direct selection
    }
    
    ESP_LOGD(TAG, "Next/previous antenna selection: radio=%s, action=%s", radio, action);
    return ESP_OK;
}
