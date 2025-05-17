#include "my_mqtt_client.h"
#include <cstring>
#include <utility>
#include "esp_log.h"
#include "esp_err.h"
#include "cat_parser.h"

MQTTClient &MQTTClient::instance() {
    static MQTTClient instance;
    return instance;
}

MQTTClient::MQTTClient()
    : client_(nullptr)
      , current_frequency_(0) {
}

esp_err_t MQTTClient::init() {
    const auto &config = ConfigManager::instance().get_config();

    if (!config.mqtt_enabled) {
        ESP_LOGI(TAG, "MQTT is disabled in configuration");
        return ESP_OK;
    }

    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.uri = config.mqtt_broker;
    mqtt_cfg.broker.address.port = config.mqtt_port;
    mqtt_cfg.credentials.username = config.mqtt_username;
    mqtt_cfg.credentials.authentication.password = config.mqtt_password;
    mqtt_cfg.credentials.client_id = config.mqtt_client_id;
    mqtt_cfg.buffer.size = 1024; // Optimize buffer sizes
    mqtt_cfg.buffer.out_size = 1024;
    mqtt_cfg.network.disable_auto_reconnect = false;
    mqtt_cfg.network.timeout_ms = 10000;
    mqtt_cfg.task.priority = 5; // Increase MQTT task priority
    mqtt_cfg.task.stack_size = 6144;

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
        ESP_LOGE(TAG, "Failed to register MQTT event handler");
        return err;
    }

    return ESP_OK;
}

esp_err_t MQTTClient::connect() const {
    if (const esp_err_t err = esp_mqtt_client_start(client_); err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client");
        return err;
    }
    return ESP_OK;
}

esp_err_t MQTTClient::subscribe_to_omnirig_topics() const { // Removed unused rig_id parameter
    const auto &config = ConfigManager::instance().get_config();

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
        ESP_LOGE(TAG, "Failed to parse JSON");
        return;
    }

    parse_radio_info(root);
    cJSON_Delete(root);
}

void MQTTClient::parse_radio_info(const cJSON *json) {
    if (const auto &config = ConfigManager::instance().get_config();
        !config.allow_concurrent_data_sources && has_serial_data_) {
        return;
    }

    // Get all needed values at once to reduce JSON operations
    const cJSON *freq = cJSON_GetObjectItem(json, "Freq");
    const cJSON *tx = cJSON_GetObjectItem(json, "IsTransmitting");

    // Handle transmit state first as it's more time-critical
    if (cJSON_IsBool(tx)) {
        if (const bool new_tx_state = cJSON_IsTrue(tx); new_tx_state != is_transmitting_) {
            is_transmitting_ = new_tx_state;
            cat_parser_set_transmit(new_tx_state); // Update CAT parser immediately
            ESP_LOGI(TAG, "Transmit state changed via MQTT to: %s", new_tx_state ? "true" : "false");
        }
    }

    // Then handle frequency if needed
    if (freq && freq->valueint > 0) {
        if (const uint32_t new_freq = freq->valueint; new_freq != current_frequency_) {
            current_frequency_ = new_freq;
            if (frequency_callback_) {
                frequency_callback_(new_freq);
            }
            ESP_LOGI(TAG, "Frequency changed via MQTT to: %lu", new_freq);
            cat_parser_set_frequency(new_freq);
        }
    }
}

esp_err_t MQTTClient::publish_message(const char *topic, const char *message) const {
    if (const int msg_id = esp_mqtt_client_publish(client_, topic, message, 0, 1, 0); msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish message to topic: %s", topic);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void MQTTClient::set_frequency_callback(std::function<void(uint32_t)> callback) {
    frequency_callback_ = std::move(callback);
}

void MQTTClient::set_has_serial_data(const bool has_data) {
    has_serial_data_ = has_data;
}

uint32_t MQTTClient::get_current_frequency() const {
    return current_frequency_;
}

void MQTTClient::mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    const auto client = static_cast<MQTTClient *>(handler_args);

    switch (const auto event = static_cast<esp_mqtt_event_handle_t>(event_data); event->event_id) {
        case MQTT_EVENT_CONNECTED: {
            ESP_LOGI(TAG, "MQTT Connected to broker");

            if (const esp_err_t err = client->subscribe_to_omnirig_topics(); err != ESP_OK) { // Call updated: no rig_id
                ESP_LOGE(TAG, "Failed to subscribe to MQTT topics");
            }
            break;
        }

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT Disconnected from broker");
            break;

        case MQTT_EVENT_DATA:
            ESP_LOGD(TAG, "Received MQTT data on topic: %.*s", event->topic_len, event->topic);
            ESP_LOGD(TAG, "Data: %.*s", event->data_len, event->data);
            client->handle_radio_info(event->data, event->data_len);
            break;

        default:
            ESP_LOGD(TAG, "MQTT event received: %d", event->event_id);
            break;
    }
}
