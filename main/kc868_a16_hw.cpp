#include "kc868_a16_hw.h"
#include "esp_log.h"

static const char* TAG = "KC868_A16_HW";
static uint16_t output_state = 0;

static esp_err_t write_pcf8574(uint8_t addr, uint8_t data) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, data, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return ret;
}

esp_err_t kc868_a16_hw_init() {
    i2c_config_t conf = {
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
    if (ret != ESP_OK) return ret;
    
    ret = i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
    if (ret != ESP_OK) return ret;

    // Initialize both PCF8574s to all outputs off
    ret = write_pcf8574(PCF8574_OUTPUT_ADDR_1, 0xFF);
    if (ret != ESP_OK) return ret;
    
    ret = write_pcf8574(PCF8574_OUTPUT_ADDR_2, 0xFF);
    if (ret != ESP_OK) return ret;

    output_state = 0;
    return ESP_OK;
}

esp_err_t kc868_a16_set_output(uint8_t output_num, bool state) {
    if (output_num >= 16) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t pcf_addr = (output_num < 8) ? PCF8574_OUTPUT_ADDR_1 : PCF8574_OUTPUT_ADDR_2;
    uint8_t bit_pos = output_num % 8;
    uint8_t current_byte = (output_num < 8) ? 
        (output_state & 0xFF) : ((output_state >> 8) & 0xFF);

    if (state) {
        current_byte &= ~(1 << bit_pos);  // PCF8574 is active low
    } else {
        current_byte |= (1 << bit_pos);
    }

    esp_err_t ret = write_pcf8574(pcf_addr, current_byte);
    if (ret == ESP_OK) {
        if (output_num < 8) {
            output_state = (output_state & 0xFF00) | current_byte;
        } else {
            output_state = (output_state & 0x00FF) | (current_byte << 8);
        }
    }
    return ret;
}

esp_err_t kc868_a16_get_output_state(uint8_t output_num, bool* state) {
    if (output_num >= 16 || state == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t mask = 1 << output_num;
    *state = !(output_state & mask);  // Invert because PCF8574 is active low
    return ESP_OK;
}

esp_err_t kc868_a16_set_all_outputs(uint16_t state_mask) {
    // Convert to PCF8574 active low logic
    uint8_t low_byte = ~(state_mask & 0xFF);
    uint8_t high_byte = ~((state_mask >> 8) & 0xFF);

    esp_err_t ret = write_pcf8574(PCF8574_OUTPUT_ADDR_1, low_byte);
    if (ret != ESP_OK) return ret;

    ret = write_pcf8574(PCF8574_OUTPUT_ADDR_2, high_byte);
    if (ret == ESP_OK) {
        output_state = (~state_mask) & 0xFFFF;
    }
    return ret;
}

uint16_t kc868_a16_get_all_outputs() {
    return ~output_state & 0xFFFF;  // Invert because PCF8574 is active low
}
