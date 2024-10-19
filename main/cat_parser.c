#include "cat_parser.h"
#include "antenna_switch.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "CAT_PARSER";

esp_err_t cat_parser_init(void)
{
    ESP_LOGI(TAG, "Initializing CAT parser");
    return ESP_OK;
}

static esp_err_t process_fa_command(const char* command, char* response, size_t response_size)
{
    uint32_t frequency;
    if (sscanf(command, "FA%u", &frequency) == 1) {
        ESP_LOGI(TAG, "Setting frequency: %u Hz", frequency);
        esp_err_t ret = antenna_switch_set_antenna(frequency);
        if (ret == ESP_OK) {
            snprintf(response, response_size, "FA%011u;", frequency);
        } else {
            snprintf(response, response_size, "?;");
        }
    } else {
        snprintf(response, response_size, "?;");
    }
    return ESP_OK;
}

static esp_err_t process_am_command(const char* command, char* response, size_t response_size)
{
    int mode;
    if (sscanf(command, "AM%d", &mode) == 1) {
        ESP_LOGI(TAG, "Setting auto mode: %s", mode ? "ON" : "OFF");
        esp_err_t ret = antenna_switch_set_auto_mode(mode != 0);
        if (ret == ESP_OK) {
            snprintf(response, response_size, "AM%d;", mode);
        } else {
            snprintf(response, response_size, "?;");
        }
    } else {
        snprintf(response, response_size, "?;");
    }
    return ESP_OK;
}

esp_err_t cat_parser_process_command(const char* command, char* response, size_t response_size)
{
    ESP_LOGI(TAG, "Processing CAT command: %s", command);

    if (strncmp(command, "FA", 2) == 0) {
        return process_fa_command(command, response, response_size);
    } else if (strncmp(command, "AM", 2) == 0) {
        return process_am_command(command, response, response_size);
    } else {
        snprintf(response, response_size, "?;");
        return ESP_OK;
    }
}
