// ReSharper disable CppRedundantParentheses
#include "include/kc868_a16_hw.h"
#include "esp_log.h"

static auto TAG = "KC868_A16_HW";
static uint16_t output_state = 0;
static bool kc868_a16_initialized = false; // Flag to track initialization status

// Variables to store I2C addresses for input expanders
// These will be set to KC868_A16_HW_EXPECTED_INPUT_ADDR_PINS_0_7 and KC868_A16_HW_EXPECTED_INPUT_ADDR_PINS_8_15
static uint8_t discovered_pcf8574_input_addr_for_pins_0_7 = 0;
static uint8_t discovered_pcf8574_input_addr_for_pins_8_15 = 0;

static esp_err_t write_pcf8574(const uint8_t addr, const uint8_t data) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, data, true);
    i2c_master_stop(cmd);

    const esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    
    // vTaskDelay(pdMS_TO_TICKS(10)); // Commented out for PTT optimization - restore if I2C issues occur
    return ret;
}

static esp_err_t read_pcf8574(const uint8_t addr, uint8_t *data) {
    if (data == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, data, I2C_MASTER_NACK);
    i2c_master_stop(cmd);

    const esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    
    // vTaskDelay(pdMS_TO_TICKS(10)); // Commented out for PTT optimization - restore if I2C issues occur
    return ret;
}

// Helper function to check if an I2C device is present at a given address
static bool check_i2c_device_present(const uint8_t addr) {
    i2c_cmd_handle_t cmd_test = i2c_cmd_link_create();

    i2c_master_start(cmd_test);
    i2c_master_write_byte(cmd_test, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd_test);
    esp_err_t test_ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd_test, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd_test);
    return test_ret == ESP_OK;
}

esp_err_t kc868_a16_hw_init() {
    if (kc868_a16_initialized) {
        ESP_LOGI(TAG, "KC868-A16 hardware already initialized.");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Initializing KC868-A16 hardware");

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

    // Verify Input PCF8574 expanders at their fixed/expected addresses
    ESP_LOGI(TAG, "Verifying Input PCF8574 expanders...");

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
            ESP_LOGI(TAG, "Input expander for pins 0-7 (X01-X08) verified at 0x%02X.", discovered_pcf8574_input_addr_for_pins_0_7);
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
            ESP_LOGI(TAG, "Input expander for pins 8-15 (X09-X16) verified at 0x%02X.", discovered_pcf8574_input_addr_for_pins_8_15);
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

esp_err_t kc868_a16_set_output(const uint8_t output_num, const bool state) {
    if (output_num >= 16) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t pcf_addr = (output_num < 8) ? PCF8574_OUTPUT_ADDR_1 : PCF8574_OUTPUT_ADDR_2;
    const uint8_t bit_pos = output_num % 8;
    uint8_t current_byte = (output_num < 8) ?
        (output_state & 0xFF) : ((output_state >> 8) & 0xFF);

    if (state) {
        current_byte &= ~(1 << bit_pos);  // PCF8574 is active low
    } else {
        current_byte |= (1 << bit_pos);
    }

    // Removed vTaskDelay here, relying on delay in write_pcf8574

    const esp_err_t ret = write_pcf8574(pcf_addr, current_byte);

    if (ret == ESP_OK) {
        if (output_num < 8) {
            output_state = (output_state & 0xFF00) | current_byte;
        } else {
            output_state = (output_state & 0x00FF) | (current_byte << 8);
        }
        // Removed vTaskDelay here
    }
    return ret;
}

esp_err_t kc868_a16_get_output_state(const uint8_t output_num, bool* state) {
    if (output_num >= 16 || state == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint16_t mask = 1 << output_num;
    *state = !(output_state & mask);  // Invert because PCF8574 is active low
    return ESP_OK;
}

esp_err_t kc868_a16_set_all_outputs(const uint16_t state_mask) {
    // Convert to PCF8574 active low logic
    const uint8_t low_byte = ~(state_mask & 0xFF);
    const uint8_t high_byte = ~((state_mask >> 8) & 0xFF);

    esp_err_t ret = write_pcf8574(PCF8574_OUTPUT_ADDR_1, low_byte);
    if (ret != ESP_OK) return ret;

    ret = write_pcf8574(PCF8574_OUTPUT_ADDR_2, high_byte);
    if (ret == ESP_OK) {
        output_state = (~state_mask) & 0xFFFF;
    }
    return ret;
}

// Invert because PCF8574 is active low
uint16_t kc868_a16_get_all_outputs() {
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
        byte_low, byte_high, combined_raw, *state_mask); // Example of a debug log if needed

    return ESP_OK;
}

void kc868_a16_hw_scan_i2c_bus() {
    if (!kc868_a16_initialized) {
        ESP_LOGW(TAG, "I2C bus scan: KC868-A16 hardware not fully initialized. Attempting basic I2C setup for scan...");
        // Attempt to initialize I2C driver if not already done by a full kc868_a16_hw_init()
        // This is a minimal setup for scanning, full init might still be needed for device operation.

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

    ESP_LOGI(TAG, "Scanning I2C bus (addresses 0x08 to 0x77)...");
    uint8_t found_count = 0;
    for (uint8_t i = 0x08; i < 0x78; i++) { // Standard 7-bit address range
        if (check_i2c_device_present(i)) {
            ESP_LOGI(TAG, "I2C device found at address 0x%02X", i);
            found_count++;
        }
    }
    if (found_count == 0) {
        ESP_LOGI(TAG, "No I2C devices found on the bus.");
    } else {
        ESP_LOGI(TAG, "I2C scan complete. Found %d device(s).", found_count);
    }
}
