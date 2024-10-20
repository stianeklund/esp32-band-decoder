#include <cstdio>
#include <memory>
#include <string>
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "antenna_switch.h"
#include "portmacro.h"
#include "wifi_manager.h"
#include "webserver.h"
#include "cat_parser.h"
#include "relay_controller.h"
#include "driver/uart.h"

#define UART_NUM UART_NUM_2

static const char *TAG = "ANTENNA_SWITCH_MAIN";

// Global pointer to RelayController
static RelayController* g_relay_controller = nullptr;

// Task to periodically update relay states
// void update_relay_states_task(void* pvParameters)
// {
    // TODO we should pause this if we're sending commands or switching so we don't spam the controller
    // or consider removing entirely
    // while (true) {
    //     if (g_relay_controller) {
    //         esp_err_t ret = g_relay_controller->update_all_relay_states();
    //         if (ret != ESP_OK) {
    //             ESP_LOGW(TAG, "Failed to update relay states: %s", esp_err_to_name(ret));
    //         }
    //     }
    //     vTaskDelay(pdMS_TO_TICKS(5000)); // Update every 5 seconds
    // }
// }

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Initializing Antenna Switch Controller");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    // Create RelayController instance
    RelayController relay_controller;
    ESP_ERROR_CHECK(ret);

    // Create default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize Wi-Fi
    ESP_ERROR_CHECK(wifi_manager_init());

    // Initialize antenna switch configuration
    ESP_ERROR_CHECK(antenna_switch_init());

    // Initialize CAT parser
    ESP_ERROR_CHECK(cat_parser_init());

    // Get the current configuration
    antenna_switch_config_t config;
    ESP_ERROR_CHECK(antenna_switch_get_config(&config));

    // Set UDP host and port for relay controller from the configuration
    ESP_LOGI(TAG, "Setting udp host: %s : %i from config", config.udp_host, config.udp_port);
    // TODO consider changing the ctor here instead or pass ptr to config directly?
    relay_controller.set_udp_host(config.udp_host);
    relay_controller.set_udp_port(config.udp_port);

    // Initialize relay controller
    ESP_ERROR_CHECK(relay_controller.init());

    // Update UDP settings after initialization
    ESP_ERROR_CHECK(relay_controller.update_udp_settings(config.udp_host, config.udp_port));

    // Set the global pointer to the RelayController instance
    g_relay_controller = &relay_controller;

    // Create task to periodically update relay states
    // xTaskCreate(update_relay_states_task, "update_relay_states", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Antenna Switch Controller initialized successfully");

    // Main loop
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
