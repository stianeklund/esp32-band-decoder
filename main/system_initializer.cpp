#include "system_initializer.h"

#include <antenna_switch.h>
#include <cat_parser.h>
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
    constexpr esp_task_wdt_config_t twdt_config = {
        .timeout_ms = 5000,      // 5 second timeout
        .idle_core_mask = 0, // No idle core deactivation
        .trigger_panic = false // Don't trigger panic on timeout
    };

    if (const esp_err_t ret = esp_task_wdt_init(&twdt_config); ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to initialize watchdog: %s", esp_err_to_name(ret));
        return ret;
    }
    return ESP_OK;
}

esp_err_t SystemInitializer::initialize_basic() {
    ESP_RETURN_ON_ERROR(init_nvs(), TAG, "Failed to initialize NVS");
    ESP_RETURN_ON_ERROR(init_task_watchdog(), TAG, "Failed to initialize watchdog");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "Failed to create event loop");
    ESP_RETURN_ON_ERROR(WifiManager::instance().init(), TAG, "Failed to initialize WiFi manager");
    return ESP_OK;
}

esp_err_t SystemInitializer::initialize_full(RelayController** relay_controller_out) {
    // Initialize antenna switch configuration
    ESP_RETURN_ON_ERROR(antenna_switch_init(), TAG, "Failed to initialize antenna switch");

    // Get the configuration
    antenna_switch_config_t config;
    ESP_RETURN_ON_ERROR(antenna_switch_get_config(&config), TAG,
                        "Failed to get antenna switch configuration");

    // Create relay controller
    auto relay_controller = std::make_unique<RelayController>();
    
    // Configure TCP settings if host is specified
    if (strlen(config.tcp_host) > 0) {
        relay_controller->set_tcp_host(config.tcp_host);
        relay_controller->set_tcp_port(config.tcp_port);
        
        ESP_LOGI(TAG, "Waiting for valid IP address...");
        
        // Wait up to 30 seconds for valid IP
        constexpr int MAX_IP_WAIT_MS = 30000;
        int waited_ms = 0;
        
        while (!is_valid_ip() && waited_ms < MAX_IP_WAIT_MS) {

            constexpr int IP_CHECK_INTERVAL_MS = 500;

            vTaskDelay(pdMS_TO_TICKS(IP_CHECK_INTERVAL_MS));
            waited_ms += IP_CHECK_INTERVAL_MS;
            
            if (waited_ms % 1000 == 0) {
                ESP_LOGI(TAG, "Waiting for IP address... %d/%d ms", 
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
                ESP_LOGI(TAG, "Device IP: " IPSTR ", TCP Host: %s",
                         IP2STR(&ip_info.ip), config.tcp_host);
                ESP_LOGI(TAG, "WiFi connected, initializing TCP connection to %s:%d",
                         config.tcp_host, config.tcp_port);
            }

            if (const esp_err_t ret = relay_controller->init(); ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to initialize relay controller: %s. Continuing without TCP connection", 
                         esp_err_to_name(ret));
                // Don't return error - continue with initialization
            }
        }
    } else {
        ESP_LOGW(TAG, "TCP host not configured. Continuing without TCP connection");
    }

    // Initialize CAT parser after relay controller is ready
    ESP_RETURN_ON_ERROR(cat_parser_init(), TAG, "Failed to initialize CAT parser");

    // Set the relay controller in antenna switch
    *relay_controller_out = relay_controller.get();
    antenna_switch_set_relay_controller(std::move(relay_controller));
    return ESP_OK;
}
