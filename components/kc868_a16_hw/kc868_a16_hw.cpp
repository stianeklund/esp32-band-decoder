// ReSharper disable CppRedundantParentheses
#include "include/kc868_a16_hw.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <atomic>
#include <mutex>

static constexpr const char* TAG = "KC868_A16_HW";
static uint16_t output_state = 0;
static std::mutex output_state_mutex_;
static bool kc868_a16_initialized = false;
static SemaphoreHandle_t i2c_bus_mutex_ = nullptr;

// --- I2C bus error instrumentation ---
// We overclock the PCF8574s to 400 kHz (see I2C_MASTER_FREQ_HZ note in header).
// These counters let us measure whether that is actually causing bus errors, so
// the decision to drop back to 100 kHz can be data-driven instead of a guess.
static std::atomic<uint32_t> i2c_first_attempt_errors_{0}; // failed on the first try (timeout or NACK)
static std::atomic<uint32_t> i2c_retry_successes_{0};      // recovered on the second attempt
static std::atomic<uint32_t> i2c_hard_failures_{0};        // still failed after the retry (or non-retryable)
static std::atomic<uint32_t> i2c_mutex_failures_{0};       // could not acquire the bus mutex

// Variables to store I2C addresses for input expanders
// These will be set to KC868_A16_HW_EXPECTED_INPUT_ADDR_PINS_0_7 and
// KC868_A16_HW_EXPECTED_INPUT_ADDR_PINS_8_15
static uint8_t discovered_pcf8574_input_addr_for_pins_0_7 = 0;
static uint8_t discovered_pcf8574_input_addr_for_pins_8_15 = 0;

static esp_err_t write_pcf8574(const uint8_t addr, const uint8_t data) {
    if (xSemaphoreTake(i2c_bus_mutex_, pdMS_TO_TICKS(10)) != pdTRUE) {
        i2c_mutex_failures_.fetch_add(1, std::memory_order_relaxed);
        ESP_LOGW(TAG, "Task '%s' FAILED to take i2c_bus_mutex_ for write_pcf8574 to 0x%02X (attempt 1) after 10ms", pcTaskGetName(NULL), addr);
        return ESP_ERR_TIMEOUT;
    }

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, data, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(10));
    i2c_cmd_link_delete(cmd);

    xSemaphoreGive(i2c_bus_mutex_);

    if (ret != ESP_OK) {
        i2c_first_attempt_errors_.fetch_add(1, std::memory_order_relaxed);
    }

    if (ret == ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "write_pcf8574 to 0x%02X timed out on 1st attempt. Retrying after 10ms delay...", addr);
        vTaskDelay(pdMS_TO_TICKS(10));

        if (xSemaphoreTake(i2c_bus_mutex_, pdMS_TO_TICKS(10)) != pdTRUE) {
            i2c_mutex_failures_.fetch_add(1, std::memory_order_relaxed);
            i2c_hard_failures_.fetch_add(1, std::memory_order_relaxed);
            ESP_LOGE(TAG, "Task '%s' FAILED to take i2c_bus_mutex_ for write_pcf8574 to 0x%02X (attempt 2) after 10ms", pcTaskGetName(NULL), addr);
            return ESP_ERR_TIMEOUT;
        }

        cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_write_byte(cmd, data, true);
        i2c_master_stop(cmd);
        ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(10));
        i2c_cmd_link_delete(cmd);

        xSemaphoreGive(i2c_bus_mutex_);

        if (ret == ESP_OK) {
            i2c_retry_successes_.fetch_add(1, std::memory_order_relaxed);
            ESP_LOGI(TAG, "write_pcf8574 to 0x%02X succeeded on retry.", addr);
        } else {
            i2c_hard_failures_.fetch_add(1, std::memory_order_relaxed);
            ESP_LOGE(TAG, "write_pcf8574 to 0x%02X failed on retry: %s", addr, esp_err_to_name(ret));
        }
    } else if (ret != ESP_OK) {
        i2c_hard_failures_.fetch_add(1, std::memory_order_relaxed);
        ESP_LOGE(TAG, "write_pcf8574 to 0x%02X failed on 1st attempt (not timeout): %s", addr, esp_err_to_name(ret));
    }

    return ret;
}

static esp_err_t read_pcf8574(const uint8_t addr, uint8_t *data) {
    if (data == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(i2c_bus_mutex_, pdMS_TO_TICKS(10)) != pdTRUE) {
        i2c_mutex_failures_.fetch_add(1, std::memory_order_relaxed);
        ESP_LOGE(TAG, "Task '%s' FAILED to take i2c_bus_mutex_ for read_pcf8574 from 0x%02X (attempt 1) after 10ms", pcTaskGetName(NULL), addr);
        return ESP_ERR_TIMEOUT;
    }

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, data, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(10));
    i2c_cmd_link_delete(cmd);

    xSemaphoreGive(i2c_bus_mutex_); // Release mutex IMMEDIATELY after the I2C operation

    if (ret != ESP_OK) {
        i2c_first_attempt_errors_.fetch_add(1, std::memory_order_relaxed);
    }

    // --- Handle Attempt 1 Result ---
    if (ret == ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "read_pcf8574 from 0x%02X timed out on 1st attempt. Retrying after 10ms delay...", addr);
        vTaskDelay(pdMS_TO_TICKS(10)); // Delay is OUTSIDE the mutex protection

        // --- Attempt 2 ---
        if (xSemaphoreTake(i2c_bus_mutex_, pdMS_TO_TICKS(10)) != pdTRUE) {
            i2c_mutex_failures_.fetch_add(1, std::memory_order_relaxed);
            i2c_hard_failures_.fetch_add(1, std::memory_order_relaxed);
            ESP_LOGE(TAG, "Task '%s' FAILED to take i2c_bus_mutex_ for read_pcf8574 from 0x%02X (attempt 2) after 10ms", pcTaskGetName(NULL), addr);
            // Return the original error or a new one indicating mutex failure on retry
            return ESP_ERR_TIMEOUT;
        }

        cmd = i2c_cmd_link_create(); // Recreate command
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_READ, true);
        i2c_master_read_byte(cmd, data, I2C_MASTER_NACK);
        i2c_master_stop(cmd);
        ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(10)); // Original timeout
        i2c_cmd_link_delete(cmd);

        xSemaphoreGive(i2c_bus_mutex_); // Release mutex IMMEDIATELY after the I2C operation

        // --- Handle Attempt 2 Result ---
        if (ret == ESP_OK) {
            i2c_retry_successes_.fetch_add(1, std::memory_order_relaxed);
            ESP_LOGD(TAG, "read_pcf8574 from 0x%02X succeeded on retry.", addr);
        } else {
            i2c_hard_failures_.fetch_add(1, std::memory_order_relaxed);
            ESP_LOGE(TAG, "read_pcf8574 from 0x%02X failed on retry: %s", addr, esp_err_to_name(ret));
        }
    } else if (ret != ESP_OK) {
        // Log non-timeout failure from first attempt
        i2c_hard_failures_.fetch_add(1, std::memory_order_relaxed);
        ESP_LOGE(TAG, "read_pcf8574 from 0x%02X failed on 1st attempt (not timeout): %s", addr, esp_err_to_name(ret));
    }
    // If ret was ESP_OK on the first attempt, no further logs are printed here for success.
    return ret;
}

// Helper function to check if an I2C device is present at a given address
static bool check_i2c_device_present(const uint8_t addr) {
    if (xSemaphoreTake(i2c_bus_mutex_, pdMS_TO_TICKS(100)) != pdTRUE) { // Longer timeout for initialization
        ESP_LOGE(TAG, "Task '%s' FAILED to take i2c_bus_mutex_ for check_i2c_device_present for 0x%02X after 100ms", pcTaskGetName(NULL), addr);
        return false; // Cannot check, assume not present or error
    }

    i2c_cmd_handle_t cmd_test = i2c_cmd_link_create();

    i2c_master_start(cmd_test);
    i2c_master_write_byte(cmd_test, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd_test);
    esp_err_t test_ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd_test, pdMS_TO_TICKS(25)); // Moderate timeout for device detection
    i2c_cmd_link_delete(cmd_test);

    xSemaphoreGive(i2c_bus_mutex_);
    return test_ret == ESP_OK;
}

esp_err_t kc868_a16_hw_init() {
    if (kc868_a16_initialized) {
        ESP_LOGI(TAG, "KC868-A16 hardware already initialized.");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Initializing KC868-A16 hardware");

    // Create the I2C bus mutex if it hasn't been created yet
    if (i2c_bus_mutex_ == nullptr) {
        i2c_bus_mutex_ = xSemaphoreCreateMutex();
        if (i2c_bus_mutex_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create I2C bus mutex!");
            return ESP_FAIL; // Critical failure
        }
    }

    discovered_pcf8574_input_addr_for_pins_0_7 = 0;
    discovered_pcf8574_input_addr_for_pins_8_15 = 0;

    constexpr i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master = {
            .clk_speed = I2C_MASTER_FREQ_HZ
        }
    };
    
    esp_err_t ret = i2c_param_config(I2C_MASTER_NUM, &conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure I2C parameters: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "I2C driver already installed for port %d.", I2C_MASTER_NUM);
        } else {
            ESP_LOGE(TAG, "Failed to install I2C driver: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    // Allow I2C bus and PCF8574 devices to settle after power-on
    vTaskDelay(pdMS_TO_TICKS(100));

    // Verify Input PCF8574 expanders at their fixed/expected addresses
    ESP_LOGD(TAG, "Verifying Input PCF8574 expanders...");

    // Assuming KC868_A16_HW_EXPECTED_INPUT_ADDR_PINS_0_7 (0x22) and
    // KC868_A16_HW_EXPECTED_INPUT_ADDR_PINS_8_15 (0x21) are defined in kc868_a16_hw.h or similar
    bool found_chip_for_0_7 = check_i2c_device_present(KC868_A16_HW_EXPECTED_INPUT_ADDR_PINS_0_7);
    if (found_chip_for_0_7) {
        // Check that this address is not one of the output expander addresses
        if constexpr (KC868_A16_HW_EXPECTED_INPUT_ADDR_PINS_0_7 == PCF8574_OUTPUT_ADDR_2) {
            ESP_LOGE(TAG, "Input expander address 0x%02X for pins 0-7 conflicts with an output expander address!", KC868_A16_HW_EXPECTED_INPUT_ADDR_PINS_0_7);
            found_chip_for_0_7 = false; // Treat as not found due to conflict
        } else {
            discovered_pcf8574_input_addr_for_pins_0_7 = KC868_A16_HW_EXPECTED_INPUT_ADDR_PINS_0_7;
            ESP_LOGD(TAG, "Input expander for pins 0-7 (X01-X08) verified at 0x%02X.", discovered_pcf8574_input_addr_for_pins_0_7);
        }
    } else {
        ESP_LOGE(TAG, "Input expander for pins 0-7 (X01-X08) NOT found at expected address 0x%02X.", KC868_A16_HW_EXPECTED_INPUT_ADDR_PINS_0_7);
    }

    bool found_chip_for_8_15 = check_i2c_device_present(KC868_A16_HW_EXPECTED_INPUT_ADDR_PINS_8_15);
    if (found_chip_for_8_15) {
        // Check that this address is not one of the output expander addresses or the other input chip
        if constexpr (KC868_A16_HW_EXPECTED_INPUT_ADDR_PINS_8_15 == PCF8574_OUTPUT_ADDR_2) {
            ESP_LOGE(TAG, "Input expander address 0x%02X for pins 8-15 conflicts with an output expander address!", KC868_A16_HW_EXPECTED_INPUT_ADDR_PINS_8_15);
            found_chip_for_8_15 = false;
        } else if (KC868_A16_HW_EXPECTED_INPUT_ADDR_PINS_8_15 == discovered_pcf8574_input_addr_for_pins_0_7) {
            ESP_LOGE(TAG, "Input expander address 0x%02X for pins 8-15 is the same as for pins 0-7!", KC868_A16_HW_EXPECTED_INPUT_ADDR_PINS_8_15);
            found_chip_for_8_15 = false;
        }
        else {
            discovered_pcf8574_input_addr_for_pins_8_15 = KC868_A16_HW_EXPECTED_INPUT_ADDR_PINS_8_15;
            ESP_LOGD(TAG, "Input expander for pins 8-15 (X09-X16) verified at 0x%02X.", discovered_pcf8574_input_addr_for_pins_8_15);
        }
    } else {
        ESP_LOGE(TAG, "Input expander for pins 8-15 (X09-X16) NOT found at expected address 0x%02X.", KC868_A16_HW_EXPECTED_INPUT_ADDR_PINS_8_15);
    }

    if (!found_chip_for_0_7 || !found_chip_for_8_15) {
        ESP_LOGE(TAG, "One or both input PCF8574 expanders not found or address conflict. Please check hardware and I2C addresses.");
        ESP_LOGE(TAG, "Expected input addresses: Pins 0-7 (X01-X08) at 0x%02X, Pins 8-15 (X09-X16) at 0x%02X.",
                 KC868_A16_HW_EXPECTED_INPUT_ADDR_PINS_0_7, KC868_A16_HW_EXPECTED_INPUT_ADDR_PINS_8_15);
        return ESP_FAIL;
    }

    // Initialize Output PCF8574s
    ret = write_pcf8574(PCF8574_OUTPUT_ADDR_1, 0xFF); // Set all output relays to OFF (active-low)
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize PCF8574_1 (Outputs at 0x%02X): %s", PCF8574_OUTPUT_ADDR_1, esp_err_to_name(ret));
        return ret;
    }
    
    ret = write_pcf8574(PCF8574_OUTPUT_ADDR_2, 0xFF); // Set all output relays to OFF (active-low)
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize PCF8574_2 (Outputs at 0x%02X): %s", PCF8574_OUTPUT_ADDR_2, esp_err_to_name(ret));
        return ret;
    }

    // Initialize discovered Input PCF8574s
    ret = write_pcf8574(discovered_pcf8574_input_addr_for_pins_0_7, 0xFF); // Set all pins to high to enable them as inputs
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize PCF8574_INPUT_1 (0x%02x): %s", discovered_pcf8574_input_addr_for_pins_0_7, esp_err_to_name(ret));
        return ret;
    }

    ret = write_pcf8574(discovered_pcf8574_input_addr_for_pins_8_15, 0xFF); // Set all pins to high to enable them as inputs
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize PCF8574_INPUT_2 (0x%02x): %s", discovered_pcf8574_input_addr_for_pins_8_15, esp_err_to_name(ret));
        return ret;
    }

    output_state = 0xFFFF; // All bits high means all relays are off (PCF8574 register view)

    kc868_a16_initialized = true;
    ESP_LOGI(TAG, "KC868-A16 hardware initialized successfully");
    return ESP_OK;
}

esp_err_t kc868_a16_get_output_state(const uint8_t output_num, bool* state) {
    if (output_num >= 16 || state == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    std::lock_guard<std::mutex> state_lock(output_state_mutex_);
    const uint16_t mask = 1 << output_num;
    *state = !(output_state & mask);  // Invert because PCF8574 is active low
    return ESP_OK;
}

esp_err_t kc868_a16_set_all_outputs(const uint16_t state_mask) {
    std::lock_guard<std::mutex> state_lock(output_state_mutex_);

    // Convert to PCF8574 active low logic
    const uint8_t low_byte = ~(state_mask & 0xFF);
    const uint8_t high_byte = ~((state_mask >> 8) & 0xFF);

    esp_err_t ret = write_pcf8574(PCF8574_OUTPUT_ADDR_1, low_byte);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set_all_outputs: Failed to write to ADDR_1 (0x%02X): %s", PCF8574_OUTPUT_ADDR_1, esp_err_to_name(ret));
        return ret;
    }
    // Chip 1's outputs are latched now that its write succeeded, so commit its
    // cache byte before risking chip 2 -- an ADDR_2 failure won't leave it stale.
    output_state = (output_state & 0xFF00) | low_byte;

    ret = write_pcf8574(PCF8574_OUTPUT_ADDR_2, high_byte);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set_all_outputs: Failed to write to ADDR_2 (0x%02X): %s", PCF8574_OUTPUT_ADDR_2, esp_err_to_name(ret));
        return ret;
    }
    output_state = (output_state & 0x00FF) | (static_cast<uint16_t>(high_byte) << 8);
    ESP_LOGD(TAG, "set_all_outputs: Success. PCF8574s updated. Logical mask 0x%04X. output_state (active-low) 0x%04X", state_mask, output_state);
    return ret;
}

// Invert because PCF8574 is active low
uint16_t kc868_a16_get_all_outputs() {
    // This function returns the logical state (1 = ON, 0 = OFF)
    // output_state stores the PCF8574 register view (active-low, 1 = OFF, 0 = ON)
    std::lock_guard<std::mutex> state_lock(output_state_mutex_);
    return ~output_state & 0xFFFF;
}

esp_err_t kc868_a16_get_input_state(const uint8_t input_num, bool* state) {
    if (input_num >= 16 || state == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    if (discovered_pcf8574_input_addr_for_pins_0_7 == 0 || discovered_pcf8574_input_addr_for_pins_8_15 == 0) {
        ESP_LOGE(TAG, "Input expander addresses not discovered or invalid. Call kc868_a16_hw_init() first.");
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t pcf_addr = (input_num < 8) ? discovered_pcf8574_input_addr_for_pins_0_7 : discovered_pcf8574_input_addr_for_pins_8_15;
    const uint8_t bit_pos = input_num % 8;
    
    uint8_t read_byte;

    if (const esp_err_t ret = read_pcf8574(pcf_addr, &read_byte); ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read from PCF8574 addr 0x%02x: %s", pcf_addr, esp_err_to_name(ret));
        // *state is not set in case of error
        return ret;
    }

    // PCF8574 inputs are high (1) if open/pulled-up, low (0) if grounded.
    // Logical 'true' means the input is active/asserted.
    // Assuming active-low: a '0' bit from PCF8574 means active.
    *state = !((read_byte >> bit_pos) & 0x01); 
    return ESP_OK;
}

esp_err_t kc868_a16_get_all_inputs(uint16_t* state_mask) {
    if (state_mask == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    if (discovered_pcf8574_input_addr_for_pins_0_7 == 0 || discovered_pcf8574_input_addr_for_pins_8_15 == 0) {
        ESP_LOGE(TAG, "Input expander addresses not initialized. Call kc868_a16_hw_init() first.");
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t byte_low, byte_high;

    esp_err_t ret = read_pcf8574(discovered_pcf8574_input_addr_for_pins_0_7, &byte_low);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read inputs from PCF8574 for pins 0-7 (0x%02x): %s", discovered_pcf8574_input_addr_for_pins_0_7, esp_err_to_name(ret));
        return ret;
    }

    ret = read_pcf8574(discovered_pcf8574_input_addr_for_pins_8_15, &byte_high);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read inputs from PCF8574 for pins 8-15 (0x%02x): %s", discovered_pcf8574_input_addr_for_pins_8_15, esp_err_to_name(ret));
        return ret;
    }

    const uint16_t combined_raw = (static_cast<uint16_t>(byte_high) << 8) | byte_low;
    *state_mask = ~combined_raw & 0xFFFF; // Invert all bits so '1' means active (active-low inputs)
    
    ESP_LOGV(TAG, "Raw low: 0x%02X, Raw high: 0x%02X, Combined raw: 0x%04X, Final state_mask: 0x%04X",
        byte_low, byte_high, combined_raw, *state_mask);

    return ESP_OK;
}

esp_err_t kc868_a16_get_all_inputs_raw(uint16_t* data) {
    if (data == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    if (discovered_pcf8574_input_addr_for_pins_0_7 == 0 || discovered_pcf8574_input_addr_for_pins_8_15 == 0) {
        ESP_LOGE(TAG, "Input expander addresses not initialized. Call kc868_a16_hw_init() first.");
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t byte_low, byte_high;

    esp_err_t ret = read_pcf8574(discovered_pcf8574_input_addr_for_pins_0_7, &byte_low);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = read_pcf8574(discovered_pcf8574_input_addr_for_pins_8_15, &byte_high);
    if (ret != ESP_OK) {
        return ret;
    }

    // Raw PCF8574 view (NOT inverted): bit '1' = pin HIGH, bit '0' = pin LOW.
    // This matches what determine_ptt_logical_state() in InputManager expects.
    *data = (static_cast<uint16_t>(byte_high) << 8) | byte_low;
    return ESP_OK;
}

esp_err_t kc868_a16_get_inputs_0_7_raw(uint8_t* data) {
    if (data == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    if (discovered_pcf8574_input_addr_for_pins_0_7 == 0) {
        ESP_LOGE(TAG, "Input expander address for pins 0-7 not initialized. Call kc868_a16_hw_init() first.");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = read_pcf8574(discovered_pcf8574_input_addr_for_pins_0_7, data);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read inputs from PCF8574 for pins 0-7 (0x%02x): %s", discovered_pcf8574_input_addr_for_pins_0_7, esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGV(TAG, "Raw data from input expander 0-7 (0x%02X): 0x%02X", discovered_pcf8574_input_addr_for_pins_0_7, *data);
    return ESP_OK;
}

void kc868_a16_hw_scan_i2c_bus() {
    if (!kc868_a16_initialized) {
        ESP_LOGW(TAG, "I2C bus scan: KC868-A16 hardware not fully initialized. Attempting basic I2C setup for scan...");

        constexpr i2c_config_t conf = {
            .mode = I2C_MODE_MASTER,
            .sda_io_num = I2C_MASTER_SDA_IO,
            .scl_io_num = I2C_MASTER_SCL_IO,
            .sda_pullup_en = GPIO_PULLUP_ENABLE,
            .scl_pullup_en = GPIO_PULLUP_ENABLE,
            .master = {
                .clk_speed = I2C_MASTER_FREQ_HZ
            }
        };

        const esp_err_t config_ret = i2c_param_config(I2C_MASTER_NUM, &conf);
        esp_err_t install_ret = ESP_FAIL;

        if (config_ret == ESP_OK) {
            install_ret = i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
        }

        if (config_ret != ESP_OK || (install_ret != ESP_OK && install_ret != ESP_ERR_INVALID_STATE /*already installed*/)) {
            ESP_LOGE(TAG, "Failed to initialize I2C driver for scan (config: %s, install: %s). Aborting scan.",
                     esp_err_to_name(config_ret), esp_err_to_name(install_ret));
            return;
        }
         if (install_ret == ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "I2C driver was already installed for port %d.", I2C_MASTER_NUM);
        }
    }

    ESP_LOGD(TAG, "Scanning I2C bus (addresses 0x08 to 0x77)...");
    uint8_t found_count = 0;
    for (uint8_t i = 0x08; i < 0x78; i++) { // Standard 7-bit address range
        if (check_i2c_device_present(i)) {
            ESP_LOGD(TAG, "I2C device found at address 0x%02X", i);
            found_count++;
        }
    }
    if (found_count == 0) {
        ESP_LOGW(TAG, "No I2C devices found on the bus.");
    } else {
        ESP_LOGD(TAG, "I2C scan complete. Found %d device(s).", found_count);
    }
}

void kc868_a16_get_i2c_stats(kc868_a16_i2c_stats_t* out) {
    if (out == nullptr) {
        return;
    }
    out->first_attempt_errors = i2c_first_attempt_errors_.load(std::memory_order_relaxed);
    out->retry_successes       = i2c_retry_successes_.load(std::memory_order_relaxed);
    out->hard_failures         = i2c_hard_failures_.load(std::memory_order_relaxed);
    out->mutex_failures        = i2c_mutex_failures_.load(std::memory_order_relaxed);
}

uint32_t kc868_a16_get_i2c_error_count() {
    // Quick health probe: total first-attempt failures since boot. Non-zero (and
    // rising) means the 400 kHz overclock is stressing the bus.
    return i2c_first_attempt_errors_.load(std::memory_order_relaxed);
}
