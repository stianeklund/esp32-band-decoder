#ifndef ANTENNA_SWITCH_H
#define ANTENNA_SWITCH_H

#include "esp_err.h"
#include "relay_controller.h"
#include <stdbool.h>
#include <string>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_BANDS 10
#define MAX_ANTENNA_PORTS 8  // Adjust this based on your hardware

typedef struct band_config {
    char description[32];
    uint32_t start_freq;
    uint32_t end_freq;
    bool antenna_ports[MAX_ANTENNA_PORTS];  // Array of booleans to indicate which ports are available for this band
} band_config_t;

typedef struct {
    bool auto_mode;
    uint8_t num_bands;
    uint8_t num_antenna_ports;  // Total number of antenna ports available
    band_config_t bands[MAX_BANDS];
    char udp_host[16];  // UDP host IP address
    uint16_t udp_port;  // UDP port
} antenna_switch_config_t;

esp_err_t antenna_switch_init();
esp_err_t antenna_switch_set_config(const antenna_switch_config_t *config);
esp_err_t antenna_switch_get_config(antenna_switch_config_t *config);
esp_err_t antenna_switch_set_frequency(uint32_t frequency);
esp_err_t antenna_switch_set_auto_mode(bool auto_mode);
esp_err_t antenna_switch_set_relay(int relay_id, bool state);
esp_err_t antenna_switch_get_relay_state(int relay_id, bool *state);
esp_err_t antenna_switch_set_udp_host(const char* host);
esp_err_t antenna_switch_set_udp_port(uint16_t port);

#ifdef __cplusplus
}
#endif

#endif // ANTENNA_SWITCH_H
