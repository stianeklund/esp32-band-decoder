#include "restart_manager.h"

#include <nvs.h>

static auto TAG = "RESTART_MANAGER";

const char* RestartManager::RESTART_COUNTER_KEY = "restart_cnt";
const char* RestartManager::ERROR_STATE_KEY = "last_error";

esp_err_t RestartManager::check_restart_count() {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) return err;

    uint8_t restart_count = 0;
    err = nvs_get_u8(nvs_handle, RESTART_COUNTER_KEY, &restart_count);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        restart_count = 0;
    }

    restart_count++;
    ESP_LOGI(TAG, "System restart count: %d/%d", restart_count, MAX_RESTART_ATTEMPTS);

    err = nvs_set_u8(nvs_handle, RESTART_COUNTER_KEY, restart_count);
    if (err == ESP_OK) {
        nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);
    return (restart_count >= MAX_RESTART_ATTEMPTS) ? ESP_FAIL : ESP_OK;
}

void RestartManager::clear_restart_count() {
    nvs_handle_t nvs_handle;
    if (nvs_open("storage", NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, RESTART_COUNTER_KEY, 0);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        ESP_LOGI(TAG, "Restart counter cleared");
    }
}

void RestartManager::store_error_state(const esp_err_t error) {
    nvs_handle_t nvs_handle;
    if (nvs_open("storage", NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_i32(nvs_handle, ERROR_STATE_KEY, error);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        ESP_LOGI(TAG, "Error state stored: %s", esp_err_to_name(error));
    }
}
