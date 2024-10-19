#include "cat_parser.h"
#include "antenna_switch.h"
#include "esp_log.h"
#include "esp_err.h"
#include <string>
#include <sstream>
#include <cstring>

static const char *TAG = "CAT_PARSER";
int bandNumber;
static antenna_switch_config_t current_config;

esp_err_t cat_parser_init()
{
    ESP_LOGI(TAG, "Initializing CAT parser");
    esp_err_t ret = cat_parser_update_config();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize CAT parser configuration: %s", esp_err_to_name(ret));
        return ret;
    }
    return ESP_OK;
}

esp_err_t cat_parser_update_config()
{
    ESP_LOGI(TAG, "Updating CAT parser configuration");
    esp_err_t ret = antenna_switch_get_config(&current_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get antenna switch configuration: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "CAT parser configuration updated successfully");
    return ESP_OK;
}
static esp_err_t decodeBand(const std::string& frequency) {
    uint32_t freq = std::stoul(frequency);
    
    for (int i = 0; i < current_config.num_bands; i++) {
        if (freq >= current_config.bands[i].start_freq && freq <= current_config.bands[i].end_freq) {
            ESP_LOGI(TAG, "Frequency %lu Hz matches band %s", 
                     freq, current_config.bands[i].description);
            return antenna_switch_set_frequency(freq);
        }
    }
    
    ESP_LOGW(TAG, "No matching band found for frequency %lu Hz", freq);
    return ESP_ERR_NOT_FOUND;
}
static esp_err_t process_fa_command(const std::string& command, std::string& response)
{
    uint32_t frequency;
    std::istringstream iss(command.substr(2));
    if (iss >> frequency) {
        ESP_LOGI(TAG, "Setting frequency: %lu Hz", frequency);
        esp_err_t ret = antenna_switch_set_frequency(frequency);
        if (ret == ESP_OK) {
            response = "FA" + std::to_string(frequency) + ";";
        } else {
            response = "?;";
        }
    } else {
        response = "?;";
    }
    return ESP_OK;
}

static esp_err_t process_ai_command(const std::string& command, std::string& response)
{
    int mode;
    std::istringstream iss(command.substr(2));
    if (iss >> mode) {
        ESP_LOGI(TAG, "Setting auto mode: %s", mode ? "ON" : "OFF");
        esp_err_t ret = antenna_switch_set_auto_mode(mode != 0);
        if (ret == ESP_OK) {
            response = "AI" + std::to_string(mode) + ";";
        } else {
            response = "?;";
        }
    } else {
        response = "?;";
    }
    return ESP_OK;
}

static esp_err_t process_ap_command(const std::string& command, std::string& response)
{
    int ports;
    std::istringstream iss(command.substr(2));
    if (iss >> ports) {
        ESP_LOGI(TAG, "Setting antenna ports: %d", ports);
        antenna_switch_config_t config;
        esp_err_t ret = antenna_switch_get_config(&config);
        if (ret == ESP_OK) {
            // Update the num_antenna_ports instead of antenna_ports
            config.num_antenna_ports = ports;
            ret = antenna_switch_set_config(&config);
            if (ret == ESP_OK) {
                response = "AP" + std::to_string(ports) + ";";
            } else {
                response = "?;";
            }
        } else {
            response = "?;";
        }
    } else {
        response = "?;";
    }
    return ESP_OK;
}

esp_err_t cat_parser_process_command(const char* command, char* response, size_t response_size)
{
    ESP_LOGI(TAG, "Processing CAT command: %s", command);

    std::string cmd(command);
    std::string resp;

    if (cmd.compare(0, 2, "FA") == 0) {
        process_fa_command(cmd, resp);
    } else if (cmd.compare(0, 2, "AI") == 0) {
        process_ai_command(cmd, resp);
    } else if (cmd.compare(0, 2, "AP") == 0) {
        process_ap_command(cmd, resp);
    } else {
        resp = "?;";
    }

    strncpy(response, resp.c_str(), response_size - 1);
    response[response_size - 1] = '\0';
    return ESP_OK;
}
