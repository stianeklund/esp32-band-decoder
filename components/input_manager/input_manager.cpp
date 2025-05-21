#include "input_manager.h"
#include "kc868_a16_hw.h" // For accessing KC868-A16 hardware functions
#include "esp_log.h"

static const char* TAG = "InputManager";

InputManager* InputManager::instance_ = nullptr;
std::mutex InputManager::instance_mutex_;

InputManager& InputManager::instance() {
    std::lock_guard lock(instance_mutex_);
    if (instance_ == nullptr) {
        instance_ = new InputManager();
    }
    return *instance_;
}

InputManager::InputManager() : initialized_(false) {
    // Constructor can be light, main initialization in init()
}

esp_err_t InputManager::init() {
    if (initialized_) {
        ESP_LOGI(TAG, "Already initialized.");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing InputManager...");

    // Initialize the underlying KC868-A16 hardware (I2C, etc.)
    // This is now safe to call even if RelayController also calls it.
    esp_err_t ret = kc868_a16_hw_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize KC868-A16 hardware: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Note: PCF8574 inputs typically don't need specific configuration beyond the I2C bus being up.
    // The pins on PCF8574 are quasi-bidirectional and default to high-impedance inputs when read.

    initialized_ = true;
    ESP_LOGI(TAG, "InputManager initialized successfully.");
    return ESP_OK;
}

esp_err_t InputManager::get_input_state(uint8_t input_num, bool* state) {
    if (!initialized_) {
        ESP_LOGE(TAG, "InputManager not initialized.");
        return ESP_ERR_INVALID_STATE;
    }
    if (state == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    // kc868_a16_get_input_state already validates input_num range (0-15)
    return kc868_a16_get_input_state(input_num, state);
}

esp_err_t InputManager::get_all_inputs(uint16_t* state_mask) {
    if (!initialized_) {
        ESP_LOGE(TAG, "InputManager not initialized.");
        return ESP_ERR_INVALID_STATE;
    }
    if (state_mask == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    return kc868_a16_get_all_inputs(state_mask);
}
