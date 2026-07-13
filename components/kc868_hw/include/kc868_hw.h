#ifndef KC868_HW_H
#define KC868_HW_H

#include "esp_err.h"
#include "driver/i2c.h"
#include "sdkconfig.h"

// --- Board I/O sizing (selected via menuconfig "Band Decoder Board") ---
// The A6 (6ch) and A8 (8ch) populate only the first output expander (0x24) and
// the first input expander (0x22); the A16 adds a second of each (0x25, 0x21).
// All the hardware ceiling is expressed through these two counts so the rest of
// the HAL can skip the absent chips instead of NACKing on them.
#if defined(CONFIG_BOARD_KC868_A6)
#define KC868_HW_NUM_RELAYS 6
#define KC868_HW_NUM_INPUTS 6
#elif defined(CONFIG_BOARD_KC868_A8)
#define KC868_HW_NUM_RELAYS 8
#define KC868_HW_NUM_INPUTS 8
#else // CONFIG_BOARD_KC868_A16 (default)
#define KC868_HW_NUM_RELAYS 16
#define KC868_HW_NUM_INPUTS 16
#endif

// Max antenna ports selectable per radio (<= MAX_ANTENNA_PORTS = 8). A board with
// fewer than 8 relays caps here so the UI/config can't request a port with no
// relay behind it (the A16 keeps the full 8 per radio).
#define KC868_HW_MAX_PORTS_PER_RADIO (KC868_HW_NUM_RELAYS < 8 ? KC868_HW_NUM_RELAYS : 8)

// PCF8574 I2C addresses
#define PCF8574_OUTPUT_ADDR_1 0x24  // First output expander (D0-D7)
#define PCF8574_OUTPUT_ADDR_2 0x25  // Second output expander (D8-D15), A16 only

// --- Configuration for Input PCF8574 Expanders ---
// The implementation now always uses these fixed addresses.
// One chip handles inputs X01-X08 (logical 0-7), the other X09-X16 (logical 8-15).
#define KC868_HW_EXPECTED_INPUT_ADDR_PINS_0_7   0x22 // Expected I2C address for input chip handling pins 0-7 (X01-X08)
#define KC868_HW_EXPECTED_INPUT_ADDR_PINS_8_15  0x21 // Expected I2C address for input chip handling pins 8-15 (X09-X16)
// --- End Configuration for Input PCF8574 Expanders ---

// I2C configuration. SDA is GPIO4 on all supported boards; SCL differs by board:
// the KC868-A6 routes SCL to GPIO15 (a boot strapping pin, but fine as I2C SCL
// after reset), while the A8/A16 use GPIO5.
#if defined(CONFIG_BOARD_KC868_A6)
#define I2C_MASTER_SCL_IO GPIO_NUM_15       // SCL pin (KC868-A6)
#else
#define I2C_MASTER_SCL_IO GPIO_NUM_5        // SCL pin (KC868-A8/A16)
#endif
#define I2C_MASTER_SDA_IO GPIO_NUM_4        // SDA pin
// PCF8574 is rated for 100 kHz. We run at 400 kHz (out of spec) which works on
// this hardware; kc868_hw_get_i2c_error_count() instruments NACK/timeout rates
// so we can tell whether the overclock is actually causing bus errors before
// deciding to drop back to 100000.
#define I2C_MASTER_FREQ_HZ 400000   // 400 kHz (overclocked; see note above)
#define I2C_MASTER_NUM I2C_NUM_0    // I2C port number

esp_err_t kc868_hw_init();

// Physical I/O counts for the selected board (8 on A8, 16 on A16). Consumers use
// these to bound relay/input/PTT-pin ranges instead of hardcoding 16.
uint8_t kc868_hw_num_relays(void);
uint8_t kc868_hw_num_inputs(void);

esp_err_t kc868_hw_get_output_state(uint8_t output_num, bool* state);
esp_err_t kc868_hw_set_all_outputs(uint16_t state_mask);
uint16_t kc868_hw_get_all_outputs();
esp_err_t kc868_hw_get_input_state(uint8_t input_num, bool* state);
esp_err_t kc868_hw_get_all_inputs(uint16_t* state_mask);
esp_err_t kc868_hw_get_inputs_0_7_raw(uint8_t* data);
// Raw 16-bit PCF8574 input view (NOT inverted): bit '1' = pin HIGH, '0' = pin LOW.
esp_err_t kc868_hw_get_all_inputs_raw(uint16_t* data);

void kc868_hw_scan_i2c_bus();

// I2C bus error instrumentation (see I2C_MASTER_FREQ_HZ note above). Counters are
// cumulative since boot and safe to read from any task.
typedef struct {
    uint32_t first_attempt_errors; // transactions that failed on the first try (timeout or NACK)
    uint32_t retry_successes;      // recovered on the second attempt
    uint32_t hard_failures;        // failed even after the retry (or a non-retryable error)
    uint32_t mutex_failures;       // could not acquire the I2C bus mutex
} kc868_hw_i2c_stats_t;

void kc868_hw_get_i2c_stats(kc868_hw_i2c_stats_t* out);
uint32_t kc868_hw_get_i2c_error_count(void);

#endif // KC868_HW_H
