#include <esp_event.h>
#include <esp_task_wdt.h>
#include <memory>
#include <nvs_flash.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "restart_manager.h"
#include "system_initializer.h"
#include "relay_controller.h"
#include "wifi_manager.hpp"

static auto TAG = "MAIN";

// Global pointer to RelayController
static RelayController *g_relay_controller = nullptr;

class SystemCleanup {
public:
    ~SystemCleanup() {
        ESP_LOGI(TAG, "Cleaning up system resources");
        nvs_flash_deinit();
        esp_event_loop_delete_default();
    }
};

extern "C" [[noreturn]] void app_main(void) {
    // Initialize all variables at the start
    esp_err_t ret = ESP_OK;
    RelayController *relay_controller = nullptr;
    SystemCleanup cleanup;
    constexpr int MAX_WIFI_WAIT_SECONDS = 30;
    constexpr int MAX_WIFI_WAIT_MS = MAX_WIFI_WAIT_SECONDS * 1000;

    ESP_LOGI(TAG, "Initializing basic system...");
    ret = SystemInitializer::initialize_basic();
    if (ret != ESP_OK) {
        goto error_handler;
    }

    if (!WifiManager::instance().is_connected()) {
        // Only wait for WiFi if we're not in SmartConfig mode
        if (!WifiManager::instance().is_in_smartconfig_mode()) {
            int elapsed_ms = 0;
            ESP_LOGI(TAG, "Waiting for WiFi connection...");

            while (elapsed_ms < MAX_WIFI_WAIT_MS) {
                constexpr int WIFI_CONNECT_CHECK_INTERVAL_MS = 100;
                if (WifiManager::instance().is_connected()) {
                    ESP_LOGI(TAG, "WiFi connected successfully");
                    break;
                }

                if (elapsed_ms % 1000 == 0) {
                    // Log only every second
                    ESP_LOGI(TAG, "Waiting for WiFi configuration... (%d/%d)",
                             elapsed_ms/1000 + 1, MAX_WIFI_WAIT_SECONDS);
                }

                vTaskDelay(pdMS_TO_TICKS(WIFI_CONNECT_CHECK_INTERVAL_MS));
                elapsed_ms += WIFI_CONNECT_CHECK_INTERVAL_MS;
            }

            if (!WifiManager::instance().is_connected()) {
                ESP_LOGE(TAG, "Failed to connect to WiFi within timeout period");
                ret = ESP_ERR_TIMEOUT;
                goto error_handler;
            }
        } else {
            ESP_LOGI(TAG, "System is in SmartConfig mode, waiting for configuration");
            // Don't treat SmartConfig mode as an error, go directly to SmartConfig handling
            goto smartconfig_handler;
        }
    }

    // Only proceed with full initialization after WiFi is connected
    ESP_LOGV(TAG, "WiFi connected, initializing full system...");
    ret = SystemInitializer::initialize_full(&relay_controller);
    if (ret != ESP_OK) {
        goto error_handler;
    }

    g_relay_controller = relay_controller;
    ESP_LOGI(TAG, "Antenna Switch Controller initialized successfully");

    // Main loop
    while (true) {
        // Feed the watchdog
        if (esp_task_wdt_status(xTaskGetCurrentTaskHandle()) == ESP_OK) {
            // Add watchdog feed
            esp_task_wdt_reset();
        }
        // esp_task_wdt_reset();
        
        // Add error checking for watchdog reset
        if (esp_task_wdt_status(xTaskGetCurrentTaskHandle()) == ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "Task not subscribed to watchdog, attempting to resubscribe");
            esp_task_wdt_add(xTaskGetCurrentTaskHandle());
        }
        
        // Increased delay to reduce system load
        vTaskDelay(pdMS_TO_TICKS(500));
    }

smartconfig_handler:
    // Start SmartConfig
    ret = WifiManager::instance().start_smartconfig();
    
    // Enter SmartConfig wait loop with watchdog feed
    while (true) {
        // Feed the watchdog

        if (esp_task_wdt_status(xTaskGetCurrentTaskHandle()) == ESP_OK) {
            // Add watchdog feed
            esp_task_wdt_reset();
        }

        if (WifiManager::instance().is_connected()) {
            ESP_LOGI(TAG, "SmartConfig successful, restarting system...");
            RestartManager::clear_restart_count();
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        }
        // Reduced delay to feed watchdog more frequently
        vTaskDelay(pdMS_TO_TICKS(100));
    }

error_handler:
    ESP_LOGE(TAG, "Fatal error occurred: %s", esp_err_to_name(ret));
    RestartManager::store_error_state(ret);

    // Clean up all resources
    if (relay_controller) {
        delete relay_controller;
        relay_controller = nullptr;
        g_relay_controller = nullptr;
    }

    // Check if we've exceeded max restart attempts
    if (RestartManager::check_restart_count() == ESP_FAIL) {
        ESP_LOGE(TAG, "Maximum restart attempts reached. Starting SmartConfig...");

        // Start SmartConfig and wait indefinitely
        ret = WifiManager::instance().start_smartconfig();

        // Enter infinite loop waiting for SmartConfig success
        while (true) {
            if (WifiManager::instance().is_connected()) {
                ESP_LOGI(TAG, "SmartConfig successful, restarting system...");
                RestartManager::clear_restart_count(); // Clear restart count on success
                vTaskDelay(pdMS_TO_TICKS(1000)); // Brief delay before restart
                esp_restart();
            }
            vTaskDelay(pdMS_TO_TICKS(1000)); // Check every second
        }
    }

    ESP_LOGW(TAG, "System will restart in 5 seconds...");
    vTaskDelay(pdMS_TO_TICKS(5000));
    esp_restart();
}
