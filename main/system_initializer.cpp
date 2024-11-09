#include "system_initializer.h"

#include <antenna_switch.h>
#include <cat_parser.h>
#include "cat_parser.h"
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_netif_types.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <wifi_manager.hpp>

#include "esp_check.h"
#include "esp_err.h"
#include "esp_task_wdt.h"

static auto TAG = "SYSTEM_INIT";

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
    ESP_RETURN_ON_ERROR(antenna_switch_init(), TAG, "Failed to initialize antenna switch");

    // Get the configuration
    antenna_switch_config_t config;
    ESP_RETURN_ON_ERROR(antenna_switch_get_config(&config), TAG,
                        "Failed to get antenna switch configuration");

    // Initialize CAT parser
    ESP_RETURN_ON_ERROR(cat_parser_init(), TAG, "Failed to initialize CAT parser");

    // Get relay controller singleton instance and initialize it immediately
    auto& relay_controller = RelayController::instance();
    esp_err_t ret = relay_controller.init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize relay controller: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Set the relay controller in antenna switch
    *relay_controller_out = &relay_controller;
    antenna_switch_set_relay_controller(*relay_controller_out);

    // Now wait for network if needed
    if (!is_valid_ip()) {
        constexpr int MAX_IP_WAIT_MS = 30000;
        int waited_ms = 0;
        
        while (!is_valid_ip() && waited_ms < MAX_IP_WAIT_MS) {
            constexpr int IP_CHECK_INTERVAL_MS = 500;
            vTaskDelay(pdMS_TO_TICKS(IP_CHECK_INTERVAL_MS));
            waited_ms += IP_CHECK_INTERVAL_MS;
            
            if (waited_ms % 1000 == 0) {
                ESP_LOGI(TAG, "Waiting to confirm network connectivity.. %d/%d ms",
                         waited_ms, MAX_IP_WAIT_MS);
            }
        }

        if (!is_valid_ip()) {
            ESP_LOGW(TAG, "Timeout waiting for valid IP address. Continuing without TCP connection");
        } else {
            // Give network stack additional time to stabilize
            vTaskDelay(pdMS_TO_TICKS(2000));

            // Log network information for debugging
            esp_netif_ip_info_t ip_info;
            if (esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                ESP_LOGI(TAG, "Device IP:" IPSTR, IP2STR(&ip_info.ip));
            }
        }
    }

    // Set the relay controller in antenna switch
    *relay_controller_out = &relay_controller;
    antenna_switch_set_relay_controller(*relay_controller_out);
    return ESP_OK;
}
