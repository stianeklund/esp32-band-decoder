#include <esp_event.h>
#include <esp_task_wdt.h>
#include <memory>
#include <nvs_flash.h>
#include <stdio.h>   // For printf
#include <stdlib.h>  // For malloc/free
#include "esp_heap_caps.h" // For heap stats

#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h" // For esp_restart
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "restart_manager.h"
#include "system_initializer.h"
#include "relay_controller.h"
#include "webserver.h"
#include "wifi_manager.hpp" // Included for CLI task

#include <string.h> // For strncmp, strlen, etc. in CLI

static auto TAG = "MAIN";

// Global pointer to RelayController
static RelayController *g_relay_controller = nullptr;

// --- START: Serial CLI Code ---
#define CLI_TASK_STACK_SIZE 4096
#define CLI_PROMPT "> "
#define MAX_CLI_INPUT_SIZE 200

static const char* CLI_TAG = "SerialCLI";

// Temporary storage for SSID and password
static char cli_ssid_buffer[33] = {0}; // Max SSID length 32 + null
static char cli_password_buffer[65] = {0}; // Max password length 64 + null
static bool cli_ssid_set = false;
static bool cli_password_set = false;

static void process_cli_command(char* line) {
    // Trim newline characters
    line[strcspn(line, "\r\n")] = 0;
    ESP_LOGD(CLI_TAG, "Received command: '%s'", line);

    if (strncmp(line, "set_ssid ", 9) == 0) {
        char* value = line + 9;
        if (strlen(value) > 0 && strlen(value) < sizeof(cli_ssid_buffer)) {
            strncpy(cli_ssid_buffer, value, sizeof(cli_ssid_buffer) - 1);
            cli_ssid_buffer[sizeof(cli_ssid_buffer) - 1] = '\0';
            cli_ssid_set = true;
            printf("OK. SSID set to: %s\n", cli_ssid_buffer);
        } else {
            printf("Error: Invalid SSID length. Max 32 characters.\n");
        }
    } else if (strncmp(line, "set_pass ", 9) == 0) {
        char* value = line + 9;
        if (strlen(value) > 0 && strlen(value) < sizeof(cli_password_buffer)) {
            strncpy(cli_password_buffer, value, sizeof(cli_password_buffer) - 1);
            cli_password_buffer[sizeof(cli_password_buffer) - 1] = '\0';
            cli_password_set = true;
            printf("OK. Password set.\n"); // Don't echo password
        } else {
            printf("Error: Invalid password length. Max 64 characters.\n");
        }
    } else if (strcmp(line, "apply_wifi") == 0) {
        if (cli_ssid_set && cli_password_set) {
            printf("Applying WiFi credentials: SSID='%s'\n", cli_ssid_buffer);
            esp_err_t err = WifiManager::instance().set_and_apply_credentials(cli_ssid_buffer, cli_password_buffer);
            if (err == ESP_OK) {
                printf("OK. Credentials saved and connection attempt initiated. Monitor logs.\n");
            } else {
                printf("Error applying credentials: %s\n", esp_err_to_name(err));
            }
            // Reset for next time
            cli_ssid_set = false;
            cli_password_set = false;
            memset(cli_ssid_buffer, 0, sizeof(cli_ssid_buffer));
            memset(cli_password_buffer, 0, sizeof(cli_password_buffer));
        } else {
            printf("Error: SSID or password not set. Use 'set_ssid' and 'set_pass' first.\n");
        }
    } else if (strcmp(line, "clear_wifi") == 0) {
        printf("Clearing stored WiFi credentials...\n");
        esp_err_t err = WifiManager::instance().clear_credentials();
        if (err == ESP_OK) {
            printf("OK. Credentials cleared. Device may enter SmartConfig on next WiFi init/reboot.\n");
        } else {
            printf("Error clearing credentials: %s\n", esp_err_to_name(err));
        }
    } else if (strcmp(line, "wifi_status") == 0) {
        printf("WiFi Status:\n");
        printf("  Connected: %s\n", WifiManager::instance().is_connected() ? "Yes" : "No");
        // is_in_smartconfig_mode() indicates if it's *currently* trying SmartConfig or would if no creds.
        printf("  Attempting SmartConfig (if not connected): %s\n", WifiManager::instance().is_in_smartconfig_mode() ? "Yes" : "No");
        char ip_addr[16];
        if (WifiManager::instance().get_ip_info(ip_addr, sizeof(ip_addr)) == ESP_OK && strlen(ip_addr) > 0 && strcmp(ip_addr, "0.0.0.0") != 0) {
            printf("  IP Address: %s\n", ip_addr);
        } else {
            printf("  IP Address: N/A\n");
        }
    } else if (strcmp(line, "reboot") == 0) {
        printf("Rebooting...\n");
        esp_restart();
    }
    else if (strcmp(line, "help") == 0) {
        printf("Available commands:\n");
        printf("  set_ssid <ssid>          - Set WiFi SSID (temporary)\n");
        printf("  set_pass <password>      - Set WiFi password (temporary)\n");
        printf("  apply_wifi             - Save temporary SSID/password to NVS and attempt to connect\n");
        printf("  clear_wifi             - Clear saved WiFi credentials from NVS\n");
        printf("  wifi_status            - Show current WiFi connection status\n");
        printf("  reboot                 - Reboot the device\n");
        printf("  help                   - Show this help message\n");
    } else if (strlen(line) > 0) { // Non-empty line
        printf("Unknown command: %s. Type 'help'.\n", line);
    }
}

void serial_cli_task(void *pvParameters) {
    char line_buffer[MAX_CLI_INPUT_SIZE];
    // Small delay to ensure UART driver is fully initialized and ready,
    // and to allow other boot messages to print first.
    vTaskDelay(pdMS_TO_TICKS(100)); 
    printf("\nSerial CLI started. Type 'help' for commands.\n");

    while (1) {
        printf(CLI_PROMPT);
        fflush(stdout); // Ensure prompt is displayed

        // Clear buffer before reading new input
        memset(line_buffer, 0, sizeof(line_buffer));

        if (fgets(line_buffer, sizeof(line_buffer), stdin) != NULL) {
            process_cli_command(line_buffer);
        } else {
            // fgets returned NULL, possibly due to EOF or error on stdin
            // This can happen if the serial terminal is disconnected.
            // Add a small delay to prevent busy-looping if stdin is truly closed.
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        // Add a small delay to allow other tasks to run, especially if fgets returns immediately (e.g. empty input)
        vTaskDelay(pdMS_TO_TICKS(10)); 
    }
}
// --- END: Serial CLI Code ---


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
        // esp_event_loop_delete_default(); // This might be problematic if other components still use it
    }
};

extern "C" [[noreturn]] void app_main(void) {
    // Initialize all variables at the start
    esp_err_t ret = ESP_OK;
    RelayController *relay_controller = nullptr;
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
    // Ensure it has a reasonable priority.
    // Priority 5 is typical for application tasks.
    if (xTaskCreate(serial_cli_task, "serial_cli_task", CLI_TASK_STACK_SIZE, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create serial_cli_task");
        // Decide if this is a fatal error. For now, we'll continue.
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
                // Don't goto error_handler here, allow system to proceed.
                // The CLI can be used to configure WiFi.
                // We will proceed to full init, but some services might not work.
                ESP_LOGW(TAG, "Proceeding without WiFi connection. Use Serial CLI to configure.");
                // ret = ESP_ERR_TIMEOUT; // No longer treating this as fatal for app_main startup
                // goto error_handler;
            }
        } else {
            ESP_LOGI(TAG, "System is in SmartConfig mode or no credentials, waiting for configuration via CLI or SmartConfig.");
            // Don't treat SmartConfig mode as an error, go directly to SmartConfig handling
            // OR, better, let the main loop run and allow CLI to be used.
            // The smartconfig_handler label might not be the best flow if CLI is primary.
            // For now, let's allow it to fall through to full system init.
            // The smartconfig_task in WifiManager will run if needed.
            // goto smartconfig_handler; // Removing this to allow CLI usage
        }
    }

    // Only proceed with full initialization after WiFi is connected (or timeout/smartconfig mode)
    ESP_LOGI(TAG, "Initializing full system..."); // Changed log from ESP_LOGV
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
            // Not going to error_handler, as system can run without webserver
        } else {
            ESP_LOGI(TAG, "Starting WebServer and registering core handlers...");
            if (WebServer::instance().start() != ESP_OK) {
                ESP_LOGE(TAG, "Failed to start WebServer or register core handlers");
                if (ret == ESP_OK) ret = ESP_FAIL;
            } else {
                ESP_LOGI(TAG, "Registering WebServer URI handlers...");
                if (WebServer::instance().register_uri_handlers() != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to register WebServer URI handlers");
                    if (ret == ESP_OK) ret = ESP_FAIL;
                    WebServer::instance().stop(); // Stop the server if handlers fail
                } else {
                    ESP_LOGI(TAG, "WebServer started and all handlers registered successfully.");
                }
            }
        }
    } else {
        ESP_LOGW(TAG, "WiFi not connected. WebServer will not be started. Use Serial CLI to configure WiFi.");
    }
    
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

// The smartconfig_handler might not be reached if we prioritize CLI.
// Consider removing or refactoring this if CLI is the primary recovery.
// For now, it's kept as a fallback if the main loop exits or for explicit calls.
smartconfig_handler: 
    ESP_LOGI(TAG, "Entering SmartConfig handler. This might be due to explicit call or specific startup condition.");
    // Start SmartConfig if not already started by WifiManager events
    if (!WifiManager::instance().is_in_smartconfig_mode()){ // Check if it's already trying
        // This call might be redundant if WifiManager's event handler already started it.
        // WifiManager::instance().start_smartconfig(); 
        ESP_LOGI(TAG, "SmartConfig task should be running via WifiManager event handler if needed.");
    }
    
    // Enter SmartConfig wait loop with watchdog feed
    while (true) {
        // Feed the watchdog

        if (esp_task_wdt_status(xTaskGetCurrentTaskHandle()) == ESP_OK) {
            // Add watchdog feed
            esp_task_wdt_reset();
        }

        if (WifiManager::instance().is_connected()) {
            ESP_LOGI(TAG, "SmartConfig successful (or connection established through other means), restarting system...");
            RestartManager::clear_restart_count();
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(100)); // Check frequently
    }

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
        // WifiManager::instance().clear_credentials(); // Optionally clear credentials to force SmartConfig
        // WifiManager::instance().start_smartconfig(); // Ensure SmartConfig is attempted

        // Loop indefinitely, allowing CLI to be used or SmartConfig to (eventually) succeed.
        // The serial_cli_task is already running.
        while (true) {
            if (esp_task_wdt_status(xTaskGetCurrentTaskHandle()) == ESP_OK) {
                 esp_task_wdt_reset();
            }
            if (WifiManager::instance().is_connected()) {
                ESP_LOGI(TAG, "Connection established after max restarts (possibly via CLI/SmartConfig), restarting system...");
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
