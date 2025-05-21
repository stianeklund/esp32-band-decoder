#include "config_manager.h"
#include "antenna_switch.h"
#include "esp_log.h"
#include "nvs.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include <cstring>
#include <sys/param.h>
#include "freertos/FreeRTOS.h" // For task and semaphore functions
#include "freertos/task.h"
#include "freertos/semphr.h"

static auto TAG = "CONFIG_MANAGER";

// Initialize static member
ConfigManager *ConfigManager::instance_ = nullptr;

ConfigManager::ConfigManager() : current_config_(new antenna_switch_config_t()), config_dirty_(false), nvs_writer_shutdown_requested_(false), nvs_writer_task_handle_(nullptr), nvs_save_signal_(nullptr) {
    if (instance_ == nullptr) {
        instance_ = this;
    }

    nvs_save_signal_ = xSemaphoreCreateBinary();
    if (nvs_save_signal_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create NVS save semaphore! Async saving will not work.");
    }

    BaseType_t task_created = xTaskCreate(
        nvs_writer_task_trampoline,
        "nvs_writer_task",
        NVS_WRITER_TASK_STACK_SIZE,
        this, // Pass ConfigManager instance as argument
        NVS_WRITER_TASK_PRIORITY,
        &nvs_writer_task_handle_
    );

    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create NVS writer task! Async saving will not work.");
        if (nvs_save_signal_ != nullptr) {
            vSemaphoreDelete(nvs_save_signal_);
            nvs_save_signal_ = nullptr;
        }
        nvs_writer_task_handle_ = nullptr; // Ensure handle is null if task creation failed
    } else {
        ESP_LOGI(TAG, "NVS writer task created successfully.");
    }
}

ConfigManager::~ConfigManager() {
    ESP_LOGI(TAG, "Shutting down ConfigManager and NVS writer task.");
    nvs_writer_shutdown_requested_.store(true);

    if (nvs_save_signal_ != nullptr) {
        xSemaphoreGive(nvs_save_signal_); // Wake up the task so it can see the shutdown flag
    }

    // Wait a bit for the task to shut down. A more robust way would be for the task
    // to signal back its termination, or use eTaskGetState to check.
    // For now, assuming it self-deletes as per nvs_writer_task_trampoline.
    if (nvs_writer_task_handle_ != nullptr) {
        ESP_LOGD(TAG, "Waiting for NVS writer task to terminate...");
        // vTaskDelay(pdMS_TO_TICKS(200));
        // If the task doesn't self-delete, you might need:
        // if (eTaskGetState(nvs_writer_task_handle_) != eDeleted) {
        //    vTaskDelete(nvs_writer_task_handle_);
        // }
        // nvs_writer_task_handle_ = nullptr; // Cleared in trampoline
    }

    if (nvs_save_signal_ != nullptr) {
        vSemaphoreDelete(nvs_save_signal_);
        nvs_save_signal_ = nullptr;
    }
    delete current_config_;
    current_config_ = nullptr; // Good practice after delete
    ESP_LOGI(TAG, "ConfigManager cleanup complete.");
}

ConfigManager &ConfigManager::instance() {
    static ConfigManager instance; // Meyers' singleton
    return instance;
}

void ConfigManager::nvs_writer_task_trampoline(void *arg) {
    ConfigManager *manager = static_cast<ConfigManager*>(arg);
    manager->nvs_writer_task();
    ESP_LOGI(TAG, "NVS writer task trampoline: task self-deleting.");
    manager->nvs_writer_task_handle_ = nullptr; // Clear handle as task is self-deleting
    vTaskDelete(nullptr); // Task self-deletes
}

void ConfigManager::nvs_writer_task() {
    ESP_LOGI(TAG, "NVS writer task started.");
    while (true) {
        if (nvs_writer_shutdown_requested_.load()) {
            ESP_LOGI(TAG, "NVS writer task: Shutdown requested, exiting.");
            break;
        }

        // Wait for a signal to save, with a timeout to periodically check shutdown flag
        if (xSemaphoreTake(nvs_save_signal_, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (nvs_writer_shutdown_requested_.load()) { // Re-check after wake-up
                ESP_LOGI(TAG, "NVS writer task: Shutdown requested after semaphore take, exiting.");
                break; 
            }

            if (config_dirty_.load()) {
                ESP_LOGI(TAG, "NVS writer task: Configuration is dirty, saving to NVS.");
                esp_err_t ret = save_to_nvs(); // This is const, so it's fine
                if (ret == ESP_OK) {
                    ESP_LOGI(TAG, "NVS writer task: Configuration saved successfully.");
                    config_dirty_.store(false); // Clear dirty flag only on successful save
                } else {
                    ESP_LOGE(TAG, "NVS writer task: Failed to save configuration to NVS: %s. Will retry on next change.", esp_err_to_name(ret));
                    // Keep dirty flag true to retry on next signal if save failed
                }
            } else {
                ESP_LOGD(TAG, "NVS writer task: Signaled, but config not dirty.");
            }
        }
        // If xSemaphoreTake timed out, the loop continues and checks nvs_writer_shutdown_requested_
    }
    ESP_LOGI(TAG, "NVS writer task finished.");
}

esp_err_t ConfigManager::flush_pending_save(TickType_t xTicksToWait)
{
    ESP_LOGI(TAG, "flush_pending_save called.");
    if (!nvs_writer_task_handle_ && !nvs_save_signal_) { 
        ESP_LOGE(TAG, "NVS writer task or semaphore not initialized, cannot flush.");
        // If task handle is null but signal exists, it means task creation failed.
        // If signal is null, constructor failed earlier.
        return ESP_FAIL; 
    }
    // Check if the task is still running, in case it crashed.
    // This is a basic check; a more robust system might use a task watchdog.
    if (nvs_writer_task_handle_ != nullptr && eTaskGetState(nvs_writer_task_handle_) == eDeleted) {
        ESP_LOGE(TAG, "NVS writer task is not running (deleted), cannot flush.");
        nvs_writer_task_handle_ = nullptr; // Mark as not running
        return ESP_FAIL;
    }


    if (config_dirty_.load()) {
        ESP_LOGI(TAG, "Flush requested: Configuration is dirty, signaling NVS writer task.");
        if (nvs_save_signal_ != nullptr) {
            xSemaphoreGive(nvs_save_signal_);
        } else {
            ESP_LOGE(TAG, "NVS save signal not available for flush.");
            return ESP_FAIL; // Should not happen if constructor succeeded
        }

        // Wait for the dirty flag to be cleared by the NVS writer task
        ESP_LOGD(TAG, "Flush: Waiting for NVS save completion...");
        TickType_t start_ticks = xTaskGetTickCount();
        while (config_dirty_.load()) {
            if ((xTaskGetTickCount() - start_ticks) >= xTicksToWait) {
                ESP_LOGW(TAG, "Timeout waiting for NVS save completion during flush.");
                return ESP_ERR_TIMEOUT; // Timed out
            }
            vTaskDelay(pdMS_TO_TICKS(20)); // Poll every 20ms
        }
        ESP_LOGI(TAG, "Flush: NVS save completed.");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Flush requested: No dirty configuration to save.");
    return ESP_OK; // No dirty data
}

esp_err_t ConfigManager::init() { // Made non-const
    ESP_LOGI(TAG, "Initializing configuration manager");

    // Try to load from NVS
    esp_err_t ret = load_from_nvs();

    // If no config exists _or_ the blob is the wrong size, create default
    if (ret == ESP_ERR_NVS_NOT_FOUND || ret == ESP_ERR_NVS_INVALID_LENGTH) {
        ESP_LOGW(TAG, "No configuration found in NVS, using defaults");

        // Set default configuration
        current_config_->num_bands = 10;
        current_config_->auto_mode = true;
        current_config_->num_antenna_ports = 6;
        current_config_->uart_baud_rate = 9600;
        current_config_->uart_parity = UART_PARITY_DISABLE;
        current_config_->uart_stop_bits = UART_STOP_BITS_1;
        current_config_->uart_flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
        current_config_->uart_rx_pin = GPIO_NUM_32; // HT1
        current_config_->uart_tx_pin = GPIO_NUM_33; // HT2

        // Whether to allow data from both uart and MQTT at the same time
        current_config_->allow_concurrent_data_sources = true;  // First come first serve

        // MQTT defaults
        current_config_->mqtt_enabled = true;  // default to enabled
        current_config_->mqtt_port = 1883;     // default MQTT port
        strncpy(current_config_->mqtt_broker, "mqtt://localhost", sizeof(current_config_->mqtt_broker));
        strncpy(current_config_->mqtt_rig_id, "rig1", sizeof(current_config_->mqtt_rig_id));
        strncpy(current_config_->mqtt_username, "mqtt", sizeof(current_config_->mqtt_username));
        strncpy(current_config_->mqtt_password, "mqtt", sizeof(current_config_->mqtt_password));
        strncpy(current_config_->mqtt_client_id, "core-mosquitto", sizeof(current_config_->mqtt_client_id));
        strncpy(current_config_->mqtt_username, "mqtt", sizeof(current_config_->mqtt_username));
        strncpy(current_config_->mqtt_password, "mqtt", sizeof(current_config_->mqtt_password));
        strncpy(current_config_->mqtt_topic, "omnirig/frequent/radio_info", sizeof(current_config_->mqtt_topic));
        
        // Default for radio operation mode and interlock
        current_config_->radio_operation_mode = RADIO_OP_MODE_SINGLE_A; // Default to Radio A only
        current_config_->interlock_auto_resolves_conflict = true; // Default to true for safety if concurrent mode is chosen
        current_config_->auto_restore_on_conflict_resolution = true; // Default to true

        // Initialize PTT input configuration defaults
        current_config_->ptt_input_radio_a = -1; // Disabled by default
        current_config_->ptt_input_radio_a_active_high = true; // Default to active high if used
        current_config_->ptt_input_radio_b = -1; // Disabled by default
        current_config_->ptt_input_radio_b_active_high = true; // Default to active high if used

        // Initialize last_used_antenna
        for (int r = 0; r < 2; ++r) {
            for (int i = 0; i < MAX_BANDS; ++i) {
                current_config_->last_used_antenna[r][i] = 0; // 0 means no preference
            }
        }

        // Initialize relay names with defaults
        for (int i = 0; i < 16; i++) {
            snprintf(current_config_->relay_names[i], sizeof(current_config_->relay_names[i]), 
                    "Relay %d", i + 1);
        }

        // Set default bands for both radios
        for (int r = 0; r < 2; ++r) {
            for (int i = 0; i < 10; ++i) {

                const uint32_t default_start[10] = {
                    1800000, 3500000, 7000000, 10100000, 14000000, 18068000, 21000000, 24890000, 28000000, 50000000
                };
                const uint32_t default_end[10] = {
                    2000000, 4000000, 7300000, 10150000, 14350000, 18168000, 21450000, 24990000, 29700000, 54000000
                };
                const char* default_band_names[10] = {
                    "160m", "80m", "40m", "30m", "20m", "17m", "15m", "12m", "10m", "6m"
                };

                strncpy(current_config_->bands[r][i].description, default_band_names[i], sizeof(current_config_->bands[r][i].description));
                current_config_->bands[r][i].start_freq = default_start[i];
                current_config_->bands[r][i].end_freq = default_end[i];
                for (int j = 0; j < MAX_ANTENNA_PORTS; ++j) {
                    current_config_->bands[r][i].antenna_ports[j] = (j == 0); // Only first port enabled by default
                }
            }
        }

        // Schedule save of default configuration
        config_dirty_.store(true);
        if (nvs_save_signal_ != nullptr && nvs_writer_task_handle_ != nullptr) {
            xSemaphoreGive(nvs_save_signal_);
            ESP_LOGI(TAG, "Default configuration set and scheduled for NVS save.");
        } else {
            ESP_LOGE(TAG, "NVS save signal or task not available. Default configuration set in memory but not scheduled for NVS save. Attempting synchronous save.");
            // Fallback to synchronous save if async mechanism is not ready
            esp_err_t sync_save_ret = save_to_nvs();
            if (sync_save_ret != ESP_OK) {
                ESP_LOGE(TAG, "Fallback synchronous save of default config failed: %s", esp_err_to_name(sync_save_ret));
                // Decide if this is a fatal error for init
            } else {
                ESP_LOGI(TAG, "Fallback synchronous save of default config successful.");
                config_dirty_.store(false); // Synchronously saved, so not dirty anymore
            }
        }
        // Assuming scheduling the save (or sync save) is success for init's perspective here.
        ret = ESP_OK; // Set ret to OK as we've handled the "not found" case by setting defaults.
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error loading configuration during init: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

esp_err_t ConfigManager::update_config(const antenna_switch_config_t &new_config) {
    ESP_LOGI(TAG, "Updating configuration");

    // Validate configuration
    if (new_config.num_bands <= 0 || new_config.num_bands > MAX_BANDS) {
        ESP_LOGE(TAG, "Invalid number of bands: %d", new_config.num_bands);
        return ESP_ERR_INVALID_ARG;
    }

    if (new_config.num_antenna_ports <= 0 || new_config.num_antenna_ports > MAX_ANTENNA_PORTS) {
        ESP_LOGE(TAG, "Invalid number of antenna ports: %d", new_config.num_antenna_ports);
        return ESP_ERR_INVALID_ARG;
    }

    // Update configuration
    *current_config_ = new_config;

    // Schedule save of new configuration to NVS
    config_dirty_.store(true);
    if (nvs_save_signal_ != nullptr && nvs_writer_task_handle_ != nullptr) {
        xSemaphoreGive(nvs_save_signal_);
        ESP_LOGI(TAG, "Configuration updated and scheduled for NVS save.");
    } else {
        ESP_LOGE(TAG, "NVS save signal or task not available. Configuration updated in memory but not scheduled for NVS save. Attempting synchronous save.");
        // Fallback to synchronous save
        esp_err_t sync_save_ret = save_to_nvs();
        if (sync_save_ret != ESP_OK) {
            ESP_LOGE(TAG, "Fallback synchronous save of updated config failed: %s", esp_err_to_name(sync_save_ret));
            // Notify observers anyway, but return the error from saving
            for (const auto &observer: observers_) {
                observer(*current_config_);
            }
            return sync_save_ret; 
        }
        ESP_LOGI(TAG, "Fallback synchronous save of updated config successful.");
        config_dirty_.store(false); // Synchronously saved
    }

    // Notify observers
    for (const auto &observer: observers_) {
        observer(*current_config_);
    }

    return ESP_OK;
}

esp_err_t ConfigManager::save_to_nvs() const {
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open("antenna_switch", NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(ret));
        return ret;
    }

    // Save all config except bands
    ret = nvs_set_blob(nvs_handle, "config", current_config_, sizeof(antenna_switch_config_t) - sizeof(current_config_->bands));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error saving configuration to NVS: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    // Save per-radio band-tables
    for (int r = 0; r < 2; ++r) {
        for (int i = 0; i < current_config_->num_bands; ++i) {
            char key[40];
            snprintf(key, sizeof(key), "band_r%d_%d_desc", r, i);
            nvs_set_str(nvs_handle, key, current_config_->bands[r][i].description);

            snprintf(key, sizeof(key), "band_r%d_%d_start", r, i);
            nvs_set_u32(nvs_handle, key, current_config_->bands[r][i].start_freq);

            snprintf(key, sizeof(key), "band_r%d_%d_end", r, i);
            nvs_set_u32(nvs_handle, key, current_config_->bands[r][i].end_freq);

            for (int j = 0; j < MAX_ANTENNA_PORTS; ++j) {
                snprintf(key, sizeof(key), "band_r%d_%d_port%d", r, i, j);
                nvs_set_u8(nvs_handle, key, current_config_->bands[r][i].antenna_ports[j] ? 1 : 0);
            }
        }
    }

    ret = nvs_commit(nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error committing NVS changes for bands: %s", esp_err_to_name(ret));
        // Continue to save other fields even if band commit fails for some reason,
        // but preserve the error.
    }

    // Save radio_operation_mode and interlock_auto_resolves_conflict
    esp_err_t radio_op_mode_err = nvs_set_u8(nvs_handle, "radio_op_mode", static_cast<uint8_t>(current_config_->radio_operation_mode));
    if (radio_op_mode_err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving radio_operation_mode: %s", esp_err_to_name(radio_op_mode_err));
        if (ret == ESP_OK) ret = radio_op_mode_err; // Preserve first error
    }

    esp_err_t interlock_err = nvs_set_u8(nvs_handle, "interlock_auto", static_cast<uint8_t>(current_config_->interlock_auto_resolves_conflict));
    if (interlock_err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving interlock_auto_resolves_conflict: %s", esp_err_to_name(interlock_err));
        if (ret == ESP_OK) ret = interlock_err; // Preserve first error
    }

    esp_err_t auto_restore_err = nvs_set_u8(nvs_handle, "ar_conflict_res", static_cast<uint8_t>(current_config_->auto_restore_on_conflict_resolution));
    if (auto_restore_err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving auto_restore_on_conflict_resolution: %s", esp_err_to_name(auto_restore_err));
        if (ret == ESP_OK) ret = auto_restore_err; // Preserve first error
    }
    
    // Final commit for the newly added fields
    esp_err_t final_commit_err = nvs_commit(nvs_handle);
    if (final_commit_err != ESP_OK) {
        ESP_LOGE(TAG, "Error committing NVS changes for radio/interlock: %s", esp_err_to_name(final_commit_err));
        if (ret == ESP_OK) ret = final_commit_err; // Preserve first error
    }

    // Save UART configuration
    esp_err_t uart_err = nvs_set_i32(nvs_handle, "uart_baud", current_config_->uart_baud_rate);
    if (uart_err != ESP_OK) ESP_LOGE(TAG, "Error saving uart_baud_rate: %s", esp_err_to_name(uart_err));
    if (ret == ESP_OK && uart_err != ESP_OK) ret = uart_err;

    uart_err = nvs_set_u8(nvs_handle, "uart_parity", current_config_->uart_parity);
    if (uart_err != ESP_OK) ESP_LOGE(TAG, "Error saving uart_parity: %s", esp_err_to_name(uart_err));
    if (ret == ESP_OK && uart_err != ESP_OK) ret = uart_err;

    uart_err = nvs_set_u8(nvs_handle, "uart_stop_b", current_config_->uart_stop_bits);
    if (uart_err != ESP_OK) ESP_LOGE(TAG, "Error saving uart_stop_bits: %s", esp_err_to_name(uart_err));
    if (ret == ESP_OK && uart_err != ESP_OK) ret = uart_err;

    uart_err = nvs_set_u8(nvs_handle, "uart_flow_c", current_config_->uart_flow_ctrl);
    if (uart_err != ESP_OK) ESP_LOGE(TAG, "Error saving uart_flow_ctrl: %s", esp_err_to_name(uart_err));
    if (ret == ESP_OK && uart_err != ESP_OK) ret = uart_err;

    uart_err = nvs_set_i32(nvs_handle, "uart_tx_pin", current_config_->uart_tx_pin);
    if (uart_err != ESP_OK) ESP_LOGE(TAG, "Error saving uart_tx_pin: %s", esp_err_to_name(uart_err));
    if (ret == ESP_OK && uart_err != ESP_OK) ret = uart_err;

    uart_err = nvs_set_i32(nvs_handle, "uart_rx_pin", current_config_->uart_rx_pin);
    if (uart_err != ESP_OK) ESP_LOGE(TAG, "Error saving uart_rx_pin: %s", esp_err_to_name(uart_err));
    if (ret == ESP_OK && uart_err != ESP_OK) ret = uart_err;

    // Save allow_concurrent_data_sources
    esp_err_t acds_err = nvs_set_u8(nvs_handle, "allow_concurr", static_cast<uint8_t>(current_config_->allow_concurrent_data_sources));
    if (acds_err != ESP_OK) ESP_LOGE(TAG, "Error saving allow_concurrent_data_sources: %s", esp_err_to_name(acds_err));
    if (ret == ESP_OK && acds_err != ESP_OK) ret = acds_err;

    // Save PTT configuration
    esp_err_t ptt_err = nvs_set_i32(nvs_handle, "ptt_in_a", current_config_->ptt_input_radio_a);
    if (ptt_err != ESP_OK) ESP_LOGE(TAG, "Error saving ptt_input_radio_a: %s", esp_err_to_name(ptt_err));
    if (ret == ESP_OK && ptt_err != ESP_OK) ret = ptt_err;

    ptt_err = nvs_set_u8(nvs_handle, "ptt_act_h_a", static_cast<uint8_t>(current_config_->ptt_input_radio_a_active_high));
    if (ptt_err != ESP_OK) ESP_LOGE(TAG, "Error saving ptt_input_radio_a_active_high: %s", esp_err_to_name(ptt_err));
    if (ret == ESP_OK && ptt_err != ESP_OK) ret = ptt_err;

    // Save Radio B PTT configuration
    ptt_err = nvs_set_i32(nvs_handle, "ptt_in_b", current_config_->ptt_input_radio_b);
    if (ptt_err != ESP_OK) ESP_LOGE(TAG, "Error saving ptt_input_radio_b: %s", esp_err_to_name(ptt_err));
    if (ret == ESP_OK && ptt_err != ESP_OK) ret = ptt_err;
    
    ptt_err = nvs_set_u8(nvs_handle, "ptt_act_h_b", static_cast<uint8_t>(current_config_->ptt_input_radio_b_active_high));
    if (ptt_err != ESP_OK) ESP_LOGE(TAG, "Error saving ptt_input_radio_b_active_high: %s", esp_err_to_name(ptt_err));
    if (ret == ESP_OK && ptt_err != ESP_OK) ret = ptt_err;

    // Save MQTT configuration
    esp_err_t mqtt_err = nvs_set_u8(nvs_handle, "mqtt_enabled", static_cast<uint8_t>(current_config_->mqtt_enabled));
    if (mqtt_err != ESP_OK) ESP_LOGE(TAG, "Error saving mqtt_enabled: %s", esp_err_to_name(mqtt_err));
    if (ret == ESP_OK && mqtt_err != ESP_OK) ret = mqtt_err;

    mqtt_err = nvs_set_str(nvs_handle, "mqtt_broker", current_config_->mqtt_broker);
    if (mqtt_err != ESP_OK) ESP_LOGE(TAG, "Error saving mqtt_broker: %s", esp_err_to_name(mqtt_err));
    if (ret == ESP_OK && mqtt_err != ESP_OK) ret = mqtt_err;

    mqtt_err = nvs_set_u16(nvs_handle, "mqtt_port", current_config_->mqtt_port);
    if (mqtt_err != ESP_OK) ESP_LOGE(TAG, "Error saving mqtt_port: %s", esp_err_to_name(mqtt_err));
    if (ret == ESP_OK && mqtt_err != ESP_OK) ret = mqtt_err;

    mqtt_err = nvs_set_str(nvs_handle, "mqtt_rig_id", current_config_->mqtt_rig_id);
    if (mqtt_err != ESP_OK) ESP_LOGE(TAG, "Error saving mqtt_rig_id: %s", esp_err_to_name(mqtt_err));
    if (ret == ESP_OK && mqtt_err != ESP_OK) ret = mqtt_err;

    mqtt_err = nvs_set_str(nvs_handle, "mqtt_user", current_config_->mqtt_username);
    if (mqtt_err != ESP_OK) ESP_LOGE(TAG, "Error saving mqtt_username: %s", esp_err_to_name(mqtt_err));
    if (ret == ESP_OK && mqtt_err != ESP_OK) ret = mqtt_err;

    mqtt_err = nvs_set_str(nvs_handle, "mqtt_pass", current_config_->mqtt_password);
    if (mqtt_err != ESP_OK) ESP_LOGE(TAG, "Error saving mqtt_password: %s", esp_err_to_name(mqtt_err));
    if (ret == ESP_OK && mqtt_err != ESP_OK) ret = mqtt_err;

    mqtt_err = nvs_set_str(nvs_handle, "mqtt_client", current_config_->mqtt_client_id);
    if (mqtt_err != ESP_OK) ESP_LOGE(TAG, "Error saving mqtt_client_id: %s", esp_err_to_name(mqtt_err));
    if (ret == ESP_OK && mqtt_err != ESP_OK) ret = mqtt_err;

    mqtt_err = nvs_set_str(nvs_handle, "mqtt_topic", current_config_->mqtt_topic);
    if (mqtt_err != ESP_OK) ESP_LOGE(TAG, "Error saving mqtt_topic: %s", esp_err_to_name(mqtt_err));
    if (ret == ESP_OK && mqtt_err != ESP_OK) ret = mqtt_err;
    
    // Commit changes for UART, ACDS, and MQTT settings
    esp_err_t extra_commit_err = nvs_commit(nvs_handle);
    if (extra_commit_err != ESP_OK) {
        ESP_LOGE(TAG, "Error committing NVS changes for UART/MQTT: %s", esp_err_to_name(extra_commit_err));
        if (ret == ESP_OK) ret = extra_commit_err; // Preserve first error
    }

    // Save last_used_antenna for each radio and band
    for (int r_idx = 0; r_idx < 2; ++r_idx) {
        for (int b_idx = 0; b_idx < MAX_BANDS; ++b_idx) {
            char key[20];
            snprintf(key, sizeof(key), "lua_r%d_b%d", r_idx, b_idx);
            esp_err_t lua_err = nvs_set_u8(nvs_handle, key, current_config_->last_used_antenna[r_idx][b_idx]);
            if (lua_err != ESP_OK) {
                ESP_LOGE(TAG, "Error saving %s: %s", key, esp_err_to_name(lua_err));
                if (ret == ESP_OK) ret = lua_err; // Preserve first error
            }
        }
    }
    
    esp_err_t lua_commit_err = nvs_commit(nvs_handle);
    if (lua_commit_err != ESP_OK) {
        ESP_LOGE(TAG, "Error committing NVS changes for last_used_antenna: %s", esp_err_to_name(lua_commit_err));
        if (ret == ESP_OK) ret = lua_commit_err; // Preserve first error
    }
    
    // Save relay names
    for (int i = 0; i < 16; i++) {
        char key[20];
        snprintf(key, sizeof(key), "relay_name_%d", i);
        esp_err_t name_err = nvs_set_str(nvs_handle, key, current_config_->relay_names[i]);
        if (name_err != ESP_OK) {
            ESP_LOGE(TAG, "Error saving relay name %d: %s", i, esp_err_to_name(name_err));
            if (ret == ESP_OK) ret = name_err; // Preserve first error
        }
    }
    
    esp_err_t names_commit_err = nvs_commit(nvs_handle);
    if (names_commit_err != ESP_OK) {
        ESP_LOGE(TAG, "Error committing NVS changes for relay names: %s", esp_err_to_name(names_commit_err));
        if (ret == ESP_OK) ret = names_commit_err; // Preserve first error
    }

    nvs_close(nvs_handle);
    return ret;
}

esp_err_t ConfigManager::load_from_nvs() const {
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open("antenna_switch", NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(ret));
        return ret;
    }

    // Load all config except bands
    size_t required_size = sizeof(antenna_switch_config_t) - sizeof(current_config_->bands);
    ret = nvs_get_blob(nvs_handle, "config", current_config_, &required_size);
    if (ret != ESP_OK) {
        nvs_close(nvs_handle);
        return ret;
    }

    // Load per-radio band-tables
    for (int r = 0; r < 2; ++r) {
        // Ensure num_bands is valid before using it in loop
        if (current_config_->num_bands > 0 && current_config_->num_bands <= MAX_BANDS) {
            for (int i = 0; i < current_config_->num_bands; ++i) {
                char key[40];
                size_t len = sizeof(current_config_->bands[r][i].description);
                snprintf(key, sizeof(key), "band_r%d_%d_desc", r, i);
                // ESP_LOGD(TAG, "Loading NVS key: %s", key);
                esp_err_t band_err = nvs_get_str(nvs_handle, key, current_config_->bands[r][i].description, &len);
                if (band_err == ESP_ERR_NVS_NOT_FOUND) {
                    // ESP_LOGW(TAG, "Band description not found for radio %d, band %d. Using default.", r, i);
                    // Default already set by init or blob load, or could set specific default here
                }


                snprintf(key, sizeof(key), "band_r%d_%d_start", r, i);
                nvs_get_u32(nvs_handle, key, &current_config_->bands[r][i].start_freq);

                snprintf(key, sizeof(key), "band_r%d_%d_end", r, i);
                nvs_get_u32(nvs_handle, key, &current_config_->bands[r][i].end_freq);

                for (int j = 0; j < MAX_ANTENNA_PORTS; ++j) {
                    snprintf(key, sizeof(key), "band_r%d_%d_port%d", r, i, j);
                    uint8_t val = 0; // Default to false if not found
                    nvs_get_u8(nvs_handle, key, &val);
                    current_config_->bands[r][i].antenna_ports[j] = (val != 0);
                }
            }
        } else {
            ESP_LOGW(TAG, "Invalid num_bands (%d) found in loaded config, skipping band data load.", current_config_->num_bands);
            // Potentially reset num_bands to a safe default if it's out of whack
            // current_config_->num_bands = 10; // Or some other default
        }
    }

    // Load radio_operation_mode
    uint8_t radio_op_mode_val;
    esp_err_t err_rom = nvs_get_u8(nvs_handle, "radio_op_mode", &radio_op_mode_val);
    if (err_rom == ESP_OK) {
        current_config_->radio_operation_mode = static_cast<radio_operation_mode_t>(radio_op_mode_val);
    } else if (err_rom == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "radio_operation_mode not found in NVS, using default (SINGLE_A).");
        current_config_->radio_operation_mode = RADIO_OP_MODE_SINGLE_A;
    } else {
        ESP_LOGE(TAG, "Error loading radio_operation_mode: %s", esp_err_to_name(err_rom));
        // Keep existing value or default if error
    }

    // Load interlock_auto_resolves_conflict
    uint8_t interlock_val;
    esp_err_t err_ia = nvs_get_u8(nvs_handle, "interlock_auto", &interlock_val);
    if (err_ia == ESP_OK) {
        current_config_->interlock_auto_resolves_conflict = static_cast<bool>(interlock_val);
    } else if (err_ia == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "interlock_auto_resolves_conflict not found in NVS, using default (true).");
        current_config_->interlock_auto_resolves_conflict = true;
    } else {
        ESP_LOGE(TAG, "Error loading interlock_auto_resolves_conflict: %s", esp_err_to_name(err_ia));
        // Keep existing value or default if error
    }

    // Load auto_restore_on_conflict_resolution
    uint8_t auto_restore_val;
    esp_err_t err_arocr = nvs_get_u8(nvs_handle, "ar_conflict_res", &auto_restore_val);
    if (err_arocr == ESP_OK) {
        current_config_->auto_restore_on_conflict_resolution = static_cast<bool>(auto_restore_val);
    } else if (err_arocr == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "auto_restore_on_conflict_resolution (key ar_conflict_res) not found in NVS, using default (true).");
        current_config_->auto_restore_on_conflict_resolution = true;
    } else {
        ESP_LOGE(TAG, "Error loading auto_restore_on_conflict_resolution (key ar_conflict_res): %s", esp_err_to_name(err_arocr));
        // Keep existing value or default if error
    }

    // Load UART configuration
    int32_t temp_i32_val; // Temporary variable for nvs_get_i32
    esp_err_t uart_err = nvs_get_i32(nvs_handle, "uart_baud", &temp_i32_val);
    if (uart_err == ESP_OK) {
        current_config_->uart_baud_rate = temp_i32_val;
    } else if (uart_err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "uart_baud_rate not found, using default.");
    } else {
        ESP_LOGE(TAG, "Error loading uart_baud_rate: %s", esp_err_to_name(uart_err));
    }
    
    uart_err = nvs_get_u8(nvs_handle, "uart_parity", &current_config_->uart_parity);
    if (uart_err == ESP_ERR_NVS_NOT_FOUND) ESP_LOGW(TAG, "uart_parity not found, using default.");
    else if (uart_err != ESP_OK) ESP_LOGE(TAG, "Error loading uart_parity: %s", esp_err_to_name(uart_err));

    uart_err = nvs_get_u8(nvs_handle, "uart_stop_b", &current_config_->uart_stop_bits);
    if (uart_err == ESP_ERR_NVS_NOT_FOUND) ESP_LOGW(TAG, "uart_stop_bits not found, using default.");
    else if (uart_err != ESP_OK) ESP_LOGE(TAG, "Error loading uart_stop_bits: %s", esp_err_to_name(uart_err));

    uart_err = nvs_get_u8(nvs_handle, "uart_flow_c", &current_config_->uart_flow_ctrl);
    if (uart_err == ESP_ERR_NVS_NOT_FOUND) ESP_LOGW(TAG, "uart_flow_ctrl not found, using default.");
    else if (uart_err != ESP_OK) ESP_LOGE(TAG, "Error loading uart_flow_ctrl: %s", esp_err_to_name(uart_err));

    uart_err = nvs_get_i32(nvs_handle, "uart_tx_pin", &temp_i32_val);
    if (uart_err == ESP_OK) {
        current_config_->uart_tx_pin = static_cast<int8_t>(temp_i32_val);
    } else if (uart_err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "uart_tx_pin not found, using default.");
    } else {
        ESP_LOGE(TAG, "Error loading uart_tx_pin: %s", esp_err_to_name(uart_err));
    }

    uart_err = nvs_get_i32(nvs_handle, "uart_rx_pin", &temp_i32_val);
    if (uart_err == ESP_OK) {
        current_config_->uart_rx_pin = static_cast<int8_t>(temp_i32_val);
    } else if (uart_err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "uart_rx_pin not found, using default.");
    } else {
        ESP_LOGE(TAG, "Error loading uart_rx_pin: %s", esp_err_to_name(uart_err));
    }

    // Load allow_concurrent_data_sources
    uint8_t acds_val;
    esp_err_t acds_err = nvs_get_u8(nvs_handle, "allow_concurr", &acds_val);
    if (acds_err == ESP_OK) current_config_->allow_concurrent_data_sources = static_cast<bool>(acds_val);
    else if (acds_err == ESP_ERR_NVS_NOT_FOUND) ESP_LOGW(TAG, "allow_concurrent_data_sources not found, using default."); // Default is true from init
    else ESP_LOGE(TAG, "Error loading allow_concurrent_data_sources: %s", esp_err_to_name(acds_err));

    // Load PTT configuration
    int32_t ptt_in_a_val;
    esp_err_t ptt_err = nvs_get_i32(nvs_handle, "ptt_in_a", &ptt_in_a_val);
    if (ptt_err == ESP_OK) current_config_->ptt_input_radio_a = ptt_in_a_val;
    else if (ptt_err == ESP_ERR_NVS_NOT_FOUND) { ESP_LOGW(TAG, "ptt_input_radio_a not found, using default (-1)."); current_config_->ptt_input_radio_a = -1; }
    else ESP_LOGE(TAG, "Error loading ptt_input_radio_a: %s", esp_err_to_name(ptt_err));

    uint8_t ptt_act_h_a_val;
    ptt_err = nvs_get_u8(nvs_handle, "ptt_act_h_a", &ptt_act_h_a_val);
    if (ptt_err == ESP_OK) current_config_->ptt_input_radio_a_active_high = static_cast<bool>(ptt_act_h_a_val);
    else if (ptt_err == ESP_ERR_NVS_NOT_FOUND) { ESP_LOGW(TAG, "ptt_input_radio_a_active_high not found, using default (true)."); current_config_->ptt_input_radio_a_active_high = true; }
    else ESP_LOGE(TAG, "Error loading ptt_input_radio_a_active_high: %s", esp_err_to_name(ptt_err));
    
    // Load Radio B PTT configuration
    int32_t ptt_in_b_val;
    ptt_err = nvs_get_i32(nvs_handle, "ptt_in_b", &ptt_in_b_val);
    if (ptt_err == ESP_OK) current_config_->ptt_input_radio_b = ptt_in_b_val;
    else if (ptt_err == ESP_ERR_NVS_NOT_FOUND) { ESP_LOGW(TAG, "ptt_input_radio_b not found, using default (-1)."); current_config_->ptt_input_radio_b = -1; }
    else ESP_LOGE(TAG, "Error loading ptt_input_radio_b: %s", esp_err_to_name(ptt_err));
    
    uint8_t ptt_act_h_b_val;
    ptt_err = nvs_get_u8(nvs_handle, "ptt_act_h_b", &ptt_act_h_b_val);
    if (ptt_err == ESP_OK) current_config_->ptt_input_radio_b_active_high = static_cast<bool>(ptt_act_h_b_val);
    else if (ptt_err == ESP_ERR_NVS_NOT_FOUND) { ESP_LOGW(TAG, "ptt_input_radio_b_active_high not found, using default (true)."); current_config_->ptt_input_radio_b_active_high = true; }
    else ESP_LOGE(TAG, "Error loading ptt_input_radio_b_active_high: %s", esp_err_to_name(ptt_err));

    // Load MQTT configuration
    uint8_t mqtt_enabled_val;
    esp_err_t mqtt_err = nvs_get_u8(nvs_handle, "mqtt_enabled", &mqtt_enabled_val);
    if (mqtt_err == ESP_OK) current_config_->mqtt_enabled = static_cast<bool>(mqtt_enabled_val);
    else if (mqtt_err == ESP_ERR_NVS_NOT_FOUND) ESP_LOGW(TAG, "mqtt_enabled not found, using default."); // Default is true from init
    else ESP_LOGE(TAG, "Error loading mqtt_enabled: %s", esp_err_to_name(mqtt_err));

    size_t len;
    len = sizeof(current_config_->mqtt_broker);
    mqtt_err = nvs_get_str(nvs_handle, "mqtt_broker", current_config_->mqtt_broker, &len);
    if (mqtt_err == ESP_ERR_NVS_NOT_FOUND) { ESP_LOGW(TAG, "mqtt_broker not found, using default."); /* Default from init */ }
    else if (mqtt_err != ESP_OK) ESP_LOGE(TAG, "Error loading mqtt_broker: %s", esp_err_to_name(mqtt_err));
    // Ensure null termination if string was loaded and filled buffer
    else if (len == sizeof(current_config_->mqtt_broker)) current_config_->mqtt_broker[len-1] = '\0';


    mqtt_err = nvs_get_u16(nvs_handle, "mqtt_port", &current_config_->mqtt_port);
    if (mqtt_err == ESP_ERR_NVS_NOT_FOUND) ESP_LOGW(TAG, "mqtt_port not found, using default."); // Default from init
    else if (mqtt_err != ESP_OK) ESP_LOGE(TAG, "Error loading mqtt_port: %s", esp_err_to_name(mqtt_err));

    len = sizeof(current_config_->mqtt_rig_id);
    mqtt_err = nvs_get_str(nvs_handle, "mqtt_rig_id", current_config_->mqtt_rig_id, &len);
    if (mqtt_err == ESP_ERR_NVS_NOT_FOUND) { ESP_LOGW(TAG, "mqtt_rig_id not found, using default."); /* Default from init */ }
    else if (mqtt_err != ESP_OK) ESP_LOGE(TAG, "Error loading mqtt_rig_id: %s", esp_err_to_name(mqtt_err));
    else if (len == sizeof(current_config_->mqtt_rig_id)) current_config_->mqtt_rig_id[len-1] = '\0';

    len = sizeof(current_config_->mqtt_username);
    mqtt_err = nvs_get_str(nvs_handle, "mqtt_user", current_config_->mqtt_username, &len);
    if (mqtt_err == ESP_ERR_NVS_NOT_FOUND) { ESP_LOGW(TAG, "mqtt_username not found, using default."); /* Default from init */ }
    else if (mqtt_err != ESP_OK) ESP_LOGE(TAG, "Error loading mqtt_username: %s", esp_err_to_name(mqtt_err));
    else if (len == sizeof(current_config_->mqtt_username)) current_config_->mqtt_username[len-1] = '\0';

    len = sizeof(current_config_->mqtt_password);
    mqtt_err = nvs_get_str(nvs_handle, "mqtt_pass", current_config_->mqtt_password, &len);
    if (mqtt_err == ESP_ERR_NVS_NOT_FOUND) { ESP_LOGW(TAG, "mqtt_password not found, using default."); /* Default from init */ }
    else if (mqtt_err != ESP_OK) ESP_LOGE(TAG, "Error loading mqtt_password: %s", esp_err_to_name(mqtt_err));
    else if (len == sizeof(current_config_->mqtt_password)) current_config_->mqtt_password[len-1] = '\0';

    len = sizeof(current_config_->mqtt_client_id);
    mqtt_err = nvs_get_str(nvs_handle, "mqtt_client", current_config_->mqtt_client_id, &len);
    if (mqtt_err == ESP_ERR_NVS_NOT_FOUND) { ESP_LOGW(TAG, "mqtt_client_id not found, using default."); /* Default from init */ }
    else if (mqtt_err != ESP_OK) ESP_LOGE(TAG, "Error loading mqtt_client_id: %s", esp_err_to_name(mqtt_err));
    else if (len == sizeof(current_config_->mqtt_client_id)) current_config_->mqtt_client_id[len-1] = '\0';

    len = sizeof(current_config_->mqtt_topic);
    mqtt_err = nvs_get_str(nvs_handle, "mqtt_topic", current_config_->mqtt_topic, &len);
    if (mqtt_err == ESP_ERR_NVS_NOT_FOUND) { ESP_LOGW(TAG, "mqtt_topic not found, using default."); /* Default from init */ }
    else if (mqtt_err != ESP_OK) ESP_LOGE(TAG, "Error loading mqtt_topic: %s", esp_err_to_name(mqtt_err));
    else if (len == sizeof(current_config_->mqtt_topic)) current_config_->mqtt_topic[len-1] = '\0';

    // Load last_used_antenna for each radio and band
    for (int r_idx = 0; r_idx < 2; ++r_idx) {
        for (int b_idx = 0; b_idx < MAX_BANDS; ++b_idx) {
            char key[20];
            snprintf(key, sizeof(key), "lua_r%d_b%d", r_idx, b_idx);
            uint8_t val = 0; // Default to 0 (no preference)
            esp_err_t lua_err = nvs_get_u8(nvs_handle, key, &val);
            if (lua_err == ESP_OK) {
                current_config_->last_used_antenna[r_idx][b_idx] = val;
            } else if (lua_err == ESP_ERR_NVS_NOT_FOUND) {
                current_config_->last_used_antenna[r_idx][b_idx] = 0; // Explicitly set default
            } else {
                ESP_LOGE(TAG, "Error loading %s: %s. Defaulting to 0.", key, esp_err_to_name(lua_err));
                current_config_->last_used_antenna[r_idx][b_idx] = 0; // Default on error
            }
        }
    }

    // Load relay names
    for (int i = 0; i < 16; i++) {
        char key[20];
        snprintf(key, sizeof(key), "relay_name_%d", i);
        size_t len = sizeof(current_config_->relay_names[i]);
        esp_err_t name_err = nvs_get_str(nvs_handle, key, current_config_->relay_names[i], &len);
        
        if (name_err == ESP_ERR_NVS_NOT_FOUND) {
            // If name not found, set default
            snprintf(current_config_->relay_names[i], sizeof(current_config_->relay_names[i]), 
                    "Relay %d", i + 1);
        } else if (name_err != ESP_OK) {
            ESP_LOGE(TAG, "Error loading relay name %d: %s", i, esp_err_to_name(name_err));
            // Set default on error
            snprintf(current_config_->relay_names[i], sizeof(current_config_->relay_names[i]), 
                    "Relay %d", i + 1);
        }
        // Ensure null termination
        else if (len == sizeof(current_config_->relay_names[i])) {
            current_config_->relay_names[i][len-1] = '\0';
        }
    }

    nvs_close(nvs_handle);
    return ESP_OK;
}

void ConfigManager::add_observer(const std::function<void(const antenna_switch_config_t &)> &observer) {
    observers_.push_back(observer);
    // Immediately notify the new observer of current config
    observer(*current_config_);
}
