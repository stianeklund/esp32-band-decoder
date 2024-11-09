#ifndef ANTENNA_SWITCH_H
#define ANTENNA_SWITCH_H

#include "esp_err.h"
#include "relay_controller.h"

// Constants and structs (these can be used from both C and C++)
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
    band_config_t bands[MAX_BANDS];
    char tcp_host[16];
    uint16_t tcp_port;
    int uart_baud_rate;
    uint8_t uart_parity;
    uint8_t uart_stop_bits;
    uint8_t uart_flow_ctrl;
    int8_t uart_tx_pin;  // GPIO pin number for UART TX
    int8_t uart_rx_pin;  // GPIO pin number for UART RX
} antenna_switch_config_t;

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
esp_err_t antenna_switch_set_tcp_host(const char *host);
esp_err_t antenna_switch_set_tcp_port(uint16_t port);
esp_err_t antenna_switch_restart();

#ifdef __cplusplus
}

// C++ specific declarations
void antenna_switch_set_relay_controller(std::unique_ptr<RelayController> controller);
#endif

#endif // ANTENNA_SWITCH_H
