#pragma once

#include "esp_err.h"
#include "esp_log.h"

class RestartManager {
public:
    static esp_err_t check_restart_count();
    static void clear_restart_count();
    static void store_error_state(esp_err_t error);

private:
    static constexpr uint8_t MAX_RESTART_ATTEMPTS = 3;
    static const char* RESTART_COUNTER_KEY;
    static const char* ERROR_STATE_KEY;
};
