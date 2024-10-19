#ifndef RELAY_CONTROLLER_H
#define RELAY_CONTROLLER_H

#include "esp_err.h"

esp_err_t relay_controller_init(void);
esp_err_t relay_controller_set_antenna(uint8_t antenna);

#endif // RELAY_CONTROLLER_H
