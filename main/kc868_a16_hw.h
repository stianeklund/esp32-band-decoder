#ifndef KC868_A16_HW_H
#define KC868_A16_HW_H

#include "esp_err.h"
#include "driver/i2c.h"

// PCF8574 I2C addresses (typical for KC868-A16)
#define PCF8574_OUTPUT_ADDR_1 0x24  // First output expander (D0-D7)
#define PCF8574_OUTPUT_ADDR_2 0x25  // Second output expander (D8-D15)
#define PCF8574_INPUT_ADDR_1 0x26  // First input (X01-X08)
#define PCF8574_INPUT_ADDR_2 0x27  // Second input (X09-X16)

// I2C configuration
#define I2C_MASTER_SCL_IO GPIO_NUM_5        // SCL pin
#define I2C_MASTER_SDA_IO GPIO_NUM_4        // SDA pin
#define I2C_MASTER_FREQ_HZ 100000   // 100kHz
#define I2C_MASTER_NUM I2C_NUM_0    // I2C port number

esp_err_t kc868_a16_hw_init();
esp_err_t kc868_a16_set_output(uint8_t output_num, bool state);
esp_err_t kc868_a16_get_output_state(uint8_t output_num, bool* state);
esp_err_t kc868_a16_set_all_outputs(uint16_t state_mask);
uint16_t kc868_a16_get_all_outputs();

#endif // KC868_A16_HW_H
