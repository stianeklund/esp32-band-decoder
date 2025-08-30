#ifndef WEBSOCKET_HANDLERS_H
#define WEBSOCKET_HANDLERS_H

#include "esp_err.h"
#include "websocket_protocol.h"
#include "cJSON.h"

class WebSocketHandlers {
public:
    // Request handlers
    static esp_err_t handle_status_request(int sockfd, const char* request_id);
    static esp_err_t handle_relay_control_request(int sockfd, const char* request_id, const cJSON* data);
    static esp_err_t handle_antenna_switch_request(int sockfd, const char* request_id, const cJSON* data);
    static esp_err_t handle_config_get_request(int sockfd, const char* request_id);
    static esp_err_t handle_config_set_request(int sockfd, const char* request_id, const cJSON* data);
    static esp_err_t handle_relay_names_request(int sockfd, const char* request_id);
    static esp_err_t handle_config_basic_request(int sockfd, const char* request_id);
    static esp_err_t handle_subscribe_request(int sockfd, const char* request_id, const cJSON* data);
    static esp_err_t handle_unsubscribe_request(int sockfd, const char* request_id, const cJSON* data);

    // Response helpers
    static esp_err_t send_json_response(int sockfd, const char* request_id, const cJSON* data);
    static esp_err_t send_success_response(int sockfd, const char* request_id, const char* message = nullptr);
    static esp_err_t send_error_response(int sockfd, const char* request_id, const char* error_msg);

public:
    // Helper functions - made public for WebSocketServer friendship access
    static cJSON* create_status_json();
    static cJSON* create_relay_names_json();
    static cJSON* create_config_basic_json();

private:
    static const char* TAG;
    static esp_err_t validate_relay_control_data(const cJSON* data, int* relay_id, bool* state);
    static esp_err_t validate_antenna_switch_data(const cJSON* data, char* radio, char* action, int* antenna_number = nullptr);
};

#endif // WEBSOCKET_HANDLERS_H