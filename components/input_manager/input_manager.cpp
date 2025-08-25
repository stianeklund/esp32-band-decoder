#include "input_manager.h"
#include "antenna_switch.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "kc868_a16_hw.h"

static auto TAG = "InputManager";

// Helper function to determine the logical state of a PTT input
// and report the physical state of the PCF8574 pin for logging.
static bool determine_ptt_logical_state(
    const int configured_pin_index,             // The configured input pin (0-15)
    const bool configured_terminal_active_high, // True if the PTT signal is active high at the terminal
    const uint16_t all_inputs_mask,             // The raw mask from kc868_a16_get_all_inputs()
    bool* out_pcf8574_pin_physically_low        // Output: true if the PCF8574 pin for this input is physically low
) {
    // For kc868_a16_get_inputs_0_7_raw, all_inputs_mask contains raw PCF8574 data for pins 0-7:
    // - A bit value of '0' means the corresponding PCF8574 physical pin is LOW.
    // - A bit value of '1' means the corresponding PCF8574 physical pin is HIGH.
    
    // Check if the specific bit for configured_pin_index in all_inputs_mask is '0' (LOW)
    *out_pcf8574_pin_physically_low = ((all_inputs_mask >> configured_pin_index) & 0x01) == 0;
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

InputManager::InputManager() : 
    initialized_(false), 
    ptt_poll_task_handle_(nullptr), 
    ptt_a_last_hw_state_(false), 
    ptt_b_last_hw_state_(false),
    ptt_input_radio_a_config_(-1),
    ptt_input_radio_a_active_high_config_(false),
    ptt_input_radio_b_config_(-1),
    ptt_input_radio_b_active_high_config_(false) {
}

esp_err_t InputManager::init() {
    if (initialized_) {
        ESP_LOGI(TAG, "Already initialized.");
        return ESP_OK;
    }

    // esp_log_level_set(TAG, ESP_LOG_DEBUG);
    ESP_LOGI(TAG, "Initializing InputManager...");

    // Initialize the underlying KC868-A16 hardware (I2C, etc.)
    // This is now safe to call even if RelayController also calls it.
    ESP_RETURN_ON_ERROR(kc868_a16_hw_init(), TAG, "Failed to initialize KC868-A16 hardware");

    // Note: PCF8574 inputs typically don't need specific configuration beyond the I2C bus being up.
    // The pins on PCF8574 are quasi-bidirectional and default to high-impedance inputs when read.

    // Create and start the PTT polling task
    const BaseType_t task_created = xTaskCreate(
        ptt_poll_task_trampoline,
        "ptt_poll_task",
        4096,
        this,
        configMAX_PRIORITIES - 3, // Increased priority
        &ptt_poll_task_handle_
    );


    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create PTT polling task");
        // ptt_poll_task_handle_ will be nullptr, task won't run.
        return ESP_FAIL; 
    }
    ESP_LOGI(TAG, "PTT polling task created and started.");

    initialized_ = true;
    ESP_LOGI(TAG, "InputManager initialized successfully.");
    return ESP_OK;
}

esp_err_t InputManager::get_input_state(const uint8_t input_num, bool* state) {
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

void InputManager::refresh_active_ptt_config() {
    // This call is efficient. It only re-reads from ConfigManager (and potentially NVS)
    // if the cache is invalid or the config version has changed.
    const auto& config = get_cached_config(); 

    ptt_input_radio_a_config_ = config.ptt_input_radio_a;
    ptt_input_radio_a_active_high_config_ = config.ptt_input_radio_a_active_high;
    ptt_input_radio_b_config_ = config.ptt_input_radio_b;
    ptt_input_radio_b_active_high_config_ = config.ptt_input_radio_b_active_high;
}

// Trampoline function for the FreeRTOS task
void InputManager::ptt_poll_task_trampoline(void* arg) {
    auto* manager = static_cast<InputManager*>(arg);
    manager->ptt_poll_task();
}

constexpr int kMaxMonitoredPin = 7;

void InputManager::handle_ptt_line(
    const int config_pin,
    const bool active_high,
    bool& last_state,
    const char* label,
    const uint16_t inputs_mask,
    const std::function<void(bool)>& on_state_change) {

    if (config_pin < 0)
        return;

    if (config_pin <= kMaxMonitoredPin) {

        bool pin_physically_low;
        const bool logically_active = determine_ptt_logical_state(config_pin, active_high, inputs_mask,
                                                            &pin_physically_low);

        if (logically_active != last_state) {
            ESP_LOGI(TAG, "Transmit state changed via PTT input line to: %s", logically_active ? "true" : "false");

            ESP_LOGV(
                TAG,
                "PTT %s (Input Pin %d) logical state CHANGED to: %s. (PCF8574 pin was %s, Configured terminal active: %s)",
                label, config_pin,
                logically_active ? "ACTIVE" : "INACTIVE",
                pin_physically_low ? "LOW" : "HIGH",
                active_high ? "HIGH" : "LOW");

            on_state_change(logically_active);
            last_state = logically_active;
        }
    }
    else {
        if (last_state) {
            ESP_LOGW(TAG, "PTT %s (Input Pin %d) is configured for an unmonitored range (>=8). Forcing INACTIVE.",
                     label, config_pin);
            on_state_change(false);
            last_state = false;
        }
    }
}

void InputManager::handle_ptt_read_error() {
    if (ptt_a_last_hw_state_) {
        AntennaSwitch::instance().on_hw_ptt_a_state_change(false);
        ptt_a_last_hw_state_ = false;
        ESP_LOGW(TAG, "PTT A forced INACTIVE due to read error.");
    }
    if (ptt_b_last_hw_state_) {
        AntennaSwitch::instance().on_hw_ptt_b_state_change(false);
        ptt_b_last_hw_state_ = false;
        ESP_LOGW(TAG, "PTT B forced INACTIVE due to read error.");
    }
}

void InputManager::ptt_poll_task() {
    uint16_t current_inputs_mask = 0;

    for (;;) {
        if (!initialized_) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        refresh_active_ptt_config();

        uint8_t ptt_inputs_raw_0_7;

        if (const esp_err_t ret = kc868_a16_get_inputs_0_7_raw(&ptt_inputs_raw_0_7); ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read inputs (0-7) in PTT poll task: %s", esp_err_to_name(ret));
            handle_ptt_read_error();
            
            // Feed watchdog before error delay
            if (esp_task_wdt_status(xTaskGetCurrentTaskHandle()) == ESP_OK) {
                esp_task_wdt_reset();
            }
            
            vTaskDelay(pdMS_TO_TICKS(50)); // Moderate delay on error, but don't slow PTT too much
            continue;
        }

        current_inputs_mask = static_cast<uint16_t>(ptt_inputs_raw_0_7);

        handle_ptt_line(
            ptt_input_radio_a_config_,
            ptt_input_radio_a_active_high_config_,
            ptt_a_last_hw_state_,
            "A",
            current_inputs_mask, [](const bool state) { AntennaSwitch::instance().on_hw_ptt_a_state_change(state); }
        );

        handle_ptt_line(
            ptt_input_radio_b_config_,
            ptt_input_radio_b_active_high_config_,
            ptt_b_last_hw_state_,
            "B",
            current_inputs_mask, [](const bool state) { AntennaSwitch::instance().on_hw_ptt_b_state_change(state); }
        );

        // Feed watchdog periodically
        if (esp_task_wdt_status(xTaskGetCurrentTaskHandle()) == ESP_OK) {
            esp_task_wdt_reset();
        }
        
        vTaskDelay(pdMS_TO_TICKS(5)); // Keep 5ms for fast PTT detection
    }
}

/*// The actual PTT polling task implementation
void InputManager::ptt_poll_task() {
    uint16_t current_inputs_mask = 0; // Will store the state of pins 0-7

    for (;;) {
        if (!initialized_) {
            vTaskDelay(pdMS_TO_TICKS(1000)); // Wait if not initialized
            continue;
        }

        // Refresh local PTT config from cache (efficiently checks if update is needed)
        refresh_active_ptt_config();

        uint8_t ptt_inputs_raw_0_7;

        if (const esp_err_t ret_hw_read = kc868_a16_get_inputs_0_7_raw(&ptt_inputs_raw_0_7); ret_hw_read == ESP_OK) {
            current_inputs_mask = static_cast<uint16_t>(ptt_inputs_raw_0_7);
            // ESP_LOGV(TAG, "PTT Poll: Raw inputs 0-7: 0x%02X, current_inputs_mask: 0x%04X", ptt_inputs_raw_0_7, current_inputs_mask);

            // --- PTT A ---
            if (ptt_input_radio_a_config_ != -1 && ptt_input_radio_a_config_ < 8) { // Ensure config is for pins 0-7
                bool pcf8574_pin_a_physically_low;
                const bool ptt_a_logically_active = determine_ptt_logical_state(
                    ptt_input_radio_a_config_,
                    ptt_input_radio_a_active_high_config_,
                    current_inputs_mask, // This now correctly represents pins 0-7 for determine_ptt_logical_state
                    &pcf8574_pin_a_physically_low
                );

                if (ptt_a_logically_active != ptt_a_last_hw_state_) {
                    ESP_LOGV(TAG, "PTT A (Input Pin %d) logical state CHANGED to: %s. (PCF8574 pin was %s, Configured terminal active: %s)",
                             ptt_input_radio_a_config_,
                             ptt_a_logically_active ? "ACTIVE" : "INACTIVE",
                             pcf8574_pin_a_physically_low ? "LOW" : "HIGH",
                             ptt_input_radio_a_active_high_config_ ? "HIGH" : "LOW");

                    AntennaSwitch::instance().on_hw_ptt_a_state_change(ptt_a_logically_active);
                    ptt_a_last_hw_state_ = ptt_a_logically_active;
                }
            } else if (ptt_input_radio_a_config_ >= 8) {
                 if (ptt_a_last_hw_state_ != false) { // If it was previously active and now config is out of range
                    ESP_LOGW(TAG, "PTT A (Input Pin %d) is configured for an unmonitored range (>=8). Forcing INACTIVE.", ptt_input_radio_a_config_);
                    AntennaSwitch::instance().on_hw_ptt_a_state_change(false); // Report as inactive
                    ptt_a_last_hw_state_ = false;
                 }
            }


            // --- PTT B ---
            if (ptt_input_radio_b_config_ != -1 && ptt_input_radio_b_config_ < 8) {
                bool pcf8574_pin_b_physically_low;
                const bool ptt_b_logically_active = determine_ptt_logical_state(
                    ptt_input_radio_b_config_,
                    ptt_input_radio_b_active_high_config_,
                    current_inputs_mask,
                    &pcf8574_pin_b_physically_low
                );

                if (ptt_b_logically_active != ptt_b_last_hw_state_) {
                    ESP_LOGD(TAG, "PTT B (Input Pin %d) logical state CHANGED to: %s. (PCF8574 pin was %s, Configured terminal active: %s)",
                            ptt_input_radio_b_config_,
                            ptt_b_logically_active ? "ACTIVE" : "INACTIVE",
                            pcf8574_pin_b_physically_low ? "LOW" : "HIGH",
                            ptt_input_radio_b_active_high_config_ ? "HIGH" : "LOW");
                    
                    AntennaSwitch::instance().on_hw_ptt_b_state_change(ptt_b_logically_active);
                    ptt_b_last_hw_state_ = ptt_b_logically_active;
                }
            } else if (ptt_input_radio_b_config_ >= 8) {
                if (ptt_b_last_hw_state_ != false) { // If it was previously active and now config is out of range
                    ESP_LOGW(TAG, "PTT B (Input Pin %d) is configured for an unmonitored range (>=8). Forcing INACTIVE.", ptt_input_radio_b_config_);
                    AntennaSwitch::instance().on_hw_ptt_b_state_change(false); // Report as inactive
                    ptt_b_last_hw_state_ = false;
                }
            }
        } else {
            ESP_LOGE(TAG, "Failed to read inputs (0-7) in PTT poll task: %s", esp_err_to_name(ret_hw_read));
            // If read fails, consider PTTs as inactive to prevent them from being stuck active
            if (ptt_a_last_hw_state_) {
                AntennaSwitch::instance().on_hw_ptt_a_state_change(false);
                ptt_a_last_hw_state_ = false;
                ESP_LOGW(TAG, "PTT A forced INACTIVE due to read error.");
            }
            if (ptt_b_last_hw_state_) {
                AntennaSwitch::instance().on_hw_ptt_b_state_change(false);
                ptt_b_last_hw_state_ = false;
                ESP_LOGW(TAG, "PTT B forced INACTIVE due to read error.");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}*/
