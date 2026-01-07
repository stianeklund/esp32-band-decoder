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
#include "cJSON.h"
#include "esp_timer.h" // For esp_timer_get_time()

static constexpr const char* TAG = "CONFIG_MANAGER";

// Initialize static member
ConfigManager *ConfigManager::instance_ = nullptr;

ConfigManager::ConfigManager() : current_config_(new antenna_switch_config_t()), config_dirty_(false), nvs_writer_shutdown_requested_(false), nvs_writer_task_handle_(nullptr), nvs_save_signal_(nullptr) {
    if (instance_ == nullptr) {
        instance_ = this;
    }

    esp_log_level_set("RELAY_CONTROLLER",ESP_LOG_WARN);

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
        ESP_LOGD(TAG, "NVS writer task created successfully.");
    }
}

ConfigManager::~ConfigManager() {
    ESP_LOGV(TAG, "Shutting down ConfigManager and NVS writer task.");
    nvs_writer_shutdown_requested_.store(true);

    if (nvs_save_signal_ != nullptr) {
        xSemaphoreGive(nvs_save_signal_); // Wake up the task so it can see the shutdown flag
    }

    // Wait a bit for the task to shut down. A more robust way would be for the task
    // to signal back its termination, or use eTaskGetState to check.
    // For now, assuming it self-deletes as per nvs_writer_task_trampoline.
    if (nvs_writer_task_handle_ != nullptr) {
        ESP_LOGV(TAG, "Waiting for NVS writer task to terminate...");
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
    ESP_LOGD(TAG, "ConfigManager cleanup complete.");
}

ConfigManager &ConfigManager::instance() {
    static ConfigManager instance; // Meyers' singleton
    return instance;
}

void ConfigManager::nvs_writer_task_trampoline(void *arg) {
    ConfigManager *manager = static_cast<ConfigManager*>(arg);
    manager->nvs_writer_task();
    ESP_LOGV(TAG, "NVS writer task trampoline: task self-deleting.");
    manager->nvs_writer_task_handle_ = nullptr; // Clear handle as task is self-deleting
    vTaskDelete(nullptr); // Task self-deletes
}

void ConfigManager::nvs_writer_task() {
    ESP_LOGD(TAG, "NVS writer task started.");
    while (true) {
        if (nvs_writer_shutdown_requested_.load()) {
            ESP_LOGV(TAG, "NVS writer task: Shutdown requested, exiting.");
            break;
        }

        // Wait for a signal to save, with a timeout to periodically check shutdown flag
        if (xSemaphoreTake(nvs_save_signal_, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (nvs_writer_shutdown_requested_.load()) { // Re-check after wake-up
                ESP_LOGV(TAG, "NVS writer task: Shutdown requested after semaphore take, exiting.");
                break; 
            }

            if (config_dirty_.load()) {
                ESP_LOGD(TAG, "NVS writer task: Configuration is dirty, saving to NVS.");
                esp_err_t ret = save_to_nvs(); // This is const, so it's fine
                if (ret == ESP_OK) {
                    ESP_LOGD(TAG, "NVS writer task: Configuration saved successfully.");
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
    ESP_LOGD(TAG, "NVS writer task finished.");
}

esp_err_t ConfigManager::flush_pending_save(TickType_t xTicksToWait)
{
    ESP_LOGV(TAG, "flush_pending_save called.");
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
        ESP_LOGD(TAG, "Flush requested: Configuration is dirty, signaling NVS writer task.");
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
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        ESP_LOGD(TAG, "Flush: NVS save completed.");
        return ESP_OK;
    }
    ESP_LOGD(TAG, "Flush requested: No dirty configuration to save.");
    return ESP_OK; // No dirty data
}

esp_err_t ConfigManager::init() { // Made non-const
    ESP_LOGI(TAG, "Initializing configuration manager");

    // Define these outside the loops to ensure they are not on the stack repeatedly.
    // Sorted by frequency: HIGH to LOW (6m -> 160m)
    static const uint32_t default_start_init[10] = {
        50000000, 28000000, 24890000, 21000000, 18068000, 14000000, 10100000, 7000000, 3500000, 1800000
    };
    static const uint32_t default_end_init[10] = {
        54000000, 29700000, 24990000, 21450000, 18168000, 14350000, 10150000, 7300000, 4000000, 2000000
    };
    static const char* default_band_names_init[10] = {
        "6m", "10m", "12m", "15m", "17m", "20m", "30m", "40m", "80m", "160m"
    };

    // Try to load from NVS
    esp_err_t ret = load_from_nvs();

    // If no config exists _or_ the blob is the wrong size, create default
    if (ret == ESP_ERR_NVS_NOT_FOUND || ret == ESP_ERR_NVS_INVALID_LENGTH) {
        ESP_LOGW(TAG, "No configuration found in NVS, using defaults");

        // Set default configuration
        current_config_->num_bands = 8;
        current_config_->auto_mode = true;
        current_config_->ai_mode = false;
        current_config_->num_antenna_ports = 6;
        current_config_->uart_baud_rate = 57600;
        current_config_->uart_parity = UART_PARITY_DISABLE;
        current_config_->uart_stop_bits = UART_STOP_BITS_1;
        current_config_->uart_flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
        current_config_->uart_rx_pin = GPIO_NUM_32; // HT1
        current_config_->uart_tx_pin = GPIO_NUM_33; // HT2

        // Whether to allow data from both uart and MQTT at the same time
        current_config_->allow_concurrent_data_sources = true;  // First come first serve

        // MQTT defaults
        current_config_->mqtt_enabled = false;  // default to disabled
        
        // WebSocket defaults
        current_config_->websocket_enabled = false;  // default to disabled for safety
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
        current_config_->radio_restore_delay_ms = 200; // Default interlock restore delay

        // Initialize PTT input configuration defaults
        current_config_->ptt_input_radio_a = -1; // Disabled by default
        current_config_->ptt_input_radio_a_active_high = true; // Default to active high if used
        current_config_->ptt_input_radio_b = -1; // Disabled by default
        current_config_->ptt_input_radio_b_active_high = true; // Default to active high if used

        // Initialize last_used_antenna
        for (auto & r : current_config_->last_used_antenna) {
            for (unsigned char & i : r) {
                i = 0; // 0 means no preference
            }
        }

        // Initialize relay names with defaults
        for (int i = 0; i < 16; i++) {
            snprintf(current_config_->relay_names[i], sizeof(current_config_->relay_names[i]), 
                    "Relay %d", i + 1);
        }

        // Set default bands for both radios (sorted by frequency: high to low)
        for (auto & band : current_config_->bands) {
            for (int i = 0; i < current_config_->num_bands && i < 10; ++i) {
                strncpy(band[i].description, default_band_names_init[i], sizeof(band[i].description)-1);
                band[i].description[sizeof(band[i].description)-1] = '\0';
                band[i].start_freq = default_start_init[i];
                band[i].end_freq = default_end_init[i];
                for (int j = 0; j < MAX_ANTENNA_PORTS; ++j) {
                    band[i].antenna_ports[j] = false; // No ports enabled by default
                }
            }
            // Initialize any remaining bands beyond the predefined ones
            for (int i = 10; i < MAX_BANDS; ++i) {
                snprintf(band[i].description, sizeof(band[i].description), "Band %d", i + 1);
                band[i].description[sizeof(band[i].description)-1] = '\0';
                band[i].start_freq = 0;
                band[i].end_freq = 0;
                for (int j = 0; j < MAX_ANTENNA_PORTS; ++j) {
                    band[i].antenna_ports[j] = false;
                }
            }
        }

        // Schedule save of default configuration
        config_dirty_.store(true);
        if (nvs_save_signal_ != nullptr && nvs_writer_task_handle_ != nullptr) {
            xSemaphoreGive(nvs_save_signal_);
            ESP_LOGD(TAG, "Default configuration set and scheduled for NVS save.");
        } else {
            ESP_LOGE(TAG, "NVS save signal or task not available. Default configuration set in memory but not scheduled for NVS save. Attempting synchronous save.");
            // Fallback to synchronous save if async mechanism is not ready
            esp_err_t sync_save_ret = save_to_nvs();
            if (sync_save_ret != ESP_OK) {
                ESP_LOGE(TAG, "Fallback synchronous save of default config failed: %s", esp_err_to_name(sync_save_ret));
                // Decide if this is a fatal error for init
            } else {
                ESP_LOGV(TAG, "Fallback synchronous save of default config successful.");
                config_dirty_.store(false); // Synchronously saved, so not dirty anymore
            }
        }
        // Assuming scheduling the save (or sync save) is success for init's perspective here.
        ret = ESP_OK; // Set ret to OK as we've handled the "not found" case by setting defaults.
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error loading configuration during init: %s", esp_err_to_name(ret));
        return ret;
    }

    // Increment version to ensure caches are initialized properly
    config_version_.fetch_add(1);

    return ESP_OK;
}

esp_err_t ConfigManager::reset_to_defaults() {
    ESP_LOGI(TAG, "Resetting configuration to defaults");
    antenna_switch_config_t defaultConfig;

    // Populate defaultConfig with the same values used in init() when NVS is not found
    // This logic is largely copied from the init() method's default section.
    defaultConfig.num_bands = 8;
    defaultConfig.auto_mode = true;
    defaultConfig.ai_mode = false;
    defaultConfig.num_antenna_ports = 6;
    defaultConfig.uart_baud_rate = 57600;
    defaultConfig.uart_parity = UART_PARITY_DISABLE;
    defaultConfig.uart_stop_bits = UART_STOP_BITS_1;
    defaultConfig.uart_flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    defaultConfig.uart_rx_pin = GPIO_NUM_32; // HT1
    defaultConfig.uart_tx_pin = GPIO_NUM_33; // HT2
    defaultConfig.allow_concurrent_data_sources = true;
    defaultConfig.mqtt_enabled = false;
    defaultConfig.websocket_enabled = false; // Default to disabled for safety
    defaultConfig.mqtt_port = 1883;
    strncpy(defaultConfig.mqtt_broker, "mqtt://localhost", sizeof(defaultConfig.mqtt_broker) - 1);
    defaultConfig.mqtt_broker[sizeof(defaultConfig.mqtt_broker) - 1] = '\0';
    strncpy(defaultConfig.mqtt_rig_id, "rig1", sizeof(defaultConfig.mqtt_rig_id) - 1);
    defaultConfig.mqtt_rig_id[sizeof(defaultConfig.mqtt_rig_id) - 1] = '\0';
    strncpy(defaultConfig.mqtt_username, "mqtt", sizeof(defaultConfig.mqtt_username) - 1);
    defaultConfig.mqtt_username[sizeof(defaultConfig.mqtt_username) - 1] = '\0';
    strncpy(defaultConfig.mqtt_password, "mqtt", sizeof(defaultConfig.mqtt_password) - 1);
    defaultConfig.mqtt_password[sizeof(defaultConfig.mqtt_password) - 1] = '\0';
    strncpy(defaultConfig.mqtt_client_id, "core-mosquitto", sizeof(defaultConfig.mqtt_client_id) - 1);
    defaultConfig.mqtt_client_id[sizeof(defaultConfig.mqtt_client_id) - 1] = '\0';
    strncpy(defaultConfig.mqtt_topic, "omnirig/frequent/radio_info", sizeof(defaultConfig.mqtt_topic) - 1);
    defaultConfig.mqtt_topic[sizeof(defaultConfig.mqtt_topic) - 1] = '\0';
    defaultConfig.radio_operation_mode = RADIO_OP_MODE_SINGLE_A;
    defaultConfig.interlock_auto_resolves_conflict = true;
    defaultConfig.auto_restore_on_conflict_resolution = true;
    defaultConfig.radio_restore_delay_ms = 200;
    defaultConfig.ptt_input_radio_a = -1;
    defaultConfig.ptt_input_radio_a_active_high = true;
    defaultConfig.ptt_input_radio_b = -1;
    defaultConfig.ptt_input_radio_b_active_high = true;

    for (auto & r : defaultConfig.last_used_antenna) {
        for (unsigned char & i : r) {
            i = 0;
        }
    }

    for (int i = 0; i < 16; i++) {
        snprintf(defaultConfig.relay_names[i], sizeof(defaultConfig.relay_names[i]), "Relay %d", i + 1);
        defaultConfig.relay_names[i][sizeof(defaultConfig.relay_names[i])-1] = '\0';
    }

    // Sorted by frequency: HIGH to LOW (6m -> 160m)
    static const uint32_t default_start_init[10] = {
        50000000, 28000000, 24890000, 21000000, 18068000, 14000000, 10100000, 7000000, 3500000, 1800000
    };
    static const uint32_t default_end_init[10] = {
        54000000, 29700000, 24990000, 21450000, 18168000, 14350000, 10150000, 7300000, 4000000, 2000000
    };
    static const char* default_band_names_init[10] = {
        "6m", "10m", "12m", "15m", "17m", "20m", "30m", "40m", "80m", "160m"
    };

    // Set default bands for both radios (sorted by frequency: high to low)
    for (auto & band_radio_set : defaultConfig.bands) { // Iterate over Radio A and Radio B bands
        for (int i = 0; i < defaultConfig.num_bands && i < 10; ++i) { // Initialize bands with specific defaults
            strncpy(band_radio_set[i].description, default_band_names_init[i], sizeof(band_radio_set[i].description)-1);
            band_radio_set[i].description[sizeof(band_radio_set[i].description)-1] = '\0';
            band_radio_set[i].start_freq = default_start_init[i];
            band_radio_set[i].end_freq = default_end_init[i];
            for (int j = 0; j < MAX_ANTENNA_PORTS; ++j) {
                band_radio_set[i].antenna_ports[j] = false; // No ports enabled by default
            }
        }
        // Initialize any remaining bands (if MAX_BANDS > 10) to a generic state
        for (int i = 10; i < MAX_BANDS; ++i) {
            snprintf(band_radio_set[i].description, sizeof(band_radio_set[i].description), "Band %d", i + 1);
            band_radio_set[i].description[sizeof(band_radio_set[i].description)-1] = '\0';
            band_radio_set[i].start_freq = 0;
            band_radio_set[i].end_freq = 0;
            for (int j = 0; j < MAX_ANTENNA_PORTS; ++j) {
                band_radio_set[i].antenna_ports[j] = false;
            }
        }
    }

    // Now update the current configuration with these defaults and save to NVS
    return update_config(defaultConfig);
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
        ESP_LOGD(TAG, "Fallback synchronous save of updated config successful.");
        config_dirty_.store(false); // Synchronously saved
    }

    // Increment version counter to invalidate caches
    config_version_.fetch_add(1);

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

    // Prepare and save the base configuration data
    base_nvs_config_data_t base_data_to_save;
    base_data_to_save.auto_mode = current_config_->auto_mode;
    base_data_to_save.ai_mode = current_config_->ai_mode;
    base_data_to_save.allow_concurrent_data_sources = current_config_->allow_concurrent_data_sources;
    base_data_to_save.websocket_enabled = current_config_->websocket_enabled;
    base_data_to_save.num_bands = current_config_->num_bands;
    base_data_to_save.num_antenna_ports = current_config_->num_antenna_ports;
    base_data_to_save.radio_operation_mode = current_config_->radio_operation_mode;
    memcpy(base_data_to_save.last_used_antenna, current_config_->last_used_antenna, sizeof(base_data_to_save.last_used_antenna));

    // Save the new base config blob, replacing the old "config" blob logic for these fields
    esp_err_t base_save_err = nvs_set_blob(nvs_handle, "config_base", &base_data_to_save, sizeof(base_nvs_config_data_t));
    if (base_save_err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving base config (config_base): %s", esp_err_to_name(base_save_err));
        nvs_close(nvs_handle);
        return base_save_err; // Return early if base config fails to save
    }
    // Commit this critical part
    esp_err_t base_commit_err = nvs_commit(nvs_handle);
    if (base_commit_err != ESP_OK) {
        ESP_LOGE(TAG, "Error committing NVS for base config: %s", esp_err_to_name(base_commit_err));
        // Preserve first error, but this is critical
        if (ret == ESP_OK) ret = base_commit_err; 
    }
    // Note: The old "config" blob will remain in NVS unless explicitly erased or overwritten.
    // For a clean transition, an NVS erase might be needed once.

    // Save per-radio band-tables as blobs
    for (int r = 0; r < 2; ++r) {
        // Validate num_bands before using it to avoid writing garbage or too many bands
        uint8_t num_bands_to_save = current_config_->num_bands;
        if (num_bands_to_save > MAX_BANDS) {
            ESP_LOGW(TAG, "num_bands (%d) exceeds MAX_BANDS (%d) for radio %d. Clamping to MAX_BANDS.", num_bands_to_save, MAX_BANDS, r);
            num_bands_to_save = MAX_BANDS;
        }
        for (int i = 0; i < num_bands_to_save; ++i) {
            char key[24]; // Key like "band_cfg_r0_b0"
            snprintf(key, sizeof(key), "band_cfg_r%d_b%d", r, i);
            esp_err_t band_save_err = nvs_set_blob(nvs_handle, key, &current_config_->bands[r][i], sizeof(band_config_t));
            if (band_save_err != ESP_OK) {
                ESP_LOGE(TAG, "Error saving band config blob for %s: %s", key, esp_err_to_name(band_save_err));
                if (ret == ESP_OK) ret = band_save_err; // Preserve first error
            }
        }
    }

    // Commit changes for bands (and potentially other items saved before this)
    esp_err_t bands_commit_err = nvs_commit(nvs_handle);
    if (bands_commit_err != ESP_OK) {
        ESP_LOGE(TAG, "Error committing NVS changes after bands: %s", esp_err_to_name(bands_commit_err));
        if (ret == ESP_OK) ret = bands_commit_err;
    }

    // radio_operation_mode is now part of "config_base"
    // Save interlock_auto_resolves_conflict, auto_restore_on_conflict_resolution, radio_restore_delay_ms
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

    esp_err_t restore_delay_err = nvs_set_u16(nvs_handle, "restore_delay", current_config_->radio_restore_delay_ms);
    if (restore_delay_err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving radio_restore_delay_ms: %s", esp_err_to_name(restore_delay_err));
        if (ret == ESP_OK) ret = restore_delay_err;
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
    
    // Save relay names as a single blob
    esp_err_t names_save_err = nvs_set_blob(nvs_handle, "relay_names_all", current_config_->relay_names, sizeof(current_config_->relay_names));
    if (names_save_err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving relay_names_all blob: %s", esp_err_to_name(names_save_err));
        if (ret == ESP_OK) ret = names_save_err; // Preserve first error
    }
    
    esp_err_t names_commit_err = nvs_commit(nvs_handle); // Commit after this change
    if (names_commit_err != ESP_OK) {
        ESP_LOGE(TAG, "Error committing NVS changes for relay names blob: %s", esp_err_to_name(names_commit_err));
        if (ret == ESP_OK) ret = names_commit_err;
    }

    nvs_close(nvs_handle);
    return ret;
}

esp_err_t ConfigManager::load_from_nvs() const {
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open("antenna_switch", NVS_READWRITE, &nvs_handle); // NVS_READONLY might be better if only loading
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(ret));
        return ret;
    }

    // Load the new base configuration blob
    base_nvs_config_data_t loaded_base_data;
    size_t base_data_size = sizeof(base_nvs_config_data_t);
    esp_err_t base_load_err = nvs_get_blob(nvs_handle, "config_base", &loaded_base_data, &base_data_size);

    if (base_load_err == ESP_OK) {
        if (base_data_size == sizeof(base_nvs_config_data_t)) {
            // Populate current_config_ from loaded_base_data
            current_config_->auto_mode = loaded_base_data.auto_mode;
            current_config_->ai_mode = loaded_base_data.ai_mode;
            current_config_->allow_concurrent_data_sources = loaded_base_data.allow_concurrent_data_sources;
            current_config_->websocket_enabled = loaded_base_data.websocket_enabled;
            current_config_->num_bands = loaded_base_data.num_bands;
            current_config_->num_antenna_ports = loaded_base_data.num_antenna_ports;
            current_config_->radio_operation_mode = loaded_base_data.radio_operation_mode;
            memcpy(current_config_->last_used_antenna, loaded_base_data.last_used_antenna, sizeof(current_config_->last_used_antenna));
            ESP_LOGD(TAG, "Base config (config_base) loaded successfully. Num_bands: %d, Num_ports: %d", current_config_->num_bands, current_config_->num_antenna_ports);
        } else {
            ESP_LOGE(TAG, "Base config (config_base) size mismatch. Expected %d, got %d. Using defaults for base config.", sizeof(base_nvs_config_data_t), base_data_size);
            base_load_err = ESP_ERR_NVS_INVALID_LENGTH; 
        }
    } else if (base_load_err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Base config (config_base) not found in NVS. Will attempt to load old 'config' blob or use defaults.");
        // Attempt to load the old "config" blob as a fallback for migration
        size_t old_config_size = sizeof(antenna_switch_config_t) - sizeof(current_config_->bands); // Old way of calculating
        esp_err_t old_config_load_err = nvs_get_blob(nvs_handle, "config", current_config_, &old_config_size);
        if (old_config_load_err == ESP_OK) {
            ESP_LOGD(TAG, "Successfully loaded old 'config' blob for migration. Please re-save configuration to migrate fully.");
            // The fields loaded from old "config" will be used.
            // num_bands, num_antenna_ports, radio_operation_mode, last_used_antenna, auto_mode, allow_concurrent_data_sources
            // are now populated from the old blob.
            // This is a one-time migration path. Subsequent saves will use "config_base".
            base_load_err = ESP_OK; // Mark as OK for the purpose of overall loading status
        } else {
            ESP_LOGW(TAG, "Old 'config' blob also not found or error: %s. Defaults will be used for base config items.", esp_err_to_name(old_config_load_err));
            // Defaults for these items are set in init() if this function returns ESP_ERR_NVS_NOT_FOUND overall
        }
    } else {
        ESP_LOGE(TAG, "Error loading base config (config_base): %s", esp_err_to_name(base_load_err));
    }
    // If base_load_err is not ESP_OK after attempting to load "config_base" and "config",
    // init() will handle setting defaults for these.
    // We assign ret to base_load_err to signal init() about the status of loading base config.
    ret = base_load_err; // This will be ESP_OK if either "config_base" or "config" (old) loaded successfully.

    // Define these outside the loop to ensure they are not on the stack repeatedly.
    static const char* default_band_names_load[10] = {"160m", "80m", "40m", "30m", "20m", "17m", "15m", "12m", "10m", "6m"};
    static const uint32_t default_start_load[10] = {1800000, 3500000, 7000000, 10100000, 14000000, 18068000, 21000000, 24890000, 28000000, 50000000};
    static const uint32_t default_end_load[10] = {2000000, 4000000, 7300000, 10150000, 14350000, 18168000, 21450000, 24990000, 29700000, 54000000};

    // Load per-radio band-tables as blobs
    for (int r = 0; r < 2; ++r) {
        // Ensure num_bands is valid before using it in loop (loaded from the main "config" blob)
        uint8_t num_bands_to_load = current_config_->num_bands;
        if (num_bands_to_load > MAX_BANDS) {
            ESP_LOGW(TAG, "Loaded num_bands (%d) exceeds MAX_BANDS (%d) for radio %d. Clamping to MAX_BANDS.", num_bands_to_load, MAX_BANDS, r);
            num_bands_to_load = MAX_BANDS;
        }

        for (int i = 0; i < num_bands_to_load; ++i) {
            char key[24];
            snprintf(key, sizeof(key), "band_cfg_r%d_b%d", r, i);
            size_t band_data_size = sizeof(band_config_t);
            esp_err_t band_load_err = nvs_get_blob(nvs_handle, key, &current_config_->bands[r][i], &band_data_size);

            bool set_band_to_default = false;
            if (band_load_err == ESP_OK) {
                if (band_data_size != sizeof(band_config_t)) {
                    ESP_LOGW(TAG, "Band config blob %s size mismatch (expected %zu, got %zu). Using defaults for this band.",
                             key, sizeof(band_config_t), band_data_size);
                    set_band_to_default = true;
                }
            } else if (band_load_err == ESP_ERR_NVS_NOT_FOUND) {
                ESP_LOGW(TAG, "Band config blob %s not found. Initializing to default.", key);
                set_band_to_default = true;
            } else {
                ESP_LOGE(TAG, "Error loading band config blob %s: %s. Using defaults.", key, esp_err_to_name(band_load_err));
                set_band_to_default = true;
            }

            if (set_band_to_default) {
                // Initialize this specific band to defaults using the static const arrays
                if (i < 10) { // Check if index is within default arrays
                    strncpy(current_config_->bands[r][i].description, default_band_names_load[i], sizeof(current_config_->bands[r][i].description)-1);
                    current_config_->bands[r][i].description[sizeof(current_config_->bands[r][i].description)-1] = '\0';
                    current_config_->bands[r][i].start_freq = default_start_load[i];
                    current_config_->bands[r][i].end_freq = default_end_load[i];
                    for (int j = 0; j < MAX_ANTENNA_PORTS; ++j) {
                        current_config_->bands[r][i].antenna_ports[j] = (j == 0);
                    }
                } else { // For bands beyond the 10 defaults, set some generic default
                    snprintf(current_config_->bands[r][i].description, sizeof(current_config_->bands[r][i].description), "Band %d", i + 1);
                    current_config_->bands[r][i].start_freq = 0;
                    current_config_->bands[r][i].end_freq = 0;
                    for (int j = 0; j < MAX_ANTENNA_PORTS; ++j) {
                        current_config_->bands[r][i].antenna_ports[j] = false;
                    }
                }
            }
        }
    }

    // radio_operation_mode is now part of "config_base"
    // Load interlock_auto_resolves_conflict, auto_restore_on_conflict_resolution, radio_restore_delay_ms
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

    // Load radio_restore_delay_ms
    uint16_t restore_delay_val;
    esp_err_t err_rd = nvs_get_u16(nvs_handle, "restore_delay", &restore_delay_val);
    if (err_rd == ESP_OK) {
        current_config_->radio_restore_delay_ms = restore_delay_val;
    } else if (err_rd == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "radio_restore_delay_ms (key restore_delay) not found in NVS, using default (200).");
        current_config_->radio_restore_delay_ms = 200;
    } else {
        ESP_LOGE(TAG, "Error loading radio_restore_delay_ms (key restore_delay): %s", esp_err_to_name(err_rd));
        current_config_->radio_restore_delay_ms = 200; // Fallback on error
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

    // Load relay names from a single blob
    size_t relay_names_blob_size = sizeof(current_config_->relay_names);
    esp_err_t names_load_err = nvs_get_blob(nvs_handle, "relay_names_all", current_config_->relay_names, &relay_names_blob_size);
    
    bool set_relay_names_to_default = false;
    if (names_load_err == ESP_OK) {
        if (relay_names_blob_size != sizeof(current_config_->relay_names)) {
            ESP_LOGW(TAG, "Relay names blob 'relay_names_all' size mismatch (expected %zu, got %zu). Using defaults.",
                     sizeof(current_config_->relay_names), relay_names_blob_size);
            set_relay_names_to_default = true;
        }
    } else if (names_load_err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Relay names blob 'relay_names_all' not found. Initializing to defaults.");
        set_relay_names_to_default = true;
    } else {
        ESP_LOGE(TAG, "Error loading relay names blob 'relay_names_all': %s. Using defaults.", esp_err_to_name(names_load_err));
        set_relay_names_to_default = true;
    }

    if (set_relay_names_to_default) {
        for (int i = 0; i < 16; i++) {
            snprintf(current_config_->relay_names[i], sizeof(current_config_->relay_names[i]), "Relay %d", i + 1);
        }
    }
    // Ensure null termination for all loaded/defaulted names just in case
    for (int i = 0; i < 16; i++) {
        current_config_->relay_names[i][sizeof(current_config_->relay_names[i])-1] = '\0';
    }


    nvs_close(nvs_handle);
    return ESP_OK;
}

void ConfigManager::add_observer(const std::function<void(const antenna_switch_config_t &)> &observer) {
    observers_.push_back(observer);
    // Immediately notify the new observer of current config
    observer(*current_config_);
}

esp_err_t ConfigManager::export_config_to_json(char **json_string) const {
    if (!json_string) {
        ESP_LOGE(TAG, "Invalid json_string parameter");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        ESP_LOGE(TAG, "Failed to create JSON root object");
        return ESP_ERR_NO_MEM;
    }

    // Add metadata
    cJSON_AddStringToObject(root, "version", "1.0");
    
    // Add timestamp (microseconds since boot converted to seconds)
    char timestamp[64];
    uint64_t time_us = esp_timer_get_time();
    snprintf(timestamp, sizeof(timestamp), "%.3f", time_us / 1000000.0);
    cJSON_AddStringToObject(root, "timestamp", timestamp);

    // Create config object
    cJSON *config = cJSON_CreateObject();
    if (!config) {
        ESP_LOGE(TAG, "Failed to create config object");
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    // Basic configuration
    cJSON_AddBoolToObject(config, "auto_mode", current_config_->auto_mode);
    cJSON_AddBoolToObject(config, "ai_mode", current_config_->ai_mode);
    cJSON_AddBoolToObject(config, "allow_concurrent_data_sources", current_config_->allow_concurrent_data_sources);
    cJSON_AddNumberToObject(config, "num_bands", current_config_->num_bands);
    cJSON_AddNumberToObject(config, "num_antenna_ports", current_config_->num_antenna_ports);
    
    // Radio operation mode
    const char *radio_mode_str;
    switch (current_config_->radio_operation_mode) {
        case RADIO_OP_MODE_SINGLE_A: radio_mode_str = "SINGLE_A"; break;
        case RADIO_OP_MODE_ALTERNATING_AB: radio_mode_str = "ALTERNATING_AB"; break;
        case RADIO_OP_MODE_CONCURRENT_AB: radio_mode_str = "CONCURRENT_AB"; break;
        default: radio_mode_str = "SINGLE_A"; break;
    }
    cJSON_AddStringToObject(config, "radio_operation_mode", radio_mode_str);

    // Bands configuration
    cJSON *bands = cJSON_CreateObject();
    cJSON *radio_a_bands = cJSON_CreateArray();
    cJSON *radio_b_bands = cJSON_CreateArray();
    
    for (int i = 0; i < current_config_->num_bands; i++) {
        // Radio A band
        cJSON *band_a = cJSON_CreateObject();
        cJSON_AddStringToObject(band_a, "description", current_config_->bands[0][i].description);
        cJSON_AddNumberToObject(band_a, "start_freq", current_config_->bands[0][i].start_freq);
        cJSON_AddNumberToObject(band_a, "end_freq", current_config_->bands[0][i].end_freq);
        
        cJSON *antenna_ports_a = cJSON_CreateArray();
        for (int j = 0; j < MAX_ANTENNA_PORTS; j++) {
            cJSON_AddItemToArray(antenna_ports_a, cJSON_CreateBool(current_config_->bands[0][i].antenna_ports[j]));
        }
        cJSON_AddItemToObject(band_a, "antenna_ports", antenna_ports_a);
        cJSON_AddItemToArray(radio_a_bands, band_a);

        // Radio B band
        cJSON *band_b = cJSON_CreateObject();
        cJSON_AddStringToObject(band_b, "description", current_config_->bands[1][i].description);
        cJSON_AddNumberToObject(band_b, "start_freq", current_config_->bands[1][i].start_freq);
        cJSON_AddNumberToObject(band_b, "end_freq", current_config_->bands[1][i].end_freq);
        
        cJSON *antenna_ports_b = cJSON_CreateArray();
        for (int j = 0; j < MAX_ANTENNA_PORTS; j++) {
            cJSON_AddItemToArray(antenna_ports_b, cJSON_CreateBool(current_config_->bands[1][i].antenna_ports[j]));
        }
        cJSON_AddItemToObject(band_b, "antenna_ports", antenna_ports_b);
        cJSON_AddItemToArray(radio_b_bands, band_b);
    }
    
    cJSON_AddItemToObject(bands, "radio_a", radio_a_bands);
    cJSON_AddItemToObject(bands, "radio_b", radio_b_bands);
    cJSON_AddItemToObject(config, "bands", bands);

    // UART configuration
    cJSON *uart = cJSON_CreateObject();
    cJSON_AddNumberToObject(uart, "baud_rate", current_config_->uart_baud_rate);
    cJSON_AddNumberToObject(uart, "parity", current_config_->uart_parity);
    cJSON_AddNumberToObject(uart, "stop_bits", current_config_->uart_stop_bits);
    cJSON_AddNumberToObject(uart, "flow_ctrl", current_config_->uart_flow_ctrl);
    cJSON_AddNumberToObject(uart, "tx_pin", current_config_->uart_tx_pin);
    cJSON_AddNumberToObject(uart, "rx_pin", current_config_->uart_rx_pin);
    cJSON_AddItemToObject(config, "uart", uart);

    // PTT configuration
    cJSON *ptt = cJSON_CreateObject();
    cJSON_AddNumberToObject(ptt, "input_radio_a", current_config_->ptt_input_radio_a);
    cJSON_AddBoolToObject(ptt, "input_radio_a_active_high", current_config_->ptt_input_radio_a_active_high);
    cJSON_AddNumberToObject(ptt, "input_radio_b", current_config_->ptt_input_radio_b);
    cJSON_AddBoolToObject(ptt, "input_radio_b_active_high", current_config_->ptt_input_radio_b_active_high);
    cJSON_AddItemToObject(config, "ptt", ptt);

    // MQTT configuration
    cJSON *mqtt = cJSON_CreateObject();
    cJSON_AddBoolToObject(mqtt, "enabled", current_config_->mqtt_enabled);
    cJSON_AddStringToObject(mqtt, "broker", current_config_->mqtt_broker);
    cJSON_AddNumberToObject(mqtt, "port", current_config_->mqtt_port);
    cJSON_AddStringToObject(mqtt, "rig_id", current_config_->mqtt_rig_id);
    cJSON_AddStringToObject(mqtt, "username", current_config_->mqtt_username);
    cJSON_AddStringToObject(mqtt, "password", current_config_->mqtt_password);
    cJSON_AddStringToObject(mqtt, "client_id", current_config_->mqtt_client_id);
    cJSON_AddStringToObject(mqtt, "topic", current_config_->mqtt_topic);
    cJSON_AddItemToObject(config, "mqtt", mqtt);

    // WebSocket configuration
    cJSON *websocket = cJSON_CreateObject();
    cJSON_AddBoolToObject(websocket, "enabled", current_config_->websocket_enabled);
    cJSON_AddItemToObject(config, "websocket", websocket);

    // Interlock configuration
    cJSON *interlock = cJSON_CreateObject();
    cJSON_AddBoolToObject(interlock, "auto_resolves_conflict", current_config_->interlock_auto_resolves_conflict);
    cJSON_AddBoolToObject(interlock, "auto_restore_on_conflict_resolution", current_config_->auto_restore_on_conflict_resolution);
    cJSON_AddNumberToObject(interlock, "radio_restore_delay_ms", current_config_->radio_restore_delay_ms);
    cJSON_AddItemToObject(config, "interlock", interlock);

    // Relay names
    cJSON *relay_names = cJSON_CreateArray();
    for (int i = 0; i < 16; i++) {
        cJSON_AddItemToArray(relay_names, cJSON_CreateString(current_config_->relay_names[i]));
    }
    cJSON_AddItemToObject(config, "relay_names", relay_names);

    // Last used antenna
    cJSON *last_used_antenna = cJSON_CreateObject();
    cJSON *radio_a_last = cJSON_CreateArray();
    cJSON *radio_b_last = cJSON_CreateArray();
    
    for (int i = 0; i < MAX_BANDS; i++) {
        cJSON_AddItemToArray(radio_a_last, cJSON_CreateNumber(current_config_->last_used_antenna[0][i]));
        cJSON_AddItemToArray(radio_b_last, cJSON_CreateNumber(current_config_->last_used_antenna[1][i]));
    }
    cJSON_AddItemToObject(last_used_antenna, "radio_a", radio_a_last);
    cJSON_AddItemToObject(last_used_antenna, "radio_b", radio_b_last);
    cJSON_AddItemToObject(config, "last_used_antenna", last_used_antenna);

    // Add config to root
    cJSON_AddItemToObject(root, "config", config);

    // Generate JSON string
    *json_string = cJSON_Print(root);
    cJSON_Delete(root);

    if (!*json_string) {
        ESP_LOGE(TAG, "Failed to generate JSON string");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGD(TAG, "Configuration exported to JSON successfully");
    return ESP_OK;
}

esp_err_t ConfigManager::import_config_from_json(const char *json_string, bool validate_only) {
    if (!json_string) {
        ESP_LOGE(TAG, "Invalid json_string parameter");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_Parse(json_string);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse JSON: invalid format");
        return ESP_ERR_INVALID_ARG;
    }

    // Check version compatibility
    cJSON *version = cJSON_GetObjectItem(root, "version");
    if (!cJSON_IsString(version)) {
        ESP_LOGE(TAG, "Missing or invalid version field");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    if (strcmp(version->valuestring, "1.0") != 0) {
        ESP_LOGW(TAG, "Version mismatch: expected 1.0, got %s. Proceeding with caution.", version->valuestring);
    }

    // Get config object
    cJSON *config = cJSON_GetObjectItem(root, "config");
    if (!cJSON_IsObject(config)) {
        ESP_LOGE(TAG, "Missing or invalid config object");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    // Create temporary config structure for validation
    antenna_switch_config_t temp_config = {};

    // Parse and validate basic configuration
    cJSON *auto_mode = cJSON_GetObjectItem(config, "auto_mode");
    if (cJSON_IsBool(auto_mode)) {
        temp_config.auto_mode = cJSON_IsTrue(auto_mode);
    } else {
        ESP_LOGE(TAG, "Invalid auto_mode field");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *ai_mode = cJSON_GetObjectItem(config, "ai_mode");
    if (cJSON_IsBool(ai_mode)) {
        temp_config.ai_mode = cJSON_IsTrue(ai_mode);
    } else {
        ESP_LOGW(TAG, "ai_mode field not found or invalid, defaulting to false");
        temp_config.ai_mode = false;
    }

    cJSON *allow_concurrent = cJSON_GetObjectItem(config, "allow_concurrent_data_sources");
    if (cJSON_IsBool(allow_concurrent)) {
        temp_config.allow_concurrent_data_sources = cJSON_IsTrue(allow_concurrent);
    } else {
        ESP_LOGE(TAG, "Invalid allow_concurrent_data_sources field");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *num_bands = cJSON_GetObjectItem(config, "num_bands");
    if (cJSON_IsNumber(num_bands) && num_bands->valueint > 0 && num_bands->valueint <= MAX_BANDS) {
        temp_config.num_bands = num_bands->valueint;
    } else {
        ESP_LOGE(TAG, "Invalid num_bands field: must be between 1 and %d", MAX_BANDS);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *num_antenna_ports = cJSON_GetObjectItem(config, "num_antenna_ports");
    if (cJSON_IsNumber(num_antenna_ports) && num_antenna_ports->valueint > 0 && num_antenna_ports->valueint <= MAX_ANTENNA_PORTS) {
        temp_config.num_antenna_ports = num_antenna_ports->valueint;
    } else {
        ESP_LOGE(TAG, "Invalid num_antenna_ports field: must be between 1 and %d", MAX_ANTENNA_PORTS);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    // Parse radio operation mode
    cJSON *radio_op_mode = cJSON_GetObjectItem(config, "radio_operation_mode");
    if (cJSON_IsString(radio_op_mode)) {
        if (strcmp(radio_op_mode->valuestring, "SINGLE_A") == 0) {
            temp_config.radio_operation_mode = RADIO_OP_MODE_SINGLE_A;
        } else if (strcmp(radio_op_mode->valuestring, "ALTERNATING_AB") == 0) {
            temp_config.radio_operation_mode = RADIO_OP_MODE_ALTERNATING_AB;
        } else if (strcmp(radio_op_mode->valuestring, "CONCURRENT_AB") == 0) {
            temp_config.radio_operation_mode = RADIO_OP_MODE_CONCURRENT_AB;
        } else {
            ESP_LOGE(TAG, "Invalid radio_operation_mode: %s", radio_op_mode->valuestring);
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
    } else {
        ESP_LOGE(TAG, "Invalid radio_operation_mode field");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    // Parse bands configuration
    cJSON *bands = cJSON_GetObjectItem(config, "bands");
    if (cJSON_IsObject(bands)) {
        cJSON *radio_a_bands = cJSON_GetObjectItem(bands, "radio_a");
        cJSON *radio_b_bands = cJSON_GetObjectItem(bands, "radio_b");
        
        if (!cJSON_IsArray(radio_a_bands) || !cJSON_IsArray(radio_b_bands)) {
            ESP_LOGE(TAG, "Invalid bands configuration");
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }

        int expected_bands = temp_config.num_bands;
        if (cJSON_GetArraySize(radio_a_bands) != expected_bands || cJSON_GetArraySize(radio_b_bands) != expected_bands) {
            ESP_LOGE(TAG, "Band array size mismatch: expected %d bands", expected_bands);
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }

        // Parse each band
        for (int i = 0; i < expected_bands; i++) {
            cJSON *band_a = cJSON_GetArrayItem(radio_a_bands, i);
            cJSON *band_b = cJSON_GetArrayItem(radio_b_bands, i);

            if (!cJSON_IsObject(band_a) || !cJSON_IsObject(band_b)) {
                ESP_LOGE(TAG, "Invalid band object at index %d", i);
                cJSON_Delete(root);
                return ESP_ERR_INVALID_ARG;
            }

            // Parse Radio A band
            cJSON *desc_a = cJSON_GetObjectItem(band_a, "description");
            cJSON *start_freq_a = cJSON_GetObjectItem(band_a, "start_freq");
            cJSON *end_freq_a = cJSON_GetObjectItem(band_a, "end_freq");
            cJSON *antenna_ports_a = cJSON_GetObjectItem(band_a, "antenna_ports");

            if (!cJSON_IsString(desc_a) || !cJSON_IsNumber(start_freq_a) || !cJSON_IsNumber(end_freq_a) || !cJSON_IsArray(antenna_ports_a)) {
                ESP_LOGE(TAG, "Invalid Radio A band %d configuration", i);
                cJSON_Delete(root);
                return ESP_ERR_INVALID_ARG;
            }

            strncpy(temp_config.bands[0][i].description, desc_a->valuestring, sizeof(temp_config.bands[0][i].description) - 1);
            temp_config.bands[0][i].description[sizeof(temp_config.bands[0][i].description) - 1] = '\0';
            temp_config.bands[0][i].start_freq = start_freq_a->valueint;
            temp_config.bands[0][i].end_freq = end_freq_a->valueint;

            if (cJSON_GetArraySize(antenna_ports_a) != MAX_ANTENNA_PORTS) {
                ESP_LOGE(TAG, "Invalid antenna_ports array size for Radio A band %d", i);
                cJSON_Delete(root);
                return ESP_ERR_INVALID_ARG;
            }

            for (int j = 0; j < MAX_ANTENNA_PORTS; j++) {
                cJSON *port = cJSON_GetArrayItem(antenna_ports_a, j);
                if (!cJSON_IsBool(port)) {
                    ESP_LOGE(TAG, "Invalid antenna port %d for Radio A band %d", j, i);
                    cJSON_Delete(root);
                    return ESP_ERR_INVALID_ARG;
                }
                temp_config.bands[0][i].antenna_ports[j] = cJSON_IsTrue(port);
            }

            // Parse Radio B band (similar logic)
            cJSON *desc_b = cJSON_GetObjectItem(band_b, "description");
            cJSON *start_freq_b = cJSON_GetObjectItem(band_b, "start_freq");
            cJSON *end_freq_b = cJSON_GetObjectItem(band_b, "end_freq");
            cJSON *antenna_ports_b = cJSON_GetObjectItem(band_b, "antenna_ports");

            if (!cJSON_IsString(desc_b) || !cJSON_IsNumber(start_freq_b) || !cJSON_IsNumber(end_freq_b) || !cJSON_IsArray(antenna_ports_b)) {
                ESP_LOGE(TAG, "Invalid Radio B band %d configuration", i);
                cJSON_Delete(root);
                return ESP_ERR_INVALID_ARG;
            }

            strncpy(temp_config.bands[1][i].description, desc_b->valuestring, sizeof(temp_config.bands[1][i].description) - 1);
            temp_config.bands[1][i].description[sizeof(temp_config.bands[1][i].description) - 1] = '\0';
            temp_config.bands[1][i].start_freq = start_freq_b->valueint;
            temp_config.bands[1][i].end_freq = end_freq_b->valueint;

            for (int j = 0; j < MAX_ANTENNA_PORTS; j++) {
                cJSON *port = cJSON_GetArrayItem(antenna_ports_b, j);
                if (!cJSON_IsBool(port)) {
                    ESP_LOGE(TAG, "Invalid antenna port %d for Radio B band %d", j, i);
                    cJSON_Delete(root);
                    return ESP_ERR_INVALID_ARG;
                }
                temp_config.bands[1][i].antenna_ports[j] = cJSON_IsTrue(port);
            }
        }
    } else {
        ESP_LOGE(TAG, "Missing or invalid bands configuration");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    // Parse UART configuration
    cJSON *uart = cJSON_GetObjectItem(config, "uart");
    if (cJSON_IsObject(uart)) {
        cJSON *baud_rate = cJSON_GetObjectItem(uart, "baud_rate");
        cJSON *parity = cJSON_GetObjectItem(uart, "parity");
        cJSON *stop_bits = cJSON_GetObjectItem(uart, "stop_bits");
        cJSON *flow_ctrl = cJSON_GetObjectItem(uart, "flow_ctrl");
        cJSON *tx_pin = cJSON_GetObjectItem(uart, "tx_pin");
        cJSON *rx_pin = cJSON_GetObjectItem(uart, "rx_pin");

        if (!cJSON_IsNumber(baud_rate) || !cJSON_IsNumber(parity) || !cJSON_IsNumber(stop_bits) ||
            !cJSON_IsNumber(flow_ctrl) || !cJSON_IsNumber(tx_pin) || !cJSON_IsNumber(rx_pin)) {
            ESP_LOGE(TAG, "Invalid UART configuration");
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }

        temp_config.uart_baud_rate = baud_rate->valueint;
        temp_config.uart_parity = parity->valueint;
        temp_config.uart_stop_bits = stop_bits->valueint;
        temp_config.uart_flow_ctrl = flow_ctrl->valueint;
        temp_config.uart_tx_pin = tx_pin->valueint;
        temp_config.uart_rx_pin = rx_pin->valueint;
    }

    // Parse PTT configuration
    cJSON *ptt = cJSON_GetObjectItem(config, "ptt");
    if (cJSON_IsObject(ptt)) {
        cJSON *input_radio_a = cJSON_GetObjectItem(ptt, "input_radio_a");
        cJSON *input_radio_a_active_high = cJSON_GetObjectItem(ptt, "input_radio_a_active_high");
        cJSON *input_radio_b = cJSON_GetObjectItem(ptt, "input_radio_b");
        cJSON *input_radio_b_active_high = cJSON_GetObjectItem(ptt, "input_radio_b_active_high");

        if (cJSON_IsNumber(input_radio_a)) temp_config.ptt_input_radio_a = input_radio_a->valueint;
        if (cJSON_IsBool(input_radio_a_active_high)) temp_config.ptt_input_radio_a_active_high = cJSON_IsTrue(input_radio_a_active_high);
        if (cJSON_IsNumber(input_radio_b)) temp_config.ptt_input_radio_b = input_radio_b->valueint;
        if (cJSON_IsBool(input_radio_b_active_high)) temp_config.ptt_input_radio_b_active_high = cJSON_IsTrue(input_radio_b_active_high);
    }

    // Parse MQTT configuration
    cJSON *mqtt = cJSON_GetObjectItem(config, "mqtt");
    if (cJSON_IsObject(mqtt)) {
        cJSON *enabled = cJSON_GetObjectItem(mqtt, "enabled");
        cJSON *broker = cJSON_GetObjectItem(mqtt, "broker");
        cJSON *port = cJSON_GetObjectItem(mqtt, "port");
        cJSON *rig_id = cJSON_GetObjectItem(mqtt, "rig_id");
        cJSON *username = cJSON_GetObjectItem(mqtt, "username");
        cJSON *password = cJSON_GetObjectItem(mqtt, "password");
        cJSON *client_id = cJSON_GetObjectItem(mqtt, "client_id");
        cJSON *topic = cJSON_GetObjectItem(mqtt, "topic");

        if (cJSON_IsBool(enabled)) temp_config.mqtt_enabled = cJSON_IsTrue(enabled);
        if (cJSON_IsString(broker)) {
            strncpy(temp_config.mqtt_broker, broker->valuestring, sizeof(temp_config.mqtt_broker) - 1);
            temp_config.mqtt_broker[sizeof(temp_config.mqtt_broker) - 1] = '\0';
        }
        if (cJSON_IsNumber(port)) temp_config.mqtt_port = port->valueint;
        if (cJSON_IsString(rig_id)) {
            strncpy(temp_config.mqtt_rig_id, rig_id->valuestring, sizeof(temp_config.mqtt_rig_id) - 1);
            temp_config.mqtt_rig_id[sizeof(temp_config.mqtt_rig_id) - 1] = '\0';
        }
        if (cJSON_IsString(username)) {
            strncpy(temp_config.mqtt_username, username->valuestring, sizeof(temp_config.mqtt_username) - 1);
            temp_config.mqtt_username[sizeof(temp_config.mqtt_username) - 1] = '\0';
        }
        if (cJSON_IsString(password)) {
            strncpy(temp_config.mqtt_password, password->valuestring, sizeof(temp_config.mqtt_password) - 1);
            temp_config.mqtt_password[sizeof(temp_config.mqtt_password) - 1] = '\0';
        }
        if (cJSON_IsString(client_id)) {
            strncpy(temp_config.mqtt_client_id, client_id->valuestring, sizeof(temp_config.mqtt_client_id) - 1);
            temp_config.mqtt_client_id[sizeof(temp_config.mqtt_client_id) - 1] = '\0';
        }
        if (cJSON_IsString(topic)) {
            strncpy(temp_config.mqtt_topic, topic->valuestring, sizeof(temp_config.mqtt_topic) - 1);
            temp_config.mqtt_topic[sizeof(temp_config.mqtt_topic) - 1] = '\0';
        }
    }

    // Parse WebSocket configuration
    cJSON *websocket = cJSON_GetObjectItem(config, "websocket");
    if (cJSON_IsObject(websocket)) {
        cJSON *enabled = cJSON_GetObjectItem(websocket, "enabled");
        if (cJSON_IsBool(enabled)) temp_config.websocket_enabled = cJSON_IsTrue(enabled);
    }

    // Parse interlock configuration
    cJSON *interlock = cJSON_GetObjectItem(config, "interlock");
    if (cJSON_IsObject(interlock)) {
        cJSON *auto_resolves = cJSON_GetObjectItem(interlock, "auto_resolves_conflict");
        cJSON *auto_restore = cJSON_GetObjectItem(interlock, "auto_restore_on_conflict_resolution");
        cJSON *restore_delay = cJSON_GetObjectItem(interlock, "radio_restore_delay_ms");

        if (cJSON_IsBool(auto_resolves)) temp_config.interlock_auto_resolves_conflict = cJSON_IsTrue(auto_resolves);
        if (cJSON_IsBool(auto_restore)) temp_config.auto_restore_on_conflict_resolution = cJSON_IsTrue(auto_restore);
        if (cJSON_IsNumber(restore_delay)) temp_config.radio_restore_delay_ms = restore_delay->valueint;
    }

    // Parse relay names
    cJSON *relay_names = cJSON_GetObjectItem(config, "relay_names");
    if (cJSON_IsArray(relay_names) && cJSON_GetArraySize(relay_names) == 16) {
        for (int i = 0; i < 16; i++) {
            cJSON *name = cJSON_GetArrayItem(relay_names, i);
            if (cJSON_IsString(name)) {
                strncpy(temp_config.relay_names[i], name->valuestring, sizeof(temp_config.relay_names[i]) - 1);
                temp_config.relay_names[i][sizeof(temp_config.relay_names[i]) - 1] = '\0';
            }
        }
    }

    // Parse last used antenna
    cJSON *last_used_antenna = cJSON_GetObjectItem(config, "last_used_antenna");
    if (cJSON_IsObject(last_used_antenna)) {
        cJSON *radio_a_last = cJSON_GetObjectItem(last_used_antenna, "radio_a");
        cJSON *radio_b_last = cJSON_GetObjectItem(last_used_antenna, "radio_b");

        if (cJSON_IsArray(radio_a_last) && cJSON_GetArraySize(radio_a_last) == MAX_BANDS) {
            for (int i = 0; i < MAX_BANDS; i++) {
                cJSON *antenna = cJSON_GetArrayItem(radio_a_last, i);
                if (cJSON_IsNumber(antenna)) {
                    temp_config.last_used_antenna[0][i] = antenna->valueint;
                }
            }
        }

        if (cJSON_IsArray(radio_b_last) && cJSON_GetArraySize(radio_b_last) == MAX_BANDS) {
            for (int i = 0; i < MAX_BANDS; i++) {
                cJSON *antenna = cJSON_GetArrayItem(radio_b_last, i);
                if (cJSON_IsNumber(antenna)) {
                    temp_config.last_used_antenna[1][i] = antenna->valueint;
                }
            }
        }
    }

    cJSON_Delete(root);

    if (validate_only) {
        ESP_LOGI(TAG, "Configuration validation successful");
        return ESP_OK;
    }

    // Apply the configuration
    esp_err_t ret = update_config(temp_config);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Configuration imported successfully");
    } else {
        ESP_LOGE(TAG, "Failed to apply imported configuration: %s", esp_err_to_name(ret));
    }

    return ret;
}
