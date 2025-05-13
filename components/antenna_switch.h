#ifndef ANTENNA_SWITCH_H
#define ANTENNA_SWITCH_H

#include "esp_err.h"
#include <cstdint>   // for uint8_t

// don’t #include relay_controller.h here to avoid circular dependency;
// just forward-declare RelayController for the C++ API below
#ifdef __cplusplus
class RelayController;
#endif

// Constants and structs (these can be used from both C and C++)

// Enum for Radio Operation Mode - must be defined before antenna_switch_config_t
typedef enum {
    RADIO_OP_MODE_SINGLE_A,         // Only Radio A is active. Radio B controls are disabled.
    RADIO_OP_MODE_ALTERNATING_AB,   // Radio A or Radio B can be active, but not simultaneously. Selecting one deactivates the other.
    RADIO_OP_MODE_CONCURRENT_AB     // Radio A and Radio B can be active simultaneously on different antennas.
} radio_operation_mode_t;

#define MAX_BANDS 10
#define MAX_ANTENNA_PORTS 8

typedef struct band_config {
    char description[32];
    uint32_t start_freq;
    uint32_t end_freq;
    bool antenna_ports[MAX_ANTENNA_PORTS];
} band_config_t;

typedef struct {
    bool auto_mode;
    uint8_t num_bands;
    uint8_t num_antenna_ports;
    // now 2 radios: index 0 = A, 1 = B
    band_config_t bands[2][MAX_BANDS];
    int uart_baud_rate;
    uint8_t uart_parity;
    uint8_t uart_stop_bits;
    uint8_t uart_flow_ctrl;
    int8_t uart_tx_pin;  // GPIO pin number for UART TX
    int8_t uart_rx_pin;  // GPIO pin number for UART RX
    bool mqtt_enabled;
    bool allow_concurrent_data_sources;
    // bool enable_radio_b; // Replaced by radio_operation_mode
    // bool allow_multi_select; // Replaced by radio_operation_mode
    bool interlock_auto_resolves_conflict; // Renamed from interlock_enabled
    char mqtt_broker[64];
    uint16_t mqtt_port;
    char mqtt_rig_id[16];
    char mqtt_username[32];
    char mqtt_password[32];
    char mqtt_client_id[32];
    char mqtt_topic[64];
    radio_operation_mode_t radio_operation_mode;
} antenna_switch_config_t;

// Enum to identify Radio A or Radio B
enum class RadioID : uint8_t {
    A = 0,
    B = 1
};

// C interface
#ifdef __cplusplus
extern "C" {
#endif

esp_err_t antenna_switch_init();
esp_err_t antenna_switch_set_config(const antenna_switch_config_t *config);
esp_err_t antenna_switch_get_config(antenna_switch_config_t *config);
esp_err_t antenna_switch_set_frequency(uint32_t frequency);
esp_err_t antenna_switch_set_auto_mode(bool auto_mode);
esp_err_t antenna_switch_set_relay(int relay_id, bool state);
esp_err_t antenna_switch_get_relay_state(int relay_id, bool *state);
esp_err_t antenna_switch_restart();

#ifdef __cplusplus
} // extern "C"
#endif

#ifdef __cplusplus
// enum class RadioID : uint8_t { A = 0, B = 1 }; // Moved above C interface block

// C++ specific declarations
void antenna_switch_set_relay_controller(RelayController* controller);

// Extended API: select by radio (default Radio A)
esp_err_t antenna_switch_set_relay_for_antenna(int relay_id, int band_number, RadioID radio, bool state);
inline esp_err_t antenna_switch_set_relay_for_antenna(int relay_id, int band_number, bool state) {
    return antenna_switch_set_relay_for_antenna(relay_id, band_number, RadioID::A, state);
}

// Direct Radio B control (non-band selection)
esp_err_t antenna_switch_set_relay_radio_b(int relay_id, bool state);

#endif

#endif // ANTENNA_SWITCH_H
