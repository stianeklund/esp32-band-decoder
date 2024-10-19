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

#define UART_NUM UART_NUM_0
#define BUF_SIZE 1024

static const char *TAG = "ANTENNA_SWITCH_MAIN";

static void cat_uart_task(void *pvParameters)
{
    uart_config_t uart_config = {
        .baud_rate = 57600,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };

    ESP_ERROR_CHECK(uart_driver_install(UART_NUM, BUF_SIZE * 2, 0, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM, &uart_config));

    std::unique_ptr<uint8_t[]> data(new uint8_t[BUF_SIZE]);
    std::string command;
    std::string response;

    while (true) {
        int len = uart_read_bytes(UART_NUM, data.get(), BUF_SIZE, 20 / portTICK_PERIOD_MS);
        if (len) {
            for (int i = 0; i < len; i++) {
                if (data[i] == ';') {
                    ESP_LOGI(TAG, "Received CAT command: %s", command.c_str());
                    esp_err_t ret = cat_parser_process_command(command.c_str(), &response[0], response.capacity());
                    if (ret == ESP_OK) {
                        uart_write_bytes(UART_NUM, response.c_str(), response.length());
                    }
                    command.clear();
                } else {
                    command += static_cast<char>(data[i]);
                }
            }
        }
    }
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Initializing Antenna Switch Controller");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Create default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize Wi-Fi
    ESP_ERROR_CHECK(wifi_manager_init());

    // Initialize antenna switch configuration
    ESP_ERROR_CHECK(antenna_switch_init());

    // Initialize CAT parser
    ESP_ERROR_CHECK(cat_parser_init());

    // Initialize relay controller
    ESP_ERROR_CHECK(relay_controller_init());

    ESP_LOGI(TAG, "Antenna Switch Controller initialized successfully");

    // Main loop
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
