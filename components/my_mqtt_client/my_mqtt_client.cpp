#include "my_mqtt_client.h"
#include <cstring>
#include <utility>
#include "esp_log.h"
#include "esp_err.h"
#include "cat_parser.h"
#include "config_manager.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/err.h"
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

// Ensure TAG is defined for logging
static constexpr const char* TAG = "MQTTClient";

MQTTClient &MQTTClient::instance() {
    static MQTTClient instance;
    return instance;
}

MQTTClient::MQTTClient()
    : client_(nullptr)
      , current_frequency_(0)
      , is_transmitting_(false) // Initialize all members
      , has_serial_data_(false) {
}

esp_err_t MQTTClient::init() {
    const auto &config = get_cached_config();

    if (!config.mqtt_enabled) {
        ESP_LOGI(TAG, "MQTT is disabled in configuration.");
        if (client_) {
            ESP_LOGI(TAG, "MQTT client was previously initialized, now de-initializing as MQTT is disabled.");

            if (const esp_err_t destroy_err = esp_mqtt_client_destroy(client_); destroy_err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to destroy MQTT client: %s", esp_err_to_name(destroy_err));
                // Log error but continue, as the goal is to be in a disabled state.
            }
            client_ = nullptr;
        }
        return ESP_OK; // Successfully handled disabled state
    }

    // MQTT is enabled in configuration.
    if (client_) {
        ESP_LOGI(TAG, "MQTT client already exists. Destroying and re-initializing to apply current configuration.");
        esp_err_t destroy_err = esp_mqtt_client_destroy(client_);
        if (destroy_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to destroy existing MQTT client during re-init: %s. Attempting to create new instance anyway.", esp_err_to_name(destroy_err));
        }
        client_ = nullptr; // Mark as null before re-initializing
    }

    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.uri = config.mqtt_broker;
    mqtt_cfg.broker.address.port = config.mqtt_port;
    mqtt_cfg.credentials.username = config.mqtt_username;
    mqtt_cfg.credentials.authentication.password = config.mqtt_password;
    mqtt_cfg.credentials.client_id = config.mqtt_client_id;
    mqtt_cfg.buffer.size = 1024;
    mqtt_cfg.buffer.out_size = 1024;
    mqtt_cfg.network.disable_auto_reconnect = false;
    mqtt_cfg.network.timeout_ms = 10000;
    mqtt_cfg.task.priority = 5;
    mqtt_cfg.task.stack_size = 8192;

    ESP_LOGI(TAG, "Initializing MQTT client with broker: %s, port: %d, username: %s, client_id: %s",
             config.mqtt_broker, config.mqtt_port, config.mqtt_username, config.mqtt_client_id);

    client_ = esp_mqtt_client_init(&mqtt_cfg);
    if (!client_) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return ESP_FAIL;
    }

    const esp_err_t err = esp_mqtt_client_register_event(client_,
                                                         static_cast<esp_mqtt_event_id_t>(ESP_EVENT_ANY_ID),
                                                         mqtt_event_handler,
                                                         this);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register MQTT event handler: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(client_); // Clean up partially initialized client
        client_ = nullptr;
        return err;
    }

    ESP_LOGI(TAG, "MQTT client initialized successfully.");
    return ESP_OK;
}

bool MQTTClient::check_broker_connectivity() const {
    const auto &config = get_cached_config();
    if (!config.mqtt_enabled) {
        return false;
    }

    // Parse broker address to extract hostname and port
    const char* broker_uri = config.mqtt_broker;
    char hostname[128] = {0};
    int port = config.mqtt_port;

    // Extract hostname from URI (handle mqtt:// or tcp:// prefixes)
    const char* host_start = broker_uri;
    if (strncmp(broker_uri, "mqtt://", 7) == 0) {
        host_start = broker_uri + 7;
    } else if (strncmp(broker_uri, "tcp://", 6) == 0) {
        host_start = broker_uri + 6;
    }

    // Copy hostname (up to : or end of string)
    const char* port_start = strchr(host_start, ':');
    size_t hostname_len;
    if (port_start) {
        hostname_len = port_start - host_start;
        // Extract port number after :
        port = atoi(port_start + 1);
    } else {
        hostname_len = strlen(host_start);
        // Use default MQTT port if not specified and we have a prefix
        if (port == 0) {
            port = 1883;
        }
    }
    
    if (hostname_len >= sizeof(hostname)) {
        ESP_LOGE(TAG, "Hostname too long in broker URI: %s", broker_uri);
        return false;
    }
    
    strncpy(hostname, host_start, hostname_len);
    hostname[hostname_len] = '\0';

    ESP_LOGD(TAG, "Checking connectivity to %s:%d", hostname, port);

    // Create socket
    int sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        ESP_LOGW(TAG, "Failed to create socket for connectivity check");
        return false;
    }

    // Set socket to non-blocking mode for timeout control
    int flags = lwip_fcntl(sock, F_GETFL, 0);
    if (lwip_fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        ESP_LOGW(TAG, "Failed to set socket to non-blocking mode");
        lwip_close(sock);
        return false;
    }

    // Resolve hostname using thread-safe getaddrinfo
    struct addrinfo hints = {};
    struct addrinfo *addr_result = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

    int gai_err = lwip_getaddrinfo(hostname, port_str, &hints, &addr_result);
    if (gai_err != 0 || !addr_result) {
        ESP_LOGD(TAG, "DNS resolution failed for %s: %d", hostname, gai_err);
        lwip_close(sock);
        return false;
    }

    // Setup address structure from resolved address
    struct sockaddr_in dest_addr;
    memcpy(&dest_addr, addr_result->ai_addr, sizeof(dest_addr));
    lwip_freeaddrinfo(addr_result);

    // Attempt connection
    int connect_result = lwip_connect(sock, (struct sockaddr*)&dest_addr, sizeof(dest_addr));
    bool is_connected = false;

    if (connect_result == 0) {
        // Connected immediately
        is_connected = true;
    } else if (errno == EINPROGRESS) {
        // Connection in progress, wait for completion with timeout
        fd_set write_set;
        struct timeval timeout;

        FD_ZERO(&write_set);
        FD_SET(sock, &write_set);
        timeout.tv_sec = 2;  // 2 second timeout
        timeout.tv_usec = 0;

        int select_result = lwip_select(sock + 1, NULL, &write_set, NULL, &timeout);
        if (select_result > 0) {
            // Check if connection actually succeeded
            int error = 0;
            socklen_t len = sizeof(error);
            if (lwip_getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error == 0) {
                is_connected = true;
            }
        }
    }

    lwip_close(sock);

    if (is_connected) {
        ESP_LOGD(TAG, "Connectivity check passed for %s:%d", hostname, port);
    } else {
        ESP_LOGD(TAG, "Connectivity check failed for %s:%d", hostname, port);
    }

    return is_connected;
}

esp_err_t MQTTClient::connect() const {
    if (const auto &config = get_cached_config(); !config.mqtt_enabled) {
        ESP_LOGI(TAG, "MQTT is disabled in configuration. Not attempting to connect.");

        // If client_ is not null here, init() should have ideally cleaned it up.
        // This check primarily prevents starting a connection if config says disabled.
        return ESP_OK; // Considered success as per disabled state. Or return ESP_ERR_INVALID_STATE.
    }

    if (!client_) {
        ESP_LOGE(TAG, "MQTT client not initialized, but MQTT is enabled in config. Call init() first.");
        return ESP_ERR_INVALID_STATE;
    }

    // Perform connectivity check before attempting MQTT connection
    if (!check_broker_connectivity()) {
        ESP_LOGW(TAG, "Broker connectivity check failed, skipping MQTT connection attempt");
        return ESP_ERR_NOT_FOUND; // Broker not reachable
    }

    ESP_LOGI(TAG, "Broker connectivity confirmed, attempting MQTT connection...");
    if (const esp_err_t err = esp_mqtt_client_start(client_); err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "MQTT client start command issued."); // esp_mqtt_client_start is non-blocking
    return ESP_OK;
}

esp_err_t MQTTClient::subscribe_to_omnirig_topics() const {
    const auto &config = get_cached_config();
    if (!config.mqtt_enabled || !client_) {
        ESP_LOGW(TAG, "MQTT not enabled or client not initialized. Cannot subscribe to topics.");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Attempting to subscribe to topic: %s", config.mqtt_topic);
    if (const int msg_id = esp_mqtt_client_subscribe(client_, config.mqtt_topic, 1); msg_id < 0) {
        ESP_LOGE(TAG, "Failed to subscribe to radio info topic");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Successfully subscribed to radio info topic: %s", config.mqtt_topic);
    return ESP_OK;
}

void MQTTClient::handle_radio_info(const char *data, int data_len) {
    cJSON *root = cJSON_ParseWithLength(data, data_len);
    if (root == nullptr) {
        ESP_LOGE(TAG, "Failed to parse JSON from MQTT data");
        return;
    }

    parse_radio_info(root);
    cJSON_Delete(root);
}

void MQTTClient::parse_radio_info(const cJSON *json) {
    // This check is for a different purpose (data source priority)
    if (const auto &config = get_cached_config();
        !config.allow_concurrent_data_sources && has_serial_data_.load()) {
        ESP_LOGD(TAG, "MQTT data received but serial data has priority and concurrent sources disallowed.");
        return;
    }

    // Get all needed values at once to reduce JSON operations
    const cJSON *freq = cJSON_GetObjectItem(json, "Freq");

    // Handle transmit state first as it's more time-critical
    if (const cJSON *tx = cJSON_GetObjectItem(json, "IsTransmitting"); cJSON_IsBool(tx)) {
        if (const bool new_tx_state = cJSON_IsTrue(tx); new_tx_state != is_transmitting_.load()) {
            is_transmitting_.store(new_tx_state);
            cat_parser_set_transmit(new_tx_state); // Update CAT parser immediately
            ESP_LOGI(TAG, "Transmit state changed via MQTT to: %s", new_tx_state ? "true" : "false");
        }
    }

    if (freq && freq->valueint > 0) {
        if (const uint32_t new_freq = freq->valueint; new_freq != current_frequency_.load()) {
            current_frequency_.store(new_freq);
            if (frequency_callback_) {
                frequency_callback_(new_freq);
            }
            ESP_LOGV(TAG, "Frequency changed via MQTT to: %lu", new_freq);
            cat_parser_set_frequency(new_freq);
        }
    }
}

esp_err_t MQTTClient::publish_message(const char *topic, const char *message) const {
    if (const auto &config = get_cached_config(); !config.mqtt_enabled || !client_) {
        ESP_LOGW(TAG, "MQTT not enabled or client not initialized. Cannot publish message.");
        return ESP_ERR_INVALID_STATE;
    }

    if (const int msg_id = esp_mqtt_client_publish(client_, topic, message, 0, 1, 0); msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish message to topic: %s", topic);
        return ESP_FAIL;
    }
    ESP_LOGV(TAG, "Published MQTT message to topic: %s", topic);
    return ESP_OK;
}

void MQTTClient::set_frequency_callback(std::function<void(uint32_t)> callback) {
    frequency_callback_ = std::move(callback);
}

void MQTTClient::set_has_serial_data(const bool has_data) {
    has_serial_data_.store(has_data);
}

uint32_t MQTTClient::get_current_frequency() const {
    return current_frequency_.load();
}

void MQTTClient::mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    const auto client_ptr = static_cast<MQTTClient *>(handler_args);
    if (!client_ptr) {
        ESP_LOGE(TAG, "MQTT event handler called with null client context");
        return;
    }

    switch (const auto event = static_cast<esp_mqtt_event_handle_t>(event_data); event->event_id) {
        case MQTT_EVENT_CONNECTED: {
            ESP_LOGI(TAG, "MQTT Connected to broker");
            // Check config again, in case it changed to disabled just before connection completed.
            if (const auto& current_config = client_ptr->get_cached_config(); current_config.mqtt_enabled && client_ptr->client_) {
                 if (const esp_err_t err = client_ptr->subscribe_to_omnirig_topics(); err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to subscribe to MQTT topics after connection.");
                }
            } else {
                ESP_LOGW(TAG, "MQTT_EVENT_CONNECTED received, but MQTT is now disabled or client is null. Not subscribing.");
            }
            break;
        }

        case MQTT_EVENT_DISCONNECTED: {
            if (const auto& current_config = client_ptr->get_cached_config(); current_config.mqtt_enabled) {
                ESP_LOGI(TAG, "MQTT Disconnected from broker.");
            } else {
                ESP_LOGD(TAG, "MQTT Disconnected event received, MQTT is disabled in config.");
            }
            break;
        }
        
        case MQTT_EVENT_DATA:
            ESP_LOGV(TAG, "Received MQTT data on topic: %.*s", event->topic_len, event->topic);
            ESP_LOGV(TAG, "Data: %.*s", event->data_len, event->data);

            // Ensure client_ptr->client_ is valid before processing, though if we get DATA, it should be.
            if (client_ptr->client_) {
                 client_ptr->handle_radio_info(event->data, event->data_len);
            } else {
                ESP_LOGW(TAG, "MQTT_EVENT_DATA received, but internal client handle is null.");
            }
            break;

        default:
            ESP_LOGV(TAG, "MQTT event received: %d", event->event_id);
            break;
    }
}
