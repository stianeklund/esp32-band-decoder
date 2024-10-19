#ifndef RELAY_CONTROLLER_H
#define RELAY_CONTROLLER_H

#include "esp_err.h"

#ifdef __cplusplus
#include <cstdint>
#else
#include <stdint.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t relay_controller_init();
esp_err_t relay_controller_set_antenna(const bool* antenna_ports);

#ifdef __cplusplus
}
#endif

#endif // RELAY_CONTROLLER_H
