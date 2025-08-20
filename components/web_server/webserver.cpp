// C++ Standard Library headers
#include <algorithm>
#include <string>
#include <memory>
#include <vector>
#include <arpa/inet.h>

// Project C++ headers
#include "webserver.h"
#include "config_manager.h" // Added for ConfigManager
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_manager.hpp"
#include "cJSON.h"
#include "html_content.h"
#include "relay_controller.h"

// Headers with class interfaces
#include "antenna_switch.h"
#include "cat_parser.h"
#include "my_mqtt_client.h"

static const char* TAG = "WEBSERVER";

#define MIN(a,b) ((a) < (b) ? (a) : (b))

// Singleton instance
WebServer& WebServer::instance() {
    static WebServer instance;
    return instance;
}

WebServer::WebServer() : m_server(nullptr), m_config() {
    ESP_LOGD(TAG, "WebServer constructor called");
}

esp_err_t WebServer::error_handler(httpd_req_t *req, httpd_err_code_t err) {
    ESP_LOGW(TAG, "HTTP Error %d occurred", err);
    httpd_resp_send_err(req, err, "Something went wrong");
    // Ensure connection is closed
    httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
    return ESP_FAIL;
}

esp_err_t WebServer::root_get_handler(httpd_req_t *req) {
    antenna_switch_config_t ant_config;

    esp_err_t ret = AntennaSwitch::instance().get_config(&ant_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get configuration: %s", esp_err_to_name(ret));
    }

    char ip_addr[16];
    ret = WifiManager::instance().get_ip_info(ip_addr, sizeof(ip_addr));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get IP address: %s", esp_err_to_name(ret));
        strcpy(ip_addr, "Unknown");
    }

    char mac_addr[18];
    ret = WifiManager::instance().get_mac_address(mac_addr, sizeof(mac_addr));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get MAC address: %s", esp_err_to_name(ret));
        strcpy(mac_addr, "Unknown");
    }

    // Use chunked HTML generation to avoid memory issues
    return HtmlContent::generate_root_html_chunked(req, ant_config, ip_addr, mac_addr);
}

esp_err_t WebServer::config_get_handler(httpd_req_t *req) {
    ESP_LOGD(TAG, "Entering config_get_handler");

    antenna_switch_config_t switch_config;
    if (const esp_err_t ret = AntennaSwitch::instance().get_config(&switch_config); ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get configuration: %s", esp_err_to_name(ret));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to get configuration");
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Configuration retrieved successfully");
    ESP_LOGD(TAG, "Number of bands: %d, Number of antenna ports: %d", switch_config.num_bands, switch_config.num_antenna_ports);

    // Check and fix configuration values before potentially saving
    bool config_corrected = false;
    
    if (switch_config.num_bands <= 0 || switch_config.num_bands > MAX_BANDS) {
        ESP_LOGE(TAG, "Invalid number of bands: %d (should be between 1 and %d)", switch_config.num_bands, MAX_BANDS);
        switch_config.num_bands = 8; // Use a reasonable default
        ESP_LOGD(TAG, "Resetting number of bands to %d", switch_config.num_bands);
        config_corrected = true;
    }

    if (switch_config.num_antenna_ports <= 0 || switch_config.num_antenna_ports > MAX_ANTENNA_PORTS) {
        ESP_LOGE(TAG, "Invalid number of antenna ports: %d (should be between 1 and %d)", switch_config.num_antenna_ports, MAX_ANTENNA_PORTS);
        switch_config.num_antenna_ports = 6; // Use a reasonable default
        ESP_LOGD(TAG, "Resetting number of antenna ports to %d", switch_config.num_antenna_ports);
        config_corrected = true;
    }

    // Save the corrected configuration if any corrections were made
    if (config_corrected) {
        if (const esp_err_t save_ret = AntennaSwitch::instance().set_config(&switch_config); save_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save corrected configuration: %s", esp_err_to_name(save_ret));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save corrected configuration");
            return ESP_FAIL;
        }
        ESP_LOGD(TAG, "Corrected configuration saved successfully");
    }

    // The HTML will be sent in chunks by generate_config_html_chunked
    httpd_resp_set_type(req, "text/html");
    esp_err_t gen_err = HtmlContent::generate_config_html_chunked(req, switch_config);

    if (gen_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to generate or send config HTML: %s", esp_err_to_name(gen_err));
        // If headers haven't been sent, an error response might be possible,
        // but httpd_resp_send_chunk might have already started sending.
        // For now, just return the error. The connection might be closed abruptly.
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Config HTML sent successfully in chunks");
    return ESP_OK;
}

esp_err_t WebServer::status_get_handler(httpd_req_t *req) {
    // Get current frequency from CAT parser
    const uint32_t current_freq = cat_parser_get_frequency();
    const bool is_transmitting = cat_parser_get_transmit();

    // Get current antenna configuration
    antenna_switch_config_t config;
    if (const esp_err_t ret = AntennaSwitch::instance().get_config(&config); ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get configuration: %s", esp_err_to_name(ret));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to get configuration");
        return ESP_FAIL;
    }

    // Modify Radio A's active antenna determination
    uint16_t relay_states = RelayController::instance().get_relay_states();
    int active_antenna_a_num = 0;
    // Radio A: relays 1-8 (0-indexed 0-7)
    for (int i = 0; i < RelayController::RELAYS_PER_RADIO; i++) { // Use RELAYS_PER_RADIO
        if (!((relay_states >> i) & 1)) { // Active low
            active_antenna_a_num = i + 1;
            break;
        }
    }

    // Add debug logging for transmit state
    ESP_LOGD(TAG, "Transmit state (Radio A): %d", is_transmitting);
    ESP_LOGD(TAG, "Active antenna (Radio A): %d", active_antenna_a_num);
    ESP_LOGD(TAG, "Current frequency (Radio A): %lu", current_freq);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "frequency", current_freq);
    cJSON_AddStringToObject(root, "antenna", active_antenna_a_num ? 
        ("Antenna " + std::to_string(active_antenna_a_num)).c_str() : "None");
    cJSON_AddBoolToObject(root, "transmitting", is_transmitting);
    
    // Determine data source for Radio A
    bool has_serial_data = MQTTClient::instance().has_serial_data();
    bool mqtt_enabled = config.mqtt_enabled;
    bool mqtt_has_frequency = MQTTClient::instance().get_current_frequency() > 0;
    
    const char* data_source_a = "None";
    if (has_serial_data) {
        data_source_a = "Serial";
    } else if (mqtt_enabled && mqtt_has_frequency) {
        data_source_a = "MQTT";
    }
    cJSON_AddStringToObject(root, "data_source", data_source_a);
    
    std::vector<int> available_antennas_a;

    if (current_freq > 0) { // Only if frequency is known for Radio A
        for (int band_idx = 0; band_idx < config.num_bands; band_idx++) {
            const auto &band_cfg = config.bands[0][band_idx]; // Radio A band config (bands[0])
            if (current_freq >= band_cfg.start_freq && current_freq <= band_cfg.end_freq) {
                // Check physical ports 1-8 (indices 0-7 in antenna_ports array)
                for (int port_idx = 0; port_idx < RelayController::RELAYS_PER_RADIO; port_idx++) {
                    if (port_idx < MAX_ANTENNA_PORTS && band_cfg.antenna_ports[port_idx]) {
                        available_antennas_a.push_back(port_idx + 1); // Relay number
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
    cJSON_AddItemToObject(root, "available_antennas", antennas_a_json); // Keep original name for Radio A

    // Add Radio B status if applicable
    if (config.radio_operation_mode != RADIO_OP_MODE_SINGLE_A) {
        int active_antenna_b_num = 0;
        // Radio B: relays 9-16 (0-indexed 8-15)
        for (int i = RelayController::RELAYS_PER_RADIO; i < RelayController::NUM_RELAYS; i++) {
            if (!((relay_states >> i) & 1)) { // Active low
                active_antenna_b_num = i + 1;
                break;
            }
        }
        cJSON_AddStringToObject(root, "antenna_b", active_antenna_b_num ?
            ("Antenna " + std::to_string(active_antenna_b_num)).c_str() : "None");

        // Placeholder for Radio B frequency and transmit state.
        // These would need a proper source, e.g., a second CAT parser instance or MQTT.
        constexpr uint32_t current_freq_b = 0;
        constexpr auto is_transmitting_b = false;
        cJSON_AddNumberToObject(root, "frequency_b", current_freq_b);
        cJSON_AddBoolToObject(root, "transmitting_b", is_transmitting_b);
        
        // For now, Radio B uses the same data source determination as Radio A
        // This could be different if Radio B had a separate CAT interface
        const char* data_source_b = data_source_a;
        cJSON_AddStringToObject(root, "data_source_b", data_source_b);

        // Placeholder for available_antennas_b. This will be empty until current_freq_b is known.

        // Example logic if current_freq_b were available:
        /*
        if (current_freq_b > 0) {
            for (int band_idx = 0; band_idx < config.num_bands; band_idx++) {
                const auto &band_cfg = config.bands[1][band_idx]; // Radio B band config (bands[1])
                if (current_freq_b >= band_cfg.start_freq && current_freq_b <= band_cfg.end_freq) {
                    // Check physical ports 9-16 (indices 8-15 in antenna_ports array)
                    for (int port_idx = RelayController::RELAYS_PER_RADIO; port_idx < MAX_ANTENNA_PORTS; port_idx++) {
                        if (band_cfg.antenna_ports[port_idx]) {
                            available_antennas_b_list.push_back(port_idx + 1); // Relay number
                        }
                    }
                    break;
                }
            }
        }
        */
        cJSON *antennas_b_json = cJSON_CreateArray();

        for (constexpr std::vector<int> available_antennas_b_list; int antenna : available_antennas_b_list) {
            cJSON_AddItemToArray(antennas_b_json, cJSON_CreateNumber(antenna));
        }
        cJSON_AddItemToObject(root, "available_antennas_b", antennas_b_json);
    }

    char *json_string = cJSON_Print(root);
    ESP_LOGD(TAG, "Sending JSON response: %s", json_string);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_string);

    free(json_string);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t WebServer::config_post_handler(httpd_req_t *req) {
    antenna_switch_config_t new_config = {};

    // Get content length and validate
    const size_t content_len = req->content_len;
    if (content_len > MAX_POST_SIZE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content too large");
        return ESP_FAIL;
    }
    
    // Allocate memory for content
    const auto content = static_cast<char *>(malloc(content_len + 1));

    if (!content) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to allocate memory");
        return ESP_FAIL;
    }
    
    // Read the data
    if (const int received = httpd_req_recv(req, content, content_len); received <= 0) {
        free(content);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive data");
        return ESP_FAIL;
    }
    content[content_len] = '\0';

    // Parse JSON
    auto root = cJSON_Parse(content);
    if (!root) {
        free(content);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    // Parse configuration from JSON
    const cJSON *auto_mode = cJSON_GetObjectItem(root, "auto_mode");
    new_config.auto_mode = cJSON_IsTrue(auto_mode);

    const cJSON *allow_concurrent_data_sources= cJSON_GetObjectItem(root, "allow_concurrent_data_sources");
    new_config.allow_concurrent_data_sources = cJSON_IsTrue(allow_concurrent_data_sources);

    // Parse radio_operation_mode
    const cJSON *radio_op_mode_json = cJSON_GetObjectItem(root, "radio_operation_mode");
    if (cJSON_IsString(radio_op_mode_json) && radio_op_mode_json->valuestring != nullptr) {

        if (strcmp(radio_op_mode_json->valuestring, "SINGLE_A") == 0) {
            new_config.radio_operation_mode = RADIO_OP_MODE_SINGLE_A;
        } else if (strcmp(radio_op_mode_json->valuestring, "ALTERNATING_AB") == 0) {
            new_config.radio_operation_mode = RADIO_OP_MODE_ALTERNATING_AB;
        } else if (strcmp(radio_op_mode_json->valuestring, "CONCURRENT_AB") == 0) {
            new_config.radio_operation_mode = RADIO_OP_MODE_CONCURRENT_AB;
        } else {
            ESP_LOGW(TAG, "Invalid radio_operation_mode string: %s. Defaulting to SINGLE_A.", radio_op_mode_json->valuestring);
            new_config.radio_operation_mode = RADIO_OP_MODE_SINGLE_A; 
        }
    } else {
        ESP_LOGW(TAG, "radio_operation_mode not found or not a string. Defaulting to SINGLE_A.");
        new_config.radio_operation_mode = RADIO_OP_MODE_SINGLE_A; 
    }

    // Parse interlock_auto_resolves_conflict
    const cJSON* interlock_json = cJSON_GetObjectItem(root, "interlock_auto_resolves_conflict");
    new_config.interlock_auto_resolves_conflict = cJSON_IsTrue(interlock_json);

    // Parse auto_restore_on_conflict_resolution
    const cJSON* auto_restore_json = cJSON_GetObjectItem(root, "auto_restore_on_conflict_resolution");
    new_config.auto_restore_on_conflict_resolution = cJSON_IsTrue(auto_restore_json);

    // Parse radio_restore_delay_ms
    const cJSON* restore_delay_json = cJSON_GetObjectItem(root, "radio_restore_delay_ms");
    if (cJSON_IsNumber(restore_delay_json)) {
        new_config.radio_restore_delay_ms = restore_delay_json->valueint;
        if (new_config.radio_restore_delay_ms < 1) new_config.radio_restore_delay_ms = 1; // Min value updated to 1ms
        if (new_config.radio_restore_delay_ms > 5000) new_config.radio_restore_delay_ms = 5000; // Max value
    } else {
        new_config.radio_restore_delay_ms = 200; // Default if not provided or invalid
        ESP_LOGW(TAG, "radio_restore_delay_ms not found or invalid in JSON, defaulting to 200ms.");
    }

    // Parse UART configuration
    if (const cJSON *uart_baud = cJSON_GetObjectItem(root, "uart_baud_rate"); cJSON_IsNumber(uart_baud)) {
        if (uart_baud->valueint > 0) {
            new_config.uart_baud_rate = uart_baud->valueint;
            ESP_LOGD(TAG, "Setting UART baud rate to: %d", uart_baud->valueint);
        } else {
            ESP_LOGE(TAG, "Invalid baud rate: %d", uart_baud->valueint);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid baud rate");
            return ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "UART baud rate not specified or invalid");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid or missing baud rate");
        return ESP_FAIL;
    }

    if (cJSON const *uart_parity = cJSON_GetObjectItem(root, "uart_parity"); cJSON_IsNumber(uart_parity)) {
        new_config.uart_parity = uart_parity->valueint;
    } else {
        ESP_LOGE(TAG, "UART parity not specified or invalid");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid or missing UART parity");
        cJSON_Delete(root);
        free(content);
        return ESP_FAIL;
    }

    if (const cJSON *uart_stop_bits = cJSON_GetObjectItem(root, "uart_stop_bits"); cJSON_IsNumber(uart_stop_bits)) {
        new_config.uart_stop_bits = uart_stop_bits->valueint;
    } else {
        ESP_LOGE(TAG, "UART stop bits not specified or invalid");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid or missing UART stop bits");
        cJSON_Delete(root);
        free(content);
        return ESP_FAIL;
    }

    if (const cJSON *uart_flow_ctrl = cJSON_GetObjectItem(root, "uart_flow_ctrl"); cJSON_IsNumber(uart_flow_ctrl)) {
        new_config.uart_flow_ctrl = uart_flow_ctrl->valueint;
        ESP_LOGD(TAG, "Setting UART flow control to: %i", uart_flow_ctrl->valueint);
    } else {
        ESP_LOGE(TAG, "UART flow control not specified or invalid");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid or missing UART flow control");
        cJSON_Delete(root);
        free(content);
        return ESP_FAIL;
    }

    // Handle UART TX pin
    if (const cJSON *uart_tx = cJSON_GetObjectItem(root, "uart_tx_pin"); cJSON_IsNumber(uart_tx)) {
        new_config.uart_tx_pin = uart_tx->valueint;
        ESP_LOGD(TAG, "Setting UART TX pin to: %i", uart_tx->valueint);
    } else {
        ESP_LOGE(TAG, "UART TX pin not specified or invalid");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid or missing UART TX pin");
        cJSON_Delete(root);
        free(content);
        return ESP_FAIL;
    }

    // Handle UART RX pin
    if (const cJSON *uart_rx = cJSON_GetObjectItem(root, "uart_rx_pin"); cJSON_IsNumber(uart_rx)) {
        new_config.uart_rx_pin = uart_rx->valueint;
        ESP_LOGD(TAG, "Setting UART RX pin to: %i", uart_rx->valueint);
    } else {
        ESP_LOGE(TAG, "UART RX pin not specified or invalid");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid or missing UART RX pin");
        cJSON_Delete(root);
        free(content);
        return ESP_FAIL;
    }

    // Parse PTT Configuration for Radio A
    if (const cJSON *ptt_pin_a_json = cJSON_GetObjectItem(root, "ptt_input_radio_a"); cJSON_IsNumber(ptt_pin_a_json)) {
        new_config.ptt_input_radio_a = ptt_pin_a_json->valueint;
        ESP_LOGD(TAG, "Setting PTT Input Pin (Radio A) to: %d", new_config.ptt_input_radio_a);
    } else {
        ESP_LOGW(TAG, "PTT Input Pin (Radio A) not specified or invalid, defaulting to -1 (disabled)");
        new_config.ptt_input_radio_a = -1;
    }

    if (const cJSON *ptt_active_high_a_json = cJSON_GetObjectItem(root, "ptt_input_radio_a_active_high"); cJSON_IsBool(ptt_active_high_a_json)) {
        new_config.ptt_input_radio_a_active_high = cJSON_IsTrue(ptt_active_high_a_json);
        ESP_LOGD(TAG, "Setting PTT Active High (Radio A) to: %s", new_config.ptt_input_radio_a_active_high ? "true" : "false");
    } else {
        ESP_LOGW(TAG, "PTT Active High (Radio A) not specified or invalid, defaulting to true");
        new_config.ptt_input_radio_a_active_high = true;
    }

    // Parse PTT Configuration for Radio B
    if (const cJSON *ptt_pin_b_json = cJSON_GetObjectItem(root, "ptt_input_radio_b"); cJSON_IsNumber(ptt_pin_b_json)) {
        new_config.ptt_input_radio_b = ptt_pin_b_json->valueint;
        ESP_LOGD(TAG, "Setting PTT Input Pin (Radio B) to: %d", new_config.ptt_input_radio_b);
    } else {
        ESP_LOGW(TAG, "PTT Input Pin (Radio B) not specified or invalid, defaulting to -1 (disabled)");
        new_config.ptt_input_radio_b = -1;
    }

    if (const cJSON *ptt_active_high_b_json = cJSON_GetObjectItem(root, "ptt_input_radio_b_active_high"); cJSON_IsBool(ptt_active_high_b_json)) {
        new_config.ptt_input_radio_b_active_high = cJSON_IsTrue(ptt_active_high_b_json);
        ESP_LOGD(TAG, "Setting PTT Active High (Radio B) to: %s", new_config.ptt_input_radio_b_active_high ? "true" : "false");
    } else {
        ESP_LOGW(TAG, "PTT Active High (Radio B) not specified or invalid, defaulting to true");
        new_config.ptt_input_radio_b_active_high = true;
    }

    // Parse MQTT settings
    const cJSON *mqtt_enabled = cJSON_GetObjectItem(root, "mqtt_enabled");
    new_config.mqtt_enabled = cJSON_IsTrue(mqtt_enabled);

    const cJSON *mqtt_broker = cJSON_GetObjectItem(root, "mqtt_broker");
    if (cJSON_IsString(mqtt_broker)) {
        strncpy(new_config.mqtt_broker, mqtt_broker->valuestring, sizeof(new_config.mqtt_broker) - 1);
    }
    
    // Parse relay names
    const cJSON *relay_names = cJSON_GetObjectItem(root, "relay_names");
    if (cJSON_IsArray(relay_names)) {
        const int num_names = cJSON_GetArraySize(relay_names);
        for (int i = 0; i < num_names && i < 16; i++) {
            const cJSON *name = cJSON_GetArrayItem(relay_names, i);
            if (cJSON_IsString(name) && name->valuestring != nullptr) {
                strncpy(new_config.relay_names[i], name->valuestring, sizeof(new_config.relay_names[i]) - 1);
                new_config.relay_names[i][sizeof(new_config.relay_names[i]) - 1] = '\0';
                ESP_LOGI(TAG, "Relay %d name: %s", i+1, new_config.relay_names[i]);
            }
        }
    }

    const cJSON *mqtt_port = cJSON_GetObjectItem(root, "mqtt_port");
    if (cJSON_IsNumber(mqtt_port)) {
        new_config.mqtt_port = mqtt_port->valueint;
    }

    const cJSON *mqtt_rig_id = cJSON_GetObjectItem(root, "mqtt_rig_id");
    if (cJSON_IsString(mqtt_rig_id)) {
        strncpy(new_config.mqtt_rig_id, mqtt_rig_id->valuestring, sizeof(new_config.mqtt_rig_id) - 1);
    }

    // Debug log the entire JSON content
    char* debug_json = cJSON_Print(root);
    ESP_LOGI(TAG, "Received JSON: %s", debug_json);
    free(debug_json);

    const cJSON *mqtt_username = cJSON_GetObjectItem(root, "mqtt_username");
    ESP_LOGI(TAG, "Processing MQTT username");
    if (mqtt_username && mqtt_username->valuestring) {
        ESP_LOGI(TAG, "Received MQTT username: '%s'", mqtt_username->valuestring);
        strncpy(new_config.mqtt_username, mqtt_username->valuestring, sizeof(new_config.mqtt_username) - 1);
        new_config.mqtt_username[sizeof(new_config.mqtt_username) - 1] = '\0';
    } else {
        ESP_LOGW(TAG, "MQTT username not found in JSON or is null");
        new_config.mqtt_username[0] = '\0';
    }

    const cJSON *mqtt_password = cJSON_GetObjectItem(root, "mqtt_password");
    ESP_LOGI(TAG, "Processing MQTT password");
    if (mqtt_password && mqtt_password->valuestring) {
        ESP_LOGI(TAG, "Received MQTT password (length: %d)", strlen(mqtt_password->valuestring));
        strncpy(new_config.mqtt_password, mqtt_password->valuestring, sizeof(new_config.mqtt_password) - 1);
        new_config.mqtt_password[sizeof(new_config.mqtt_password) - 1] = '\0';
    } else {
        ESP_LOGW(TAG, "MQTT password not found in JSON or is null");
        new_config.mqtt_password[0] = '\0';
    }

    const cJSON *mqtt_client_id = cJSON_GetObjectItem(root, "mqtt_client_id");
    if (cJSON_IsString(mqtt_client_id)) {
        strncpy(new_config.mqtt_client_id, mqtt_client_id->valuestring, sizeof(new_config.mqtt_client_id) - 1);
    } else {
        // Set default client ID if not provided
        strncpy(new_config.mqtt_client_id, "core-mosquitto", sizeof(new_config.mqtt_client_id) - 1);
    }

    const cJSON *mqtt_topic = cJSON_GetObjectItem(root, "mqtt_topic");
    if (cJSON_IsString(mqtt_topic)) {
        strncpy(new_config.mqtt_topic, mqtt_topic->valuestring, sizeof(new_config.mqtt_topic) - 1);
        new_config.mqtt_topic[sizeof(new_config.mqtt_topic) - 1] = '\0';
    } else {
        // Set default if not provided
        strncpy(new_config.mqtt_topic, "omnirig/frequent/radio_info", sizeof(new_config.mqtt_topic) - 1);
    }

    // Get num_bands and num_antenna_ports from JSON
    const cJSON *num_bands_json = cJSON_GetObjectItem(root, "num_bands");
    if (!cJSON_IsNumber(num_bands_json)) {
        ESP_LOGE(TAG, "Number of bands not specified or invalid");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid or missing UART RX pin");
        cJSON_Delete(root);
        free(content);
        return ESP_FAIL;
    }

    const int num_bands = num_bands_json->valueint;
    if (num_bands <= 0 || num_bands > MAX_BANDS) {
        ESP_LOGE(TAG, "Invalid number of bands: %d", num_bands);
        cJSON_Delete(root);
        free(content);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid number of bands");
        return ESP_FAIL;
    }
    new_config.num_bands = num_bands;

    const cJSON *num_antenna_ports_json = cJSON_GetObjectItem(root, "num_antenna_ports");
    if (!cJSON_IsNumber(num_antenna_ports_json)) {
        ESP_LOGE(TAG, "Number of antenna ports not specified or invalid");
        cJSON_Delete(root);
        free(content);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid or missing num_antenna_ports");
        return ESP_FAIL;
    }

    const int num_antenna_ports = num_antenna_ports_json->valueint;
    if (num_antenna_ports <= 0 || num_antenna_ports > MAX_ANTENNA_PORTS) {
        ESP_LOGE(TAG, "Invalid number of antenna ports: %d", num_antenna_ports);
        cJSON_Delete(root);
        free(content);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid number of antenna ports");
        return ESP_FAIL;
    }
    new_config.num_antenna_ports = num_antenna_ports;

    if (const cJSON *bands_json_array = cJSON_GetObjectItem(root, "bands"); cJSON_IsArray(bands_json_array)) {
        const int num_parsed_bands = cJSON_GetArraySize(bands_json_array);
        // new_config.num_bands is already set from num_bands_json->valueint, use that as the authority.
        // Ensure we don't process more bands than MAX_BANDS or what's specified by num_bands.
        int bands_to_process = MIN(new_config.num_bands, num_parsed_bands);
        bands_to_process = MIN(bands_to_process, MAX_BANDS);


        for (int i = 0; i < bands_to_process; i++) {
            if (cJSON const *band_item_json = cJSON_GetArrayItem(bands_json_array, i); cJSON_IsObject(band_item_json)) {
                auto &band_config_a = new_config.bands[0][i];
                auto &band_config_b = new_config.bands[1][i];

                // Initialize band_config_b to be same as band_config_a for description/freq, then override ports
                memset(&band_config_a, 0, sizeof(band_config_t)); // Clear previous data
                memset(&band_config_b, 0, sizeof(band_config_t)); // Clear previous data


                if (const cJSON *description_json = cJSON_GetObjectItem(band_item_json, "description"); cJSON_IsString(description_json)) {
                    if (auto it = HtmlContent::band_info.find(description_json->valuestring); it != HtmlContent::band_info.end()) {
                        strncpy(band_config_a.description, it->second.name, sizeof(band_config_a.description) - 1);
                        band_config_a.start_freq = it->second.start_freq;
                        band_config_a.end_freq   = it->second.end_freq;

                        // Copy to Radio B config
                        strncpy(band_config_b.description, it->second.name, sizeof(band_config_b.description) - 1);
                        band_config_b.start_freq = it->second.start_freq;
                        band_config_b.end_freq   = it->second.end_freq;

                        ESP_LOGV(TAG, "Setting band %d: %s (%lu-%lu Hz) for Radio A/B", i,
                                band_config_a.description, band_config_a.start_freq, band_config_a.end_freq);
                    } else {
                        ESP_LOGW(TAG, "Unknown band description: %s for band %d", description_json->valuestring, i);
                    }
                } else {
                    ESP_LOGW(TAG, "Missing or invalid band description for band %d", i);
                }

                // Parse antenna_ports_a for Radio A
                if (const cJSON *ports_a_json = cJSON_GetObjectItem(band_item_json, "antenna_ports_a"); cJSON_IsArray(ports_a_json)) {
                    const int num_ports_a = cJSON_GetArraySize(ports_a_json);
                    for (int j = 0; j < num_ports_a && j < MAX_ANTENNA_PORTS; j++) {
                        band_config_a.antenna_ports[j] = cJSON_IsTrue(cJSON_GetArrayItem(ports_a_json, j));
                    }
                }

                // Parse antenna_ports_b for Radio B
                if (const cJSON *ports_b_json = cJSON_GetObjectItem(band_item_json, "antenna_ports_b"); cJSON_IsArray(ports_b_json)) {
                    const int num_ports_b = cJSON_GetArraySize(ports_b_json);
                    for (int j = 0; j < num_ports_b && j < MAX_ANTENNA_PORTS; j++) {
                        band_config_b.antenna_ports[j] = cJSON_IsTrue(cJSON_GetArrayItem(ports_b_json, j));
                    }
                }
            }
        }
    }
    
    cJSON_Delete(root);

    esp_err_t err = AntennaSwitch::instance().set_config(&new_config);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to set configuration");
        return ESP_FAIL;
    }

    // Update CAT parser configuration
    err = cat_parser_update_config();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to update CAT parser configuration: %s", esp_err_to_name(err));
    }

    // Use chunked sending for the success response
    httpd_resp_set_hdr(req, "Transfer-Encoding", "chunked");
    const auto success_msg = "<h2>Configuration Updated</h2>"
            "<p>The configuration was updated successfully.</p>"
            "<script>window.location.href='/';</script>";

    httpd_resp_sendstr_chunk(req, success_msg);
    httpd_resp_sendstr_chunk(req, nullptr); // Terminate chunked response
    
    free(content);
    return ESP_OK;
}

esp_err_t WebServer::toggle_auto_mode_handler(httpd_req_t *req) {
    antenna_switch_config_t ant_config;
    esp_err_t ret = AntennaSwitch::instance().get_config(&ant_config);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to get configuration");
        return ESP_FAIL;
    }

    ant_config.auto_mode = !ant_config.auto_mode;

    ret = AntennaSwitch::instance().set_config(&ant_config);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to set configuration");
        return ESP_FAIL;
    }

    // Redirect back to the home page
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

esp_err_t WebServer::reset_config_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Handling reset configuration request.");

    // Call ConfigManager to reset to its defined defaults
    if (const esp_err_t ret = ConfigManager::instance().reset_to_defaults(); ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reset configuration via ConfigManager: %s", esp_err_to_name(ret));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to reset configuration");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Configuration successfully reset to defaults.");

    // Redirect back to the config page
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/config");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

esp_err_t WebServer::restart_handler(httpd_req_t *req) {
    ESP_LOGD(TAG, "Handling restart request");

    // Send a success response before restarting
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Restarting...");

    // Small delay to allow the response to be sent
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // Restart the device
    AntennaSwitch::instance().restart();

    return ESP_OK;
}

esp_err_t WebServer::reset_wifi_handler(httpd_req_t *req) {
    ESP_LOGD(TAG, "Handling WiFi reset request");

    // Clear WiFi credentials
    if (const esp_err_t ret = WifiManager::instance().clear_credentials(); ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to clear WiFi credentials");
        return ESP_FAIL;
    }

    // Redirect back to root with success message
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

esp_err_t WebServer::relay_status_handler(httpd_req_t *req) {
    uint16_t relay_states = RelayController::instance().get_relay_states();
    
    // Log the states for debugging
    ESP_LOGD(TAG, "Raw relay states: 0x%04X", relay_states);
    
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "states", relay_states);

    char *json_string = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_string);

    free(json_string);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t WebServer::relay_control_handler(httpd_req_t *req) {
    char buf[32];
    int ret = httpd_req_recv(req, buf, MIN(req->content_len, sizeof(buf) - 1));
    if (ret <= 0) {
        ESP_LOGE(TAG, "Failed to receive relay control request");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    const cJSON *relay = cJSON_GetObjectItem(root, "relay");
    const cJSON *state = cJSON_GetObjectItem(root, "state");

    if (!cJSON_IsNumber(relay) || !cJSON_IsBool(state)) {
        ESP_LOGE(TAG, "Invalid relay or state in request");
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid relay or state");
        return ESP_FAIL;
    }

    const int relay_num = relay->valueint;
    const bool relay_state = cJSON_IsTrue(state);

    // apply policy & set the relay in one shot
    ESP_LOGD(TAG, "Calling antenna_switch_set_relay(%d, %d)", relay_num, relay_state);
    esp_err_t err = AntennaSwitch::instance().set_relay(relay_num, relay_state);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set relay: %s", esp_err_to_name(err));
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to set relay");
        return ESP_FAIL;
    }

    // Add a small delay to allow the hardware to update
    vTaskDelay(pdMS_TO_TICKS(50));

    // Verify the new state
    const bool current_state = RelayController::instance().get_relay_state(relay_num);
    ESP_LOGV(TAG, "Relay %d state after setting: %d", relay_num, current_state);

    cJSON_Delete(root);
    
    // Return the current state in the response
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "state", current_state);
    char *json_string = cJSON_Print(response);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_string);
    
    free(json_string);
    cJSON_Delete(response);
    return ESP_OK;
}

esp_err_t WebServer::config_export_handler(httpd_req_t *req) {
    ESP_LOGD(TAG, "Config export request received");

    char *json_config = nullptr;
    esp_err_t ret = ConfigManager::instance().export_config_to_json(&json_config);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to export configuration: %s", esp_err_to_name(ret));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to export configuration");
        return ESP_FAIL;
    }

    if (!json_config) {
        ESP_LOGE(TAG, "Export returned NULL configuration");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to generate configuration");
        return ESP_FAIL;
    }

    // Set headers for file download
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"kc868_config.json\"");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");

    // Send the JSON configuration
    httpd_resp_sendstr(req, json_config);
    
    // Free the allocated JSON string
    free(json_config);
    
    ESP_LOGI(TAG, "Configuration exported successfully");
    return ESP_OK;
}

esp_err_t WebServer::config_import_handler(httpd_req_t *req) {
    ESP_LOGD(TAG, "Config import request received");

    // Get content length and validate
    const size_t content_len = req->content_len;
    if (content_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No configuration data received");
        return ESP_FAIL;
    }

    if (content_len > MAX_POST_SIZE * 4) { // Allow larger size for JSON configs
        ESP_LOGE(TAG, "Configuration file too large: %zu bytes", content_len);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Configuration file too large");
        return ESP_FAIL;
    }

    // Allocate memory for content
    char *content = (char *)malloc(content_len + 1);
    if (!content) {
        ESP_LOGE(TAG, "Failed to allocate memory for config import");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }

    // Read the configuration data
    size_t remaining = content_len;
    size_t offset = 0;
    
    while (remaining > 0) {
        int received = httpd_req_recv(req, content + offset, remaining);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                ESP_LOGW(TAG, "Socket timeout during config import");
                continue;
            }
            ESP_LOGE(TAG, "Error receiving config data: %d", received);
            free(content);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive configuration data");
            return ESP_FAIL;
        }
        remaining -= received;
        offset += received;
    }
    
    content[content_len] = '\0';
    ESP_LOGD(TAG, "Received %zu bytes of configuration data", content_len);

    // First validate the configuration without applying it
    esp_err_t validate_ret = ConfigManager::instance().import_config_from_json(content, true);
    if (validate_ret != ESP_OK) {
        ESP_LOGE(TAG, "Configuration validation failed: %s", esp_err_to_name(validate_ret));
        free(content);
        
        cJSON *error_response = cJSON_CreateObject();
        cJSON_AddStringToObject(error_response, "status", "error");
        cJSON_AddStringToObject(error_response, "message", "Configuration validation failed");
        cJSON_AddStringToObject(error_response, "error_code", esp_err_to_name(validate_ret));
        
        char *error_json = cJSON_Print(error_response);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, error_json);
        
        free(error_json);
        cJSON_Delete(error_response);
        return ESP_FAIL;
    }

    // Apply the configuration
    esp_err_t import_ret = ConfigManager::instance().import_config_from_json(content, false);
    free(content);
    
    if (import_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to apply imported configuration: %s", esp_err_to_name(import_ret));
        
        cJSON *error_response = cJSON_CreateObject();
        cJSON_AddStringToObject(error_response, "status", "error");
        cJSON_AddStringToObject(error_response, "message", "Failed to apply configuration");
        cJSON_AddStringToObject(error_response, "error_code", esp_err_to_name(import_ret));
        
        char *error_json = cJSON_Print(error_response);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, error_json);
        
        free(error_json);
        cJSON_Delete(error_response);
        return ESP_FAIL;
    }

    // Send success response
    cJSON *success_response = cJSON_CreateObject();
    cJSON_AddStringToObject(success_response, "status", "success");
    cJSON_AddStringToObject(success_response, "message", "Configuration imported successfully");
    
    char *success_json = cJSON_Print(success_response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, success_json);
    
    free(success_json);
    cJSON_Delete(success_response);
    
    ESP_LOGI(TAG, "Configuration imported and applied successfully");
    return ESP_OK;
}

esp_err_t WebServer::relay_names_handler(httpd_req_t *req) {
    ESP_LOGD(TAG, "Relay names request received");

    antenna_switch_config_t config;
    esp_err_t ret = AntennaSwitch::instance().get_config(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get configuration: %s", esp_err_to_name(ret));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to get configuration");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        ESP_LOGE(TAG, "Failed to create JSON object");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }

    // Add relay names (1-16)
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

    char *json_string = cJSON_Print(root);
    if (!json_string) {
        ESP_LOGE(TAG, "Failed to generate JSON string");
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to generate response");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_string);

    free(json_string);
    cJSON_Delete(root);
    
    ESP_LOGD(TAG, "Relay names sent successfully");
    return ESP_OK;
}

esp_err_t WebServer::config_basic_handler(httpd_req_t *req) {
    ESP_LOGD(TAG, "Basic config request received");

    antenna_switch_config_t config;
    esp_err_t ret = AntennaSwitch::instance().get_config(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get configuration: %s", esp_err_to_name(ret));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to get configuration");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        ESP_LOGE(TAG, "Failed to create JSON object");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }

    // Add basic configuration fields
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

    // Add relay names array
    cJSON *relay_names_array = cJSON_CreateArray();
    if (relay_names_array) {
        for (int i = 0; i < 16; i++) {
            const char* relay_name;
            if (config.relay_names[i][0] != '\0') {
                relay_name = config.relay_names[i];
            } else {
                static char default_name[16];
                snprintf(default_name, sizeof(default_name), "Relay %d", i + 1);
                relay_name = default_name;
            }
            cJSON_AddItemToArray(relay_names_array, cJSON_CreateString(relay_name));
        }
        cJSON_AddItemToObject(root, "relay_names", relay_names_array);
    }

    char *json_string = cJSON_Print(root);
    if (!json_string) {
        ESP_LOGE(TAG, "Failed to generate JSON string");
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to generate response");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_string);

    free(json_string);
    cJSON_Delete(root);
    
    ESP_LOGD(TAG, "Basic config sent successfully");
    return ESP_OK;
}

esp_err_t WebServer::register_uri_handlers() const
{
    // Register global error handler first
    esp_err_t ret = httpd_register_err_handler(m_server, HTTPD_500_INTERNAL_SERVER_ERROR, error_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register error handler: %s", esp_err_to_name(ret));
        return ret;
    }

    static constexpr httpd_uri_t root = {
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = root_get_handler,
        .user_ctx  = nullptr
    };

    static constexpr httpd_uri_t config_get = {
        .uri       = "/config",
        .method    = HTTP_GET,
        .handler   = config_get_handler,
        .user_ctx  = nullptr
    };

    static constexpr httpd_uri_t config_post = {
        .uri       = "/config",
        .method    = HTTP_POST,
        .handler   = config_post_handler,
        .user_ctx  = nullptr
    };

    static constexpr httpd_uri_t status = {
        .uri       = "/status",
        .method    = HTTP_GET,
        .handler   = status_get_handler,
        .user_ctx  = nullptr
    };

    static constexpr httpd_uri_t toggle_auto_mode = {
        .uri       = "/toggle-auto-mode",
        .method    = HTTP_POST,
        .handler   = toggle_auto_mode_handler,
        .user_ctx  = nullptr
    };

    static constexpr httpd_uri_t reset_config = {
        .uri       = "/reset-config",
        .method    = HTTP_POST,
        .handler   = reset_config_handler,
        .user_ctx  = nullptr
    };

    static constexpr httpd_uri_t restart = {
        .uri       = "/restart",
        .method    = HTTP_POST,
        .handler   = restart_handler,
        .user_ctx  = nullptr
    };

    static constexpr httpd_uri_t reset_wifi = {
        .uri       = "/reset-wifi",
        .method    = HTTP_POST,
        .handler   = reset_wifi_handler,
        .user_ctx  = nullptr
    };

    static constexpr httpd_uri_t relay_status = {
        .uri = "/relay/status",
        .method = HTTP_GET,
        .handler = relay_status_handler,
        .user_ctx = nullptr
    };

    static constexpr httpd_uri_t relay_control = {
        .uri = "/relay/control",
        .method = HTTP_POST,
        .handler = relay_control_handler,
        .user_ctx = nullptr
    };

    static constexpr httpd_uri_t config_export = {
        .uri = "/api/config/export",
        .method = HTTP_GET,
        .handler = config_export_handler,
        .user_ctx = nullptr
    };

    static constexpr httpd_uri_t config_import = {
        .uri = "/api/config/import",
        .method = HTTP_POST,
        .handler = config_import_handler,
        .user_ctx = nullptr
    };

    static constexpr httpd_uri_t relay_names = {
        .uri = "/api/relay/names",
        .method = HTTP_GET,
        .handler = relay_names_handler,
        .user_ctx = nullptr
    };

    static constexpr httpd_uri_t config_basic = {
        .uri = "/api/config/basic",
        .method = HTTP_GET,
        .handler = config_basic_handler,
        .user_ctx = nullptr
    };

    static constexpr httpd_uri_t favicon = {
        .uri = "/favicon.ico",
        .method = HTTP_GET,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            httpd_resp_set_status(req, "204 No Content");
            return httpd_resp_send(req, nullptr, 0);
        },
        .user_ctx = nullptr
    };

    ESP_LOGV(TAG, "Registering URI handlers");
    
    ret = httpd_register_uri_handler(m_server, &root);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register root URI handler: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = httpd_register_uri_handler(m_server, &config_get);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register config GET URI handler: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = httpd_register_uri_handler(m_server, &config_post);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register config POST URI handler: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = httpd_register_uri_handler(m_server, &status);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register status URI handler: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = httpd_register_uri_handler(m_server, &toggle_auto_mode);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register toggle auto mode URI handler: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = httpd_register_uri_handler(m_server, &reset_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register reset config URI handler: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = httpd_register_uri_handler(m_server, &restart);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register restart URI handler: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = httpd_register_uri_handler(m_server, &reset_wifi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register reset wifi URI handler: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = httpd_register_uri_handler(m_server, &relay_status);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register relay status handler: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = httpd_register_uri_handler(m_server, &relay_control);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register relay control handler: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = httpd_register_uri_handler(m_server, &config_export);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register config export handler: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = httpd_register_uri_handler(m_server, &config_import);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register config import handler: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = httpd_register_uri_handler(m_server, &relay_names);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register relay names handler: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = httpd_register_uri_handler(m_server, &config_basic);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register config basic handler: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = httpd_register_uri_handler(m_server, &favicon);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register favicon handler: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

esp_err_t WebServer::init() {
    ESP_LOGI(TAG, "Initializing webserver");
    
    m_config = HTTPD_DEFAULT_CONFIG();
    m_config.stack_size = 8192;
    m_config.task_priority = tskIDLE_PRIORITY+5;
    m_config.max_uri_handlers = 15;
    m_config.max_resp_headers = 4;
    m_config.lru_purge_enable = true;    // Enable LRU purging for large requests
    m_config.recv_wait_timeout = 5;
    m_config.uri_match_fn = httpd_uri_match_wildcard;
    m_config.keep_alive_enable = false;
    m_config.max_open_sockets = 3;

    return ESP_OK;
}

esp_err_t WebServer::start() {
    if (m_server == nullptr) {
        ESP_LOGD(TAG, "Starting webserver");

        char ip_addr[16];
        esp_err_t ret = WifiManager::instance().get_ip_info(ip_addr, sizeof(ip_addr));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to get IP address: %s", esp_err_to_name(ret));
            return ret;
        }

        ESP_LOGI(TAG, "Starting server on IP: %s, port: '%d'", ip_addr, m_config.server_port);
        ret = httpd_start(&m_server, &m_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Error starting server: %s", esp_err_to_name(ret));
            return ret;
        }

        // Register URI handlers immediately after starting server
        ret = register_uri_handlers();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register URI handlers: %s", esp_err_to_name(ret));
            httpd_stop(m_server);
            m_server = nullptr;
            return ret;
        }

        ESP_LOGI(TAG, "Server daemon started successfully with all URI handlers registered.");
    }
    return ESP_OK;
}

esp_err_t WebServer::stop() {
    if (m_server) {
        ESP_LOGD(TAG, "Stopping webserver");
        httpd_stop(m_server);
        m_server = nullptr;
    }
    return ESP_OK;
}

esp_err_t WebServer::restart() {
    ESP_LOGD(TAG, "Restarting webserver");
    stop();
    return start();
}

bool WebServer::is_running() const {
    return m_server != nullptr;
}

// Legacy C-style function wrappers for backwards compatibility
extern "C" {
    esp_err_t webserver_init() {
        return WebServer::instance().init();
    }

    esp_err_t webserver_start() {
        return WebServer::instance().start();
    }

    esp_err_t webserver_stop() {
        return WebServer::instance().stop();
    }

    esp_err_t webserver_restart() {
        return WebServer::instance().restart();
    }

    bool webserver_is_running() {
        return WebServer::instance().is_running();
    }
}
