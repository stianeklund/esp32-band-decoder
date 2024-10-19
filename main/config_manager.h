#pragma once

#include <functional>
#include <vector>
#include "esp_err.h"

#include "antenna_switch.h"

class ConfigManager {
    static ConfigManager *instance_;
    antenna_switch_config_t *current_config_;
    std::vector<std::function<void(const antenna_switch_config_t &)> > observers_;

    // Private constructor for singleton
    ConfigManager();

public:
    static ConfigManager &instance();

    // Delete copy constructor and assignment operator
    ConfigManager(const ConfigManager &) = delete;

    ConfigManager &operator=(const ConfigManager &) = delete;

    // Get current config (const to prevent unauthorized modifications)
    const antenna_switch_config_t &get_config() const { return *current_config_; }

    // Update config and notify all observers
    esp_err_t update_config(const antenna_switch_config_t &new_config);

    // Save to / load from NVS
    esp_err_t save_to_nvs() const;

    esp_err_t load_from_nvs() const;

    // Observer pattern
    void add_observer(const std::function<void(const antenna_switch_config_t &)> &observer);

    // Initialize with default config if needed
    esp_err_t init() const;

    // Destructor to clean up current_config_
    ~ConfigManager();
};
