#ifndef KC868_A16_HW_H
#define KC868_A16_HW_H

#include "esp_err.h"
#include "driver/i2c.h"

// PCF8574 I2C addresses (typical for KC868-A16)
#define PCF8574_OUTPUT_ADDR_1 0x24  // First output expander (D0-D7)
#define PCF8574_OUTPUT_ADDR_2 0x25  // Second output expander (D8-D15)

#define PCF8574_INPUT_ADDR_PINS_0_7 0x21  // Pins X01-X08 (logical 0-7)
#define PCF8574_INPUT_ADDR_PINS_8_15 0x20  // Pins X09-X16 (logical 8-15)

// --- Configuration for Input PCF8574 Expanders ---
// The implementation now always uses these fixed addresses.
// One chip handles inputs X01-X08 (logical 0-7), the other X09-X16 (logical 8-15).
#define KC868_A16_HW_EXPECTED_INPUT_ADDR_PINS_0_7   0x22 // Expected I2C address for input chip handling pins 0-7 (X01-X08)
#define KC868_A16_HW_EXPECTED_INPUT_ADDR_PINS_8_15  0x21 // Expected I2C address for input chip handling pins 8-15 (X09-X16)
// --- End Configuration for Input PCF8574 Expanders ---

// I2C configuration
#define I2C_MASTER_SCL_IO GPIO_NUM_5        // SCL pin
#define I2C_MASTER_SDA_IO GPIO_NUM_4        // SDA pin
// #define I2C_MASTER_FREQ_HZ 100000   // 100 kHz - PCF8574 spec limit
#define I2C_MASTER_FREQ_HZ 400000   // 400 kHz (the PCF8574 can only do 100 kHz.. overclocking seems  to work OK)
#define I2C_MASTER_NUM I2C_NUM_0    // I2C port number

esp_err_t kc868_a16_hw_init();
esp_err_t kc868_a16_set_output(uint8_t output_num, bool state);
esp_err_t kc868_a16_get_output_state(uint8_t output_num, bool* state);
esp_err_t kc868_a16_set_all_outputs(uint16_t state_mask);
uint16_t kc868_a16_get_all_outputs();
esp_err_t kc868_a16_get_input_state(uint8_t input_num, bool* state);
esp_err_t kc868_a16_get_all_inputs(uint16_t* state_mask);
esp_err_t kc868_a16_get_inputs_0_7_raw(uint8_t* data);

void kc868_a16_hw_scan_i2c_bus();

#endif // KC868_A16_HW_H
