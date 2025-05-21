#include "input_manager.h"
#include "kc868_a16_hw.h" // For accessing KC868-A16 hardware functions
#include "esp_log.h"
#include "antenna_switch.h" // For AntennaSwitch::instance() and its callbacks
#include "config_manager.h" // For ConfigManager::instance().get_config()

static const char* TAG = "InputManager";

// Helper function to determine the logical state of a PTT input
// and report the physical state of the PCF8574 pin for logging.
static bool determine_ptt_logical_state(
    const int configured_pin_index,             // The configured input pin number (0-15)
    const bool configured_terminal_active_high, // True if the PTT signal is active high at the terminal
    const uint16_t all_inputs_mask,             // The raw mask from kc868_a16_get_all_inputs()
    bool* out_pcf8574_pin_physically_low        // Output: true if the PCF8574 pin for this input is physically low
) {
    // all_inputs_mask bit is '1' if PCF8574 physical pin is LOW (due to inversion in kc868_a16_get_all_inputs).
    // all_inputs_mask bit is '0' if PCF8574 physical pin is HIGH.
    *out_pcf8574_pin_physically_low = (all_inputs_mask & (1 << configured_pin_index)) != 0;
    const bool pcf8574_pin_physically_high = !(*out_pcf8574_pin_physically_low);

    // The KC868-A16 input circuit relationship:
    // - Terminal HIGH (+12V, e.g., radio PTT active) -> Optocoupler OFF -> PCF8574 pin HIGH.
    // - Terminal LOW (0V,   e.g., radio PTT inactive) -> Optocoupler ON  -> PCF8574 pin LOW.

    bool logical_ptt_active;
    if (configured_terminal_active_high) {
        // PTT is active when the TERMINAL is HIGH.
        // Terminal HIGH corresponds to PCF8574 pin HIGH.
        logical_ptt_active = pcf8574_pin_physically_high;
    } else {
        // PTT is active when the TERMINAL is LOW.
        // Terminal LOW corresponds to PCF8574 pin LOW.
        logical_ptt_active = *out_pcf8574_pin_physically_low;
    }
    return logical_ptt_active;
}

InputManager* InputManager::instance_ = nullptr;
std::mutex InputManager::instance_mutex_;

InputManager& InputManager::instance() {
    std::lock_guard lock(instance_mutex_);
    if (instance_ == nullptr) {
        instance_ = new InputManager();
    }
    return *instance_;
}

InputManager::InputManager() : initialized_(false), ptt_poll_task_handle_(nullptr), ptt_a_last_hw_state_(false), ptt_b_last_hw_state_(false) {
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

    // Create and start the PTT polling task
    BaseType_t task_created = xTaskCreate(
        ptt_poll_task_trampoline,
        "ptt_poll_task",
        4096,
        this,
        5,
        &ptt_poll_task_handle_
    );

    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create PTT polling task");
        // ptt_poll_task_handle_ will be nullptr, task won't run.
        return ESP_FAIL; 
    }
    ESP_LOGI(TAG, "PTT polling task created and started."); // Changed from ESP_LOGD to ESP_LOGI

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

// Trampoline function for the FreeRTOS task
void InputManager::ptt_poll_task_trampoline(void* arg) {
    auto* manager = static_cast<InputManager*>(arg);
    manager->ptt_poll_task();
}

// The actual PTT polling task implementation
void InputManager::ptt_poll_task() {
    uint16_t current_inputs_mask = 0;

    for (;;) {
        if (!initialized_) {
            vTaskDelay(pdMS_TO_TICKS(1000)); // Wait if not initialized
            continue;
        }

        const auto& config = ConfigManager::instance().get_config();

        if (const esp_err_t ret = kc868_a16_get_all_inputs(&current_inputs_mask); ret == ESP_OK) {
            ESP_LOGV(TAG, "PTT Poll: current_inputs_mask from HW: 0x%04X", current_inputs_mask); // Log overall mask if needed

            // --- PTT A ---
            if (config.ptt_input_radio_a != -1) { // Check if PTT A input is configured
                bool pcf8574_pin_a_physically_low;

                const bool ptt_a_logically_active = determine_ptt_logical_state(
                    config.ptt_input_radio_a,
                    config.ptt_input_radio_a_active_high,
                    current_inputs_mask,
                    &pcf8574_pin_a_physically_low
                );

                if (ptt_a_logically_active != ptt_a_last_hw_state_) {
                    ESP_LOGV(TAG, "PTT A (Input Pin %d) logical state CHANGED to: %s. (PCF8574 pin was %s, Configured terminal active: %s)",
                             config.ptt_input_radio_a,
                             ptt_a_logically_active ? "ACTIVE" : "INACTIVE",
                             pcf8574_pin_a_physically_low ? "LOW" : "HIGH",
                             config.ptt_input_radio_a_active_high ? "HIGH" : "LOW");

                    AntennaSwitch::instance().on_hw_ptt_a_state_change(ptt_a_logically_active);
                    ptt_a_last_hw_state_ = ptt_a_logically_active;
                }
            }

            // --- PTT B ---
            if (config.ptt_input_radio_b != -1) { // Check if PTT B input is configured
                bool pcf8574_pin_b_physically_low;

                const bool ptt_b_logically_active = determine_ptt_logical_state(
                    config.ptt_input_radio_b,
                    config.ptt_input_radio_b_active_high,
                    current_inputs_mask,
                    &pcf8574_pin_b_physically_low
                );

                if (ptt_b_logically_active != ptt_b_last_hw_state_) {
                     ESP_LOGV(TAG, "PTT B (Input Pin %d) logical state CHANGED to: %s. (PCF8574 pin was %s, Configured terminal active: %s)",
                             config.ptt_input_radio_b,
                             ptt_b_logically_active ? "ACTIVE" : "INACTIVE",
                             pcf8574_pin_b_physically_low ? "LOW" : "HIGH", 
                             config.ptt_input_radio_b_active_high ? "HIGH" : "LOW");

                    AntennaSwitch::instance().on_hw_ptt_b_state_change(ptt_b_logically_active);
                    ptt_b_last_hw_state_ = ptt_b_logically_active;
                }
            }

        } else {
            ESP_LOGE(TAG, "Failed to read inputs in PTT poll task: %s", esp_err_to_name(ret));
        }

        vTaskDelay(pdMS_TO_TICKS(50)); // Poll every 50ms
    }
}
