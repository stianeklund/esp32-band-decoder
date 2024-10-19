#pragma once

#include "esp_err.h"
#include "relay_controller.h"

class SystemInitializer {
public:
    static esp_err_t initialize_basic();
    static esp_err_t initialize_full(RelayController** relay_controller_out);

private:
    static esp_err_t init_nvs();
    static esp_err_t init_task_watchdog();
};
