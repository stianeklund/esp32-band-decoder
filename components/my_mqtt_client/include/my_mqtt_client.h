#pragma once

#include <functional>
#include "cJSON.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "mqtt_client.h"

class MQTTClient {
public:
    static MQTTClient& instance();
    esp_err_t init();
    esp_err_t connect() const;
    esp_err_t publish_message(const char* topic, const char* message) const;
    void set_frequency_callback(std::function<void(uint32_t)> callback);
    esp_err_t subscribe_to_omnirig_topics() const; // Removed unused rig_id parameter
    void handle_radio_info(const char* data, int data_len);
    void parse_radio_info(const cJSON* json);
    void set_has_serial_data(bool has_data);
    [[nodiscard]] uint32_t get_current_frequency() const;
    [[nodiscard]] bool is_transmitting() const { return is_transmitting_; }

private:
    MQTTClient();
    static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
    
    esp_mqtt_client_handle_t client_;
    std::function<void(uint32_t)> frequency_callback_;
    static constexpr char TAG[] = "MQTTClient";
    uint32_t current_frequency_;
    bool is_transmitting_{false};
    bool has_serial_data_{false};
};
