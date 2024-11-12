// C++ Standard Library headers
#include <algorithm>
#include <string>
#include <memory>
#include <vector>
#include <arpa/inet.h>

// Project C++ headers
#include "webserver.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_manager.hpp"
#include "cJSON.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "html_content.h"
#include "relay_controller.h"
#include "config_manager.h"

// Headers with C interfaces
#include "antenna_switch.h"
#include "cat_parser.h"

static auto TAG = "WEBSERVER";

static httpd_handle_t server = nullptr;
static httpd_config_t config;
constexpr size_t MAX_POST_SIZE = 4096;
#define MIN(a,b) ((a) < (b) ? (a) : (b))

static esp_err_t root_get_handler(httpd_req_t *req) {
    antenna_switch_config_t config;

    esp_err_t ret = antenna_switch_get_config(&config);
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

    const std::string resp_str = generate_root_html(config, ip_addr, mac_addr);

    httpd_resp_send(req, resp_str.c_str(), resp_str.length());
    return ESP_OK;
}

static esp_err_t config_get_handler(httpd_req_t *req) {
    ESP_LOGD(TAG, "Entering config_get_handler");

    antenna_switch_config_t config;
    if (const esp_err_t ret = antenna_switch_get_config(&config); ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get configuration: %s", esp_err_to_name(ret));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to get configuration");
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Configuration retrieved successfully");
    ESP_LOGD(TAG, "Number of bands: %d, Number of antenna ports: %d", config.num_bands, config.num_antenna_ports);

    if (config.num_bands <= 0 || config.num_bands > MAX_BANDS) {
        ESP_LOGE(TAG, "Invalid number of bands: %d (should be between 1 and %d)", config.num_bands, MAX_BANDS);

        // Reset to a valid number of bands (e.g., 1)
        config.num_bands = 1;
        ESP_LOGD(TAG, "Resetting number of bands to %d", config.num_bands);

        // Save the corrected configuration to NVS
        if (const esp_err_t save_ret = antenna_switch_set_config(&config); save_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save corrected configuration: %s", esp_err_to_name(save_ret));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save corrected configuration");
            return ESP_FAIL;
        }

        ESP_LOGD(TAG, "Corrected configuration saved successfully");
    }
    if (config.num_antenna_ports == 0) {
        ESP_LOGW(TAG, "Configuration was not set");
          config.num_antenna_ports = 1;

      }

    if (config.num_antenna_ports <= 0 || config.num_antenna_ports > MAX_ANTENNA_PORTS) {
          config.num_antenna_ports = 1;
          ESP_LOGE(TAG, "Invalid number of antenna ports: %d (should be between 1 and %d)", config.num_antenna_ports,
                 MAX_ANTENNA_PORTS);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Invalid configuration: number of antenna ports out of range");
        return ESP_FAIL;
    }

    const std::string resp_str = generate_config_html(config);
    if (resp_str.empty()) {
        ESP_LOGE(TAG, "Failed to generate HTML");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to generate HTML");
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "HTML generated successfully, length: %d", resp_str.length());

    ESP_LOGD(TAG, "Sending response");
    if (const esp_err_t send_ret = httpd_resp_send(req, resp_str.c_str(), resp_str.length()); send_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send response: %s", esp_err_to_name(send_ret));
        return ESP_FAIL;
    }

    ESP_LOGV(TAG, "Response sent successfully");
    return ESP_OK;
}

static int find_active_port(const uint32_t current_freq, const antenna_switch_config_t &config, int &active_antenna) {
    active_antenna = 0;
    for (int i = 0; i < config.num_bands; i++) {
        if (current_freq >= config.bands[i].start_freq &&
            current_freq <= config.bands[i].end_freq) {
            // Find first enabled antenna port for this band
            for (int j = 0; j < config.num_antenna_ports; j++) {
                if (config.bands[i].antenna_ports[j]) {
                    // Convert to 1-based index (this is because antenna port selection starts at 1)
                    active_antenna = j + 1;
                    break;
                }
            }
            break;
        }
    }
    return active_antenna;
}

static esp_err_t status_get_handler(httpd_req_t *req) {
    // Get current frequency from CAT parser
    const uint32_t current_freq = cat_parser_get_frequency();

    const bool is_transmitting = cat_parser_get_transmit();

    // Get current antenna configuration
    antenna_switch_config_t config;
    if (const esp_err_t ret = antenna_switch_get_config(&config); ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get configuration: %s", esp_err_to_name(ret));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to get configuration");
        return ESP_FAIL;
    }

    // Find which antenna is active for the current frequency
    int active_antenna = find_active_port(current_freq, config, active_antenna);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "frequency", current_freq);
    cJSON_AddStringToObject(root, "antenna", active_antenna ? 
        ("Antenna " + std::to_string(active_antenna)).c_str() : "None");
    cJSON_AddBoolToObject(root, "transmitting", is_transmitting);  // Add transmitting state

    char *json_string = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_string);

    free(json_string);
    cJSON_Delete(root);
    return ESP_OK;
}

static constexpr httpd_uri_t status = {
        .uri       = "/status",
        .method    = HTTP_GET,
        .handler   = status_get_handler,
        .user_ctx  = nullptr
};

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

static esp_err_t config_post_handler(httpd_req_t *req) {
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
    cJSON *root = cJSON_Parse(content);
    if (!root) {
        free(content);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    // Parse configuration from JSON
    const cJSON *auto_mode = cJSON_GetObjectItem(root, "auto_mode");
    new_config.auto_mode = cJSON_IsTrue(auto_mode);


    // Parse UART configuration
    // Log the raw JSON content for debugging
    // ESP_LOGI(TAG, "Received config JSON: %s", content);

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

    // Get num_bands and num_antenna_ports from JSON
    const cJSON *num_bands_json = cJSON_GetObjectItem(root, "num_bands");
    if (!cJSON_IsNumber(num_bands_json)) {
        ESP_LOGE(TAG, "Number of bands not specified or invalid");
        cJSON_Delete(root);
        free(content);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid or missing num_bands");
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

    if (const cJSON *bands = cJSON_GetObjectItem(root, "bands"); cJSON_IsArray(bands)) {
        const int num_bands1 = cJSON_GetArraySize(bands);
        new_config.num_bands = num_bands1;

        for (int i = 0; i < num_bands1 && i < MAX_BANDS; i++) {
            if (cJSON const *band = cJSON_GetArrayItem(bands, i); cJSON_IsObject(band)) {
                if (const cJSON *description = cJSON_GetObjectItem(band, "description"); cJSON_IsString(description)) {
                    if (auto it = band_info.find(description->valuestring); it != band_info.end()) {
                        strncpy(new_config.bands[i].description, it->second.name,
                               sizeof(new_config.bands[i].description) - 1);
                        new_config.bands[i].start_freq = it->second.start_freq;
                        new_config.bands[i].end_freq = it->second.end_freq;
                        ESP_LOGV(TAG, "Setting band %d: %s (%lu-%lu Hz)", i,
                                new_config.bands[i].description,
                                new_config.bands[i].start_freq,
                                new_config.bands[i].end_freq);
                    } else {
                        ESP_LOGW(TAG, "Unknown band description: %s", description->valuestring);
                    }
                } else {
                    ESP_LOGW(TAG, "Missing or invalid band description for band %d", i);
                }

                if (const cJSON *antenna_ports = cJSON_GetObjectItem(band, "antenna_ports"); cJSON_IsArray(antenna_ports)) {
                    const int num_ports = cJSON_GetArraySize(antenna_ports);
                    for (int j = 0; j < num_ports && j < MAX_ANTENNA_PORTS; j++) {
                        const cJSON *port = cJSON_GetArrayItem(antenna_ports, j);
                        new_config.bands[i].antenna_ports[j] = cJSON_IsTrue(port);
                    }
                }
            }
        }
    }
    
    cJSON_Delete(root);

    esp_err_t err = antenna_switch_set_config(&new_config);
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

static esp_err_t toggle_auto_mode_handler(httpd_req_t *req) {
    antenna_switch_config_t config;
    esp_err_t ret = antenna_switch_get_config(&config);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to get configuration");
        return ESP_FAIL;
    }

    config.auto_mode = !config.auto_mode;

    ret = antenna_switch_set_config(&config);
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

static esp_err_t reset_config_handler(httpd_req_t *req) {
    constexpr antenna_switch_config_t default_config = {
        .auto_mode = true,
        .num_bands = 10,
        .num_antenna_ports = 1,
        .bands = {
            {.description = "160m", .start_freq = 1800000, .end_freq = 2000000, .antenna_ports = {true}},
            {.description = "80m", .start_freq = 3500000, .end_freq = 4000000, .antenna_ports = {true}},
            {.description = "40m", .start_freq = 7000000, .end_freq = 7300000, .antenna_ports = {true}},
            {.description = "30m", .start_freq = 10100000, .end_freq = 10150000, .antenna_ports = {true}},
            {.description = "20m", .start_freq = 14000000, .end_freq = 14350000, .antenna_ports = {true}},
            {.description = "17m", .start_freq = 18068000, .end_freq = 18168000, .antenna_ports = {true}},
            {.description = "15m", .start_freq = 21000000, .end_freq = 21450000, .antenna_ports = {true}},
            {.description = "12m", .start_freq = 24890000, .end_freq = 24990000, .antenna_ports = {true}},
            {.description = "10m", .start_freq = 28000000, .end_freq = 29700000, .antenna_ports = {true}},
            {.description = "6m", .start_freq = 50000000, .end_freq = 54000000, .antenna_ports = {true}},
        },
        .uart_baud_rate = 9600,
        .uart_parity = UART_PARITY_DISABLE,
        .uart_stop_bits = 1,
        .uart_flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };

    if (const esp_err_t ret = antenna_switch_set_config(&default_config); ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to reset configuration");
        return ESP_FAIL;
    }

    // Redirect back to the config page
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/config");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

static esp_err_t restart_handler(httpd_req_t *req) {
    ESP_LOGD(TAG, "Handling restart request");

    // Send a success response before restarting
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Restarting...");

    // Small delay to allow the response to be sent
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // Restart the device
    antenna_switch_restart();

    return ESP_OK;
}

static esp_err_t reset_wifi_handler(httpd_req_t *req) {
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

static esp_err_t relay_status_handler(httpd_req_t *req) {
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

static esp_err_t relay_control_handler(httpd_req_t *req) {
    char buf[32];
    int ret = httpd_req_recv(req, buf, MIN(req->content_len, sizeof(buf) - 1));
    if (ret <= 0) {
        ESP_LOGE(TAG, "Failed to receive relay control request");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    // ESP_LOGI(TAG, "Received relay control request: %s", buf);

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

    ESP_LOGD(TAG, "Setting relay %d to state %d", relay_num, relay_state);

    if (const esp_err_t err = RelayController::instance().set_relay(relay_num, relay_state); err != ESP_OK) {
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

static constexpr httpd_uri_t reset_wifi = {
        .uri       = "/reset-wifi",
        .method    = HTTP_POST,
        .handler   = reset_wifi_handler,
        .user_ctx  = nullptr
};

static constexpr httpd_uri_t config_post = {
        .uri       = "/config",
        .method    = HTTP_POST,
        .handler   = config_post_handler,
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

esp_err_t webserver_init() {
    config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192; //KB //32768;
    config.max_uri_handlers = 12;
    config.max_resp_headers = 8;
    config.lru_purge_enable = true;  // Enable LRU purging for large requests
    config.recv_wait_timeout = 10;
    config.uri_match_fn = httpd_uri_match_wildcard;

    char ip_addr[16];
    esp_err_t ret = WifiManager::instance().get_ip_info(ip_addr, sizeof(ip_addr));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get IP address: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Starting server on IP: %s, port: '%d'", ip_addr, config.server_port);
    ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error starting server: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGD(TAG, "Registering URI handlers");
    ret = httpd_register_uri_handler(server, &root);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register root URI handler: %s", esp_err_to_name(ret));
        goto error_handler;
    }

    ret = httpd_register_uri_handler(server, &config_get);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register config GET URI handler: %s", esp_err_to_name(ret));
        goto error_handler;
    }

    ret = httpd_register_uri_handler(server, &config_post);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register config POST URI handler: %s", esp_err_to_name(ret));
        goto error_handler;
    }

    ret = httpd_register_uri_handler(server, &status);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register status URI handler: %s", esp_err_to_name(ret));
        goto error_handler;
    }

    ret = httpd_register_uri_handler(server, &toggle_auto_mode);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register toggle auto mode URI handler: %s", esp_err_to_name(ret));
        goto error_handler;
    }

    ret = httpd_register_uri_handler(server, &reset_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register reset config URI handler: %s", esp_err_to_name(ret));
        goto error_handler;
    }

    ret = httpd_register_uri_handler(server, &restart);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register restart URI handler: %s", esp_err_to_name(ret));
        goto error_handler;
    }

    ret = httpd_register_uri_handler(server, &reset_wifi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register reset wifi URI handler: %s", esp_err_to_name(ret));
        goto error_handler;
    }

    ret = httpd_register_uri_handler(server, &relay_status);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register relay status handler: %s", esp_err_to_name(ret));
        goto error_handler;
    }

    ret = httpd_register_uri_handler(server, &relay_control);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register relay control handler: %s", esp_err_to_name(ret));
        goto error_handler;
    }

    ESP_LOGI(TAG, "Server started successfully");
    return ESP_OK;

    error_handler:
    ESP_LOGE(TAG, "Error occurred during server initialization. Stopping server.");
    webserver_stop();
    return ret;
}


esp_err_t webserver_start() {
    if (server == nullptr) {
        ESP_LOGD(TAG, "Starting normal webserver");

        if (const esp_err_t ret = webserver_init(); ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize webserver: %s", esp_err_to_name(ret));
            return ret;
        }
    }
    return ESP_OK;
}

esp_err_t webserver_stop() {
    if (server) {
        ESP_LOGD(TAG, "Stopping webserver");
        httpd_stop(server);
        server = nullptr;
    }
    return ESP_OK;
}

esp_err_t webserver_restart() {
    ESP_LOGD(TAG, "Restarting webserver");
    webserver_stop();
    return webserver_start();
}
