#ifndef ANTENNA_SWITCH_H
#define ANTENNA_SWITCH_H

#include "esp_err.h"

#define MAX_ANTENNAS 8
#define MAX_BANDS 10

typedef struct {
    uint32_t start_freq;
    uint32_t end_freq;
    uint8_t antenna;
} band_config_t;

typedef struct {
    band_config_t bands[MAX_BANDS];
    uint8_t num_bands;
    bool auto_mode;
} antenna_switch_config_t;

esp_err_t antenna_switch_init(void);
esp_err_t antenna_switch_set_config(const antenna_switch_config_t *config);
esp_err_t antenna_switch_get_config(antenna_switch_config_t *config);
esp_err_t antenna_switch_set_antenna(uint32_t frequency);
esp_err_t antenna_switch_set_auto_mode(bool auto_mode);

#endif // ANTENNA_SWITCH_H
