#include <cstdio>
#include <esp_event.h>
#include <esp_task_wdt.h>
#include <nvs_flash.h>

#ifdef CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
#include "esp_heap_caps.h" // For heap stats
#endif

#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h" // For esp_restart
#include "relay_controller.h"
#include "restart_manager.h"
#include "serial_cli.h"
#include "system_initializer.h"
#include "webserver.h"
#include "wifi_manager.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static auto TAG = "MAIN";

static RelayController* g_relay_controller = nullptr;
static SerialCli g_serial_cli;


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
        // esp_event_loop_delete_default(); // This might be problematic if other components still use it
    }
};

extern "C" [[noreturn]] void app_main(void) {
    // Initialize all variables at the start
    esp_err_t ret = ESP_OK;
    RelayController* relay_controller = nullptr;
    SystemCleanup cleanup; // RAII cleanup for NVS
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

    // Start the Serial CLI task early, so it's available even if WiFi setup has issues.
    if (g_serial_cli.start_task() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Serial CLI task");
        // Decide if this is a fatal error. For now, we'll continue without CLI.
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

                // Feed the watchdog during this potentially long wait
                if (const esp_err_t wdt_status_wifi_wait = esp_task_wdt_status(xTaskGetCurrentTaskHandle());
                    wdt_status_wifi_wait == ESP_OK) {
                    esp_task_wdt_reset();
                }
                else if (wdt_status_wifi_wait == ESP_ERR_NOT_FOUND) {
                    // This should not happen if WDT was initialized and task added by SystemInitializer
                    ESP_LOGW(TAG, "Main task not subscribed to WDT during WiFi wait loop!");
                }

                vTaskDelay(pdMS_TO_TICKS(WIFI_CONNECT_CHECK_INTERVAL_MS));
                elapsed_ms += WIFI_CONNECT_CHECK_INTERVAL_MS;
            }

            if (!WifiManager::instance().is_connected()) {
                ESP_LOGE(TAG, "Failed to connect to WiFi within timeout period");
                // Don't goto error_handler here, allow system to proceed.
                // The CLI can be used to configure WiFi.
                // We will proceed to full init, but some services might not work.
                ESP_LOGW(TAG, "Proceeding without WiFi connection. Use Serial CLI to configure.");
                // ret = ESP_ERR_TIMEOUT; // No longer treating this as fatal for app_main startup
                // goto error_handler;
            }
        }
        else {
            ESP_LOGI(
                TAG,
                "System is in SmartConfig mode or no credentials, waiting for configuration via CLI or SmartConfig.");
            // Don't treat SmartConfig mode as an error, go directly to SmartConfig handling
            // OR, better, let the main loop run and allow CLI to be used.
            // The smartconfig_handler label might not be the best flow if CLI is primary.
            // For now, let's allow it to fall through to full system init.
            // The smartconfig_task in WifiManager will run if needed.
            // goto smartconfig_handler; // Removing this to allow CLI usage
        }
    }

    // Only proceed with full initialization after WiFi is connected (or timeout/smartconfig mode)
    ESP_LOGD(TAG, "Initializing full system...");
    ret = SystemInitializer::initialize_full(&relay_controller);
    if (ret != ESP_OK) {
        goto error_handler;
    }

    g_relay_controller = relay_controller;
    ESP_LOGI(TAG, "Antenna Switch Controller initialized successfully");

    // Initialize WebServer only if WiFi is connected
    if (WifiManager::instance().is_connected()) {
        ESP_LOGI(TAG, "Initializing WebServer...");
        if (WebServer::instance().init() != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize WebServer");
            if (ret == ESP_OK) ret = ESP_FAIL; // Keep track of first error
        }
        else {
            ESP_LOGI(TAG, "Starting WebServer with URI handlers...");
            if (WebServer::instance().start() != ESP_OK) {
                ESP_LOGE(TAG, "Failed to start WebServer with URI handlers");
                if (ret == ESP_OK) ret = ESP_FAIL;
            }
            else {
                ESP_LOGI(TAG, "WebServer started successfully with all URI handlers registered.");
            }
        }
    }
    else {
        ESP_LOGW(TAG, "WiFi not connected. WebServer will not be started. Use Serial CLI to configure WiFi.");
    }

    // Main loop
    while (true) {
        // Feed the watchdog
        if (const esp_err_t wdt_status = esp_task_wdt_status(xTaskGetCurrentTaskHandle()); wdt_status == ESP_OK) {
            esp_task_wdt_reset();
        }
        else if (wdt_status == ESP_ERR_NOT_FOUND) {
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

    // The following block provides a dedicated mode for WiFi configuration
    // where SmartConfig is actively initiated, and the Serial CLI remains available.
    // If WiFi connection is established, the system restarts.
    // This section is currently not jumped to via a goto, but could be refactored into a function
    // or used with a goto if specific unrecoverable startup states need to force this mode.
    ESP_LOGI(TAG, "Entering SmartConfig handler. Attempting to start SmartConfig.");

    // Optional: Clear existing credentials to ensure a fresh SmartConfig attempt,
    // especially if this handler is entered due to persistent connection failures.
    // WifiManager::instance().clear_credentials();

    // Ensure SmartConfig is started. WifiManager's internal logic might also start it,
    // but calling it here makes it explicit for this handler.
    if (WifiManager::instance().start_smartconfig() == ESP_OK) {
        ESP_LOGI(TAG, "SmartConfig initiated by handler. CLI is available for alternative configuration.");
    }
    else {
        ESP_LOGW(
            TAG,
            "Failed to explicitly start SmartConfig via handler. WifiManager might still attempt it based on events.");
    }

    ESP_LOGI(TAG, "SmartConfig handler active. CLI is available. Waiting for WiFi connection to restart system...");
    // Loop indefinitely, allowing CLI to operate (as it's a separate task)
    // and waiting for WiFi connection to trigger a system restart.
    while (true) {
        if (esp_task_wdt_status(xTaskGetCurrentTaskHandle()) == ESP_OK) {
            esp_task_wdt_reset();
        }

        if (WifiManager::instance().is_connected()) {
            ESP_LOGI(TAG, "Connection established (possibly via SmartConfig or CLI). Restarting system...");
            RestartManager::clear_restart_count(); // Assuming connection means configuration is good
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart(); // Restart to apply new state cleanly
        }
        vTaskDelay(pdMS_TO_TICKS(1000)); // Check connection status periodically
    }
    // Note: The above loop is infinite. app_main will remain here until WiFi connects and system restarts.

error_handler:
    ESP_LOGE(TAG, "Fatal error occurred in app_main: %s. Restarting.", esp_err_to_name(ret));
    RestartManager::store_error_state(ret);

    // Clean up all resources
    if (relay_controller) {
        delete relay_controller;
        relay_controller = nullptr;
        g_relay_controller = nullptr;
    }

    if (RestartManager::check_restart_count() == ESP_FAIL) {
        ESP_LOGE(TAG, "Maximum restart attempts reached. Forcing SmartConfig mode and CLI availability.");
        // Optionally clear credentials to ensure SmartConfig doesn't try to use faulty ones from previous attempts.
        // WifiManager::instance().clear_credentials();

        ESP_LOGI(TAG, "Attempting to start SmartConfig due to max restarts.");
        if (WifiManager::instance().start_smartconfig() == ESP_OK) {
            ESP_LOGI(TAG, "SmartConfig initiated due to max restarts. CLI is also available.");
        }
        else {
            ESP_LOGW(
                TAG,
                "Failed to explicitly start SmartConfig after max restarts. WifiManager might still attempt it based on events.")
            ;
        }

        // Loop indefinitely, allowing CLI to be used or SmartConfig to (eventually) succeed.
        // The serial_cli_task is already running.
        while (true) {
            if (esp_task_wdt_status(xTaskGetCurrentTaskHandle()) == ESP_OK) {
                esp_task_wdt_reset();
            }
            if (WifiManager::instance().is_connected()) {
                ESP_LOGI(
                    TAG,
                    "Connection established after max restarts (possibly via CLI/SmartConfig), restarting system...");
                RestartManager::clear_restart_count();
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_restart();
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    ESP_LOGW(TAG, "System will restart in 5 seconds due to error...");
    vTaskDelay(pdMS_TO_TICKS(5000));
    esp_restart();
}
