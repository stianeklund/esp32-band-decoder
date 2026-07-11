#include "system_initializer.h"
#include "input_manager.h"

#include "antenna_switch.h"
#include "cat_parser.h"
#include "config_manager.h"
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_netif_types.h>
#include "my_mqtt_client.h"
#include <nvs.h>
#include <nvs_flash.h>
#include "wifi_manager.hpp"

#include "esp_check.h"
#include "esp_err.h"
#include "esp_task_wdt.h"

static constexpr const char* TAG = "SYSTEM_INIT";

static bool is_valid_ip() {
    esp_netif_ip_info_t ip_info;

    if (esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        return false;
    }
    
    // Check if we have a non-zero IP address
    return ip_info.ip.addr != 0;
}

esp_err_t SystemInitializer::init_nvs() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

esp_err_t SystemInitializer::init_task_watchdog() {
    // First try to delete any existing watchdog
    esp_task_wdt_deinit();
    vTaskDelay(pdMS_TO_TICKS(100));  // Add delay after deinit
    
    constexpr esp_task_wdt_config_t twdt_config = {
        .timeout_ms = 30000,      // Increase timeout to 30 seconds
        .idle_core_mask = (1 << 0), // Watch core 0
        .trigger_panic = false    // Don't trigger panic on timeout
    };

    esp_err_t ret = esp_task_wdt_init(&twdt_config);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to initialize watchdog: %s", esp_err_to_name(ret));
        return ret;
    }
    
    vTaskDelay(pdMS_TO_TICKS(100));  // Add delay after init
    
    // Subscribe the main task to the watchdog
    ret = esp_task_wdt_add(xTaskGetCurrentTaskHandle());
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to subscribe main task to watchdog: %s", esp_err_to_name(ret));
        return ret;
    }
    
    return ESP_OK;
}

esp_err_t SystemInitializer::initialize_basic() {
    // Initialize NVS first
    ESP_RETURN_ON_ERROR(init_nvs(), TAG, "Failed to initialize NVS");
    vTaskDelay(pdMS_TO_TICKS(100));  // Add delay after NVS init

    // Initialize event loop before WiFi
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "Failed to create event loop");
    vTaskDelay(pdMS_TO_TICKS(100));  // Add delay after event loop creation

    // Initialize WiFi with more robust error handling
    esp_err_t ret = WifiManager::instance().init();
    if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Failed to initialize WiFi manager: %s", esp_err_to_name(ret));
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(100));  // Add delay after WiFi init

    // Initialize watchdog last
    ESP_RETURN_ON_ERROR(init_task_watchdog(), TAG, "Failed to initialize watchdog");
    
    return ESP_OK;
}

esp_err_t SystemInitializer::initialize_full(RelayController** relay_controller_out) {
    // Initialize antenna switch configuration
    ESP_RETURN_ON_ERROR(AntennaSwitch::instance().init(), TAG, "Failed to initialize antenna switch");

    // Relay-dependent callbacks can arrive as soon as the input and CAT tasks start.
    // Initialize and attach the controller before either asynchronous task is created.
    auto& relay_controller = RelayController::instance();
    esp_err_t ret = relay_controller.init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize relay controller: %s", esp_err_to_name(ret));
        return ret;
    }
    *relay_controller_out = &relay_controller;
    AntennaSwitch::instance().set_relay_controller(*relay_controller_out);

    // Initialize input polling and CAT only after relay control is ready.
    ESP_RETURN_ON_ERROR(InputManager::instance().init(), TAG, "Failed to initialize input manager");
    ESP_RETURN_ON_ERROR(cat_parser_init(), TAG, "Failed to initialize CAT parser");
    
    // Request async AI mode probe (non-blocking, will be handled by UART task)
    ESP_LOGI(TAG, "Requesting async AI mode probe...");
    CatParser::instance().request_ai_probe();

    // Initialize MQTT client configuration (this does not connect yet).
    // The MQTTClient::init() method itself handles the case where MQTT might be disabled
    // in the configuration by not preparing a client or cleaning up an old one.
    ESP_LOGI(TAG, "Preparing MQTT client configuration...");
    if (esp_err_t mqtt_init_ret = MQTTClient::instance().init(); mqtt_init_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to prepare MQTT client configuration: %s", esp_err_to_name(mqtt_init_ret));
        // Depending on severity, you might want to return mqtt_init_ret here.
        // For now, we log and continue, as MQTT might not be essential for all operations.
    } else {
        ESP_LOGI(TAG, "MQTT client configuration prepared.");
    }

    ESP_LOGI(TAG, "Main task stack high-water mark after device init: %u bytes",
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));

    // --- Wait for Network Connectivity ---
    bool ip_obtained = is_valid_ip(); // Check initial state

    if (!ip_obtained) {
        ESP_LOGI(TAG, "Network not immediately available. Waiting for IP address...");
        constexpr int MAX_IP_WAIT_MS = 30000; // 30 seconds timeout
        int waited_ms = 0;
        
        while (!is_valid_ip() && waited_ms < MAX_IP_WAIT_MS) {
            constexpr int IP_CHECK_INTERVAL_MS = 500;
            vTaskDelay(pdMS_TO_TICKS(IP_CHECK_INTERVAL_MS));
            waited_ms += IP_CHECK_INTERVAL_MS;
            
            // Feed the watchdog during this potentially long wait
            if (const esp_err_t wdt_status = esp_task_wdt_status(xTaskGetCurrentTaskHandle());
                wdt_status == ESP_OK) {
                esp_task_wdt_reset();
            }
            else if (wdt_status == ESP_ERR_NOT_FOUND) {
                ESP_LOGW(TAG, "Main task not subscribed to WDT during network wait!");
            }
            
            if (waited_ms % 2000 == 0) { // Log every 2 seconds
                ESP_LOGI(TAG, "Waiting to confirm network connectivity.. %d/%d ms",
                         waited_ms, MAX_IP_WAIT_MS);
            }
        }
        ip_obtained = is_valid_ip(); // Check final state after waiting
    }

    // --- Network Dependent Initializations (like MQTT connection) ---
    if (ip_obtained) {
        ESP_LOGI(TAG, "Valid IP address obtained.");
        // Give network stack a moment to stabilize further if needed
        vTaskDelay(pdMS_TO_TICKS(1000)); 

        esp_netif_ip_info_t ip_info_log;
        if (esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            netif != nullptr && esp_netif_get_ip_info(netif, &ip_info_log) == ESP_OK) {
            ESP_LOGI(TAG, "Device IP: " IPSTR, IP2STR(&ip_info_log.ip));
        }

        // Attempt to connect MQTT if it's enabled in the configuration
        if (ConfigManager::instance().is_mqtt_enabled()) {
            ESP_LOGI(TAG, "MQTT is enabled. Attempting to connect MQTT client...");
            // MQTTClient::connect() will internally check WifiManager::is_connected(),
            // initialize the client with esp_mqtt_client_init, register events, and start.
            esp_err_t mqtt_connect_ret = MQTTClient::instance().connect();
            if (mqtt_connect_ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to initiate MQTT client connection: %s", esp_err_to_name(mqtt_connect_ret));
                // ESP_ERR_NETWORK_DOWN is a possible return if WifiManager says not connected,
                // or if MQTTClient::init() hadn't successfully prepared the config.
            } else {
                ESP_LOGI(TAG, "MQTT client connection process initiated. Subscriptions will occur upon successful connection via event handler.");
                // The actual subscription to topics is handled within the MQTTClient's 
                // event handler upon receiving MQTT_EVENT_CONNECTED.
            }
        } else {
            ESP_LOGI(TAG, "MQTT is disabled in configuration. Skipping MQTT connection attempt.");
        }
    } else {
        ESP_LOGW(TAG, "Timeout waiting for valid IP address or IP not available.");
        if (ConfigManager::instance().is_mqtt_enabled()) {
            ESP_LOGW(TAG, "MQTT is enabled in config but will not connect due to lack of IP.");
            // MQTTClient::init() was already called earlier, so config is loaded.
            // No further action needed here for MQTT if IP is not obtained.
        }
    }

    // Yield to other tasks before returning
    vTaskDelay(pdMS_TO_TICKS(100));

    return ESP_OK;
}
