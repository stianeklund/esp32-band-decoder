#include <esp_event.h>
#include <esp_task_wdt.h>
#include <memory>
#include <nvs_flash.h>
#include <stdio.h>   // For printf
#include <stdlib.h>  // For malloc/free
#include "esp_heap_caps.h" // For heap stats

#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "restart_manager.h"
#include "system_initializer.h"
#include "relay_controller.h"
#include "webserver.h"
#include "wifi_manager.hpp"

static auto TAG = "MAIN";

// Global pointer to RelayController
static RelayController *g_relay_controller = nullptr;

// Function to display FreeRTOS runtime statistics

#ifdef CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
static void display_runtime_stats() {
    constexpr size_t buffer_size = 2048; // Buffer for stats, adjust if necessary
    const auto stats_buffer = static_cast<char*>(malloc(buffer_size));

    if (stats_buffer == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate buffer for runtime stats");
        return;
    }

    printf("\n\n--- FreeRTOS Task List & Runtime Stats ---\n");

    // Task List
    // vTaskList() formats the list of tasks into a human-readable table.
    printf("Task List:\n");
    vTaskList(stats_buffer);
    printf("%s\n", stats_buffer); // Output is already formatted with newlines

    // Runtime Stats
    // vTaskGetRunTimeStats() formats the runtime statistics into a human-readable table.
    printf("Runtime Stats (Abs Time, %%Time):\n");
    vTaskGetRunTimeStats(stats_buffer);
    printf("%s\n", stats_buffer); // Output is already formatted with newlines

    free(stats_buffer);

    // Heap Information
    printf("Heap Information:\n");
    printf("  Current free heap: %lu bytes\n", esp_get_free_heap_size());
    printf("  Minimum free heap: %lu bytes\n", esp_get_minimum_free_heap_size());
    // For more detailed heap info, you can use:
    // heap_caps_print_heap_info(MALLOC_CAP_DEFAULT);
    printf("--- End of FreeRTOS Stats ---\n\n");
}
#endif

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

#ifdef CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
        uint32_t loop_counter = 0;
    // Display stats every (stats_display_interval_multiplier * 500ms)
    constexpr uint32_t stats_display_interval_multiplier = 20; // e.g., 20 * 500ms = 10 seconds
#endif

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

    // Initialize WebServer
    ESP_LOGI(TAG, "Initializing WebServer...");
    // ESP_GOTO_ON_ERROR(WebServer::instance().init(), error_handler, TAG, "failed to initialize WebServer");
    if (WebServer::instance().init() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WebServer");
        if (ret == ESP_OK) ret = ESP_FAIL;
        goto error_handler;
    }

    // Start WebServer (which now also registers core handlers)
    ESP_LOGI(TAG, "Starting WebServer and registering core handlers...");
    if (WebServer::instance().start() != ESP_OK) { 
        ESP_LOGE(TAG, "Failed to start WebServer or register core handlers");
        if (ret == ESP_OK) ret = ESP_FAIL; 
        // WebServer::start() should handle its own cleanup (stopping m_server) on failure
        goto error_handler;
    }
    
    // Register WebServer URI handlers
    ESP_LOGI(TAG, "Registering WebServer URI handlers...");
    if (WebServer::instance().register_uri_handlers() != ESP_OK) { 
        ESP_LOGE(TAG, "Failed to register WebServer URI handlers");
        if (ret == ESP_OK) ret = ESP_FAIL; 
        WebServer::instance().stop(); // Stop the server if handlers fail
        goto error_handler;
    }

    ESP_LOGI(TAG, "WebServer started and all handlers registered successfully.");

    // Main loop

    while (true) {
        // Feed the watchdog
        const esp_err_t wdt_status = esp_task_wdt_status(xTaskGetCurrentTaskHandle());
        if (wdt_status == ESP_OK) {
            esp_task_wdt_reset();
        } else if (wdt_status == ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "Task not subscribed to WDT, attempting to resubscribe.");
            if (esp_task_wdt_add(xTaskGetCurrentTaskHandle()) != ESP_OK) {
                ESP_LOGE(TAG, "Failed to add task to WDT.");
            }
        }

#ifdef CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
        // uint32_t loop_counter = 0; // Moved to the top of app_main
        // Display stats every (stats_display_interval_multiplier * 500ms)
        // const uint32_t stats_display_interval_multiplier = 20; // e.g., 20 * 500ms = 10 seconds // Moved to the top of app_main

        // Periodically display runtime stats
        if (++loop_counter >= stats_display_interval_multiplier) {
            loop_counter = 0;
            display_runtime_stats();
        }
#endif // CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
        
        // Delay to reduce system load and allow other tasks to run
        // This should be unconditional and inside the main loop
        vTaskDelay(pdMS_TO_TICKS(500));
    } // This brace now correctly closes the while(true) loop,
      // regardless of CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS.

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
