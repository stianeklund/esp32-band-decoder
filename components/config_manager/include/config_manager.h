#pragma once

#include <functional>
#include <vector>
#include <atomic> // Required for std::atomic
#include <mutex>
#include "esp_err.h"
#include "freertos/FreeRTOS.h" // Required for FreeRTOS types
#include "freertos/task.h"     // Required for TaskHandle_t
#include "freertos/semphr.h"   // Required for SemaphoreHandle_t

#include "antenna_switch.h"

#define NVS_WRITER_TASK_STACK_SIZE 4096
#define NVS_WRITER_TASK_PRIORITY (tskIDLE_PRIORITY + 1) // Slightly above idle

class ConfigManager {
    static ConfigManager *instance_;
    antenna_switch_config_t *current_config_;
    mutable std::mutex config_mutex_;
    std::vector<std::function<void(const antenna_switch_config_t &)> > observers_;
    
    // Configuration cache version counter for invalidation
    mutable std::atomic<uint32_t> config_version_{0};

    // Asynchronous NVS saving members
    std::atomic<bool> config_dirty_{false};
    std::atomic<bool> full_save_required_{false};
    std::atomic<bool> nvs_writer_shutdown_requested_{false};
    TaskHandle_t nvs_writer_task_handle_{nullptr};
    SemaphoreHandle_t nvs_save_signal_{nullptr};

    static void nvs_writer_task_trampoline(void *arg);
    void nvs_writer_task();
    esp_err_t save_preferences_to_nvs() const;

    // Private constructor for singleton
    ConfigManager();

public:
    static ConfigManager &instance();

    // Delete copy constructor and assignment operator
    ConfigManager(const ConfigManager &) = delete;

    ConfigManager &operator=(const ConfigManager &) = delete;

    // Return snapshots so callers cannot observe a concurrent in-place update.
    antenna_switch_config_t get_config() const;
    antenna_switch_config_t get_config_ref() const;
    void get_ptt_config(int &radio_a_input, bool &radio_a_active_high,
                        int &radio_b_input, bool &radio_b_active_high) const;
    bool is_mqtt_enabled() const;
    bool is_websocket_enabled() const;
    uint16_t get_radio_restore_delay_ms() const;
    
    // Get current configuration version for cache invalidation
    uint32_t get_config_version() const { return config_version_.load(); }

    // Update config and notify all observers
    esp_err_t update_config(const antenna_switch_config_t &new_config);
    esp_err_t update_last_used_antenna(size_t radio_index, size_t band_index, uint8_t relay_id);

    // Save to / load from NVS
    esp_err_t save_to_nvs() const;

    esp_err_t load_from_nvs() const;

    // Observer pattern
    void add_observer(const std::function<void(const antenna_switch_config_t &)> &observer);

    // Initialize with default config if needed
    esp_err_t init(); // Made non-const

    // Reset current configuration to factory defaults
    esp_err_t reset_to_defaults();

    /**
     * @brief Triggers a save of pending configuration changes to NVS and waits for completion or timeout.
     * @param xTicksToWait The maximum time to wait for the save operation to complete.
     * @return ESP_OK if save completed or no dirty data, ESP_ERR_TIMEOUT if timeout occurred,
     *         ESP_FAIL if NVS writer task is not available or other error.
     */
    esp_err_t flush_pending_save(TickType_t xTicksToWait);

    // Configuration export/import functionality
    /**
     * @brief Export current configuration to JSON string
     * @param json_string Pointer to store the allocated JSON string (must be freed by caller)
     * @return ESP_OK on success, error code on failure
     */
    esp_err_t export_config_to_json(char **json_string) const;

    /**
     * @brief Import configuration from JSON string and apply it
     * @param json_string JSON string containing the configuration
     * @param validate_only If true, only validate without applying changes
     * @return ESP_OK on success, error code on failure
     */
    esp_err_t import_config_from_json(const char *json_string, bool validate_only = false);

    // Destructor to clean up current_config_
    ~ConfigManager();
};
