#include "serial_cli.h"
#include <cctype>          // For isprint
#include <cstdio>
#include <cstring>
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"      // For nvs_flash_erase
#include "wifi_manager.hpp" // For WiFi commands

SerialCli::SerialCli() :
    ssid_set_(false),
    password_set_(false),
    task_handle_(nullptr) {
    memset(ssid_buffer_, 0, sizeof(ssid_buffer_));
    memset(password_buffer_, 0, sizeof(password_buffer_));
}

esp_err_t SerialCli::start_task() {
    const BaseType_t result = xTaskCreate(
        cli_task_trampoline,
        "serial_cli_task",
        TASK_STACK_SIZE,
        this, // Pass instance pointer as arg
        5,    // Priority
        &task_handle_
    );

    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create serial_cli_task");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Serial CLI task created successfully."); // Changed from "started" to "created" as task runs after scheduler starts
    return ESP_OK;
}

void SerialCli::process_command(char* line) {
    // The input line from cli_task_member's getchar loop is already null-terminated
    // and does not contain newline characters.
    ESP_LOGD(TAG, "Received command: '%s'", line);

    if (strncmp(line, "set_ssid ", 9) == 0) {
        if (const char* value = line + 9; strlen(value) > 0 && strlen(value) < sizeof(ssid_buffer_)) {
            strncpy(ssid_buffer_, value, sizeof(ssid_buffer_) - 1);
            ssid_buffer_[sizeof(ssid_buffer_) - 1] = '\0'; // Ensure null termination
            ssid_set_ = true;
            printf("OK. SSID set to: %s\n", ssid_buffer_);
        } else if (strlen(value) == 0) {
            printf("Error: SSID cannot be empty.\n");
        } else {
            printf("Error: Invalid SSID length. Max 32 characters.\n");
        }
    } else if (strncmp(line, "set_pass ", 9) == 0) {
        // Password can be empty, but check length if not empty
        if (const char* value = line + 9; strlen(value) < sizeof(password_buffer_)) {
            strncpy(password_buffer_, value, sizeof(password_buffer_) - 1);
            password_buffer_[sizeof(password_buffer_) - 1] = '\0'; // Ensure null termination
            password_set_ = true;
            printf("OK. Password set.\n"); // Don't echo password
        } else {
            printf("Error: Invalid password length. Max 64 characters.\n");
        }
    } else if (strcmp(line, "apply_wifi") == 0) {
        if (ssid_set_ && password_set_) {
            printf("Applying WiFi credentials: SSID='%s'\n", ssid_buffer_);

            if (const esp_err_t err = WifiManager::instance().set_and_apply_credentials(ssid_buffer_, password_buffer_); err == ESP_OK) {
                printf("OK. Credentials saved and connection attempt initiated. Monitor logs.\n");
            } else {
                printf("Error applying credentials: %s\n", esp_err_to_name(err));
            }
            // Reset for next time, regardless of success
            ssid_set_ = false;
            password_set_ = false;
            memset(ssid_buffer_, 0, sizeof(ssid_buffer_));
            memset(password_buffer_, 0, sizeof(password_buffer_));
        } else {
            printf("Error: SSID or password not set. Use 'set_ssid' and 'set_pass' first.\n");
        }
    } else if (strcmp(line, "clear_wifi") == 0) {
        printf("Clearing stored WiFi credentials...\n");
        if (const esp_err_t err = WifiManager::instance().clear_credentials(); err == ESP_OK) {
            printf("OK. Credentials cleared. Device may enter SmartConfig or require manual config on next WiFi init/reboot.\n");
        } else {
            printf("Error clearing credentials: %s\n", esp_err_to_name(err));
        }
    } else if (strcmp(line, "wifi_status") == 0) {
        printf("WiFi Status:\n");
        printf("  Connected: %s\n", WifiManager::instance().is_connected() ? "Yes" : "No");
        printf("  SmartConfig Mode (if not connected & no creds): %s\n", WifiManager::instance().is_in_smartconfig_mode() ? "Yes" : "No");
        char ip_addr[16] = {0};
        if (WifiManager::instance().is_connected() && WifiManager::instance().get_ip_info(ip_addr, sizeof(ip_addr)) == ESP_OK && strlen(ip_addr) > 0 && strcmp(ip_addr, "0.0.0.0") != 0) {
            printf("  IP Address: %s\n", ip_addr);
        } else {
            printf("  IP Address: N/A\n");
        }
    } else if (strcmp(line, "erase_nvs") == 0) {
        printf("WARNING: This will erase ALL data in Non-Volatile Storage (NVS),\n");
        printf("including WiFi credentials and any other stored settings.\n");
        printf("The device will reboot afterwards.\n");
        printf("Type 'erase_nvs_confirm' to proceed.\n");
    } else if (strcmp(line, "erase_nvs_confirm") == 0) {
        printf("Erasing NVS...\n");
        if (const esp_err_t err = nvs_flash_erase(); err == ESP_OK) {
            printf("NVS erased successfully. Rebooting...\n");
        } else {
            printf("Error erasing NVS: %s. Rebooting anyway...\n", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(1000)); // Delay for message to be sent/logged
        esp_restart();
    } else if (strcmp(line, "reboot") == 0) {
        printf("Rebooting...\n");
        vTaskDelay(pdMS_TO_TICKS(100)); // Short delay for printf to flush
        esp_restart();
    } else if (strcmp(line, "help") == 0) {
        printf("Available commands:\n");
        printf("  set_ssid <ssid>          - Set WiFi SSID (temporary, max 32 chars)\n");
        printf("  set_pass <password>      - Set WiFi password (temporary, max 64 chars)\n");
        printf("  apply_wifi             - Save temporary SSID/password to NVS and attempt to connect\n");
        printf("  clear_wifi             - Clear saved WiFi credentials from NVS\n");
        printf("  wifi_status            - Show current WiFi connection status\n");
        printf("  erase_nvs              - !!! Erase all data in NVS and reboot !!!\n");
        printf("  reboot                 - Reboot the device\n");
        printf("  help                   - Show this help message\n");
    } else if (strlen(line) > 0) { // Non-empty line that wasn't a command
        printf("Unknown command: '%s'. Type 'help'.\n", line);
    }
    // If line is empty (only enter pressed), do nothing, just loop for new prompt.
}

void SerialCli::cli_task_trampoline(void* arg) {
    if (auto* self = static_cast<SerialCli*>(arg)) {
        self->cli_task_member();
    } else {
        ESP_LOGE(TAG, "cli_task_trampoline received null argument");
        vTaskDelete(nullptr); // Delete self if arg is null
    }
}

// ReSharper disoble once CppDFAUnreachableFunctionCall
void SerialCli::cli_task_member() {
    char line_buffer[MAX_INPUT_SIZE];

    // Small delay to ensure UART driver is fully initialized and ready,
    // and to allow other boot messages to print first.
    vTaskDelay(pdMS_TO_TICKS(100));
    printf("\nSerial CLI initialized. Type 'help' for commands.\n");

    while (true) { // Main loop for CLI
        printf(PROMPT);
        fflush(stdout); // Ensure prompt is displayed

        int pos = 0;
        memset(line_buffer, 0, sizeof(line_buffer));

        while (true) {
            const int c = getchar();

            if (c == EOF) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            if (c == '\n' || c == '\r') {
                line_buffer[pos] = '\0'; // Null-terminate the command
                printf("\n");            // Echo newline to the terminal
                fflush(stdout);

                if (pos > 0) {
                    process_command(line_buffer);
                }
                break; // Exit inner loop, will go to next_command_prompt via outer loop's structure
            }

            if (c == '\b' || c == 127) { // Handle backspace (ASCII BS or DEL)
                if (pos > 0) {
                    pos--;
                    printf("\b \b"); // Erase character on terminal: move cursor back, print space, move cursor back
                    fflush(stdout);
                }
            } else if (isprint(c)) {
                if (pos < (MAX_INPUT_SIZE - 1)) {
                    line_buffer[pos++] = static_cast<char>(c);
                    putchar(c); // Echo character to the terminal
                    fflush(stdout);
                } else {
                    // Buffer is full, ignore character. Optionally, ring bell (putchar('\a');)
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
