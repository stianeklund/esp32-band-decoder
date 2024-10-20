#include "cat_parser.h"
#include "antenna_switch.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string>
#include <sstream>
#include <cstring>

static const char *TAG = "CAT_PARSER";
int bandNumber;
static antenna_switch_config_t current_config;

#define BUF_SIZE 256
#define UART_TASK_STACK_SIZE 4096
#define UART_QUEUE_SIZE 20

static QueueHandle_t uart_queue;

esp_err_t cat_parser_init()
{
    ESP_LOGI(TAG, "Initializing CAT parser");
    
    // Configure UART
    uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM, BUF_SIZE * 2, BUF_SIZE * 2, UART_QUEUE_SIZE, &uart_queue, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    // Create a task to handle UART events
    xTaskCreate(cat_parser_uart_task, "uart_task", UART_TASK_STACK_SIZE, NULL, 12, NULL);

    esp_err_t ret = cat_parser_update_config();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize CAT parser configuration: %s", esp_err_to_name(ret));
        return ret;
    }
    return ESP_OK;
}

static void cat_parser_uart_task(void *pvParameters)
{
    uart_event_t event;
    uint8_t* data = (uint8_t*) malloc(BUF_SIZE);
    size_t buffered_size;
    int len;
    
    while (1) {
        if (xQueueReceive(uart_queue, (void *)&event, (TickType_t)portMAX_DELAY)) {
            switch(event.type) {
                case UART_DATA:
                    uart_get_buffered_data_len(UART_NUM, &buffered_size);
                    len = uart_read_bytes(UART_NUM, data, std::min(buffered_size, (size_t)BUF_SIZE), 100 / portTICK_PERIOD_MS);
                    if (len > 0) {
                        data[len] = 0; // Null-terminate the received data
                        char response[MAX_CAT_COMMAND_LENGTH];
                        cat_parser_process_command((const char*)data, response, MAX_CAT_COMMAND_LENGTH);
                        uart_write_bytes(UART_NUM, response, strlen(response));
                    }
                    break;
                case UART_FIFO_OVF:
                    ESP_LOGW(TAG, "UART FIFO overflow detected");
                    uart_flush_input(UART_NUM);
                    xQueueReset(uart_queue);
                    break;
                case UART_BUFFER_FULL:
                    ESP_LOGW(TAG, "UART ring buffer full");
                    uart_flush_input(UART_NUM);
                    xQueueReset(uart_queue);
                    break;
                case UART_BREAK:
                    ESP_LOGW(TAG, "UART break detected");
                    break;
                case UART_PARITY_ERR:
                    ESP_LOGW(TAG, "UART parity error");
                    break;
                case UART_FRAME_ERR:
                    ESP_LOGW(TAG, "UART frame error");
                    break;
                default:
                    ESP_LOGW(TAG, "Unhandled UART event type: %d", event.type);
                    break;
            }
        }
    }
    free(data);
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
            esp_err_t ret = antenna_switch_set_frequency(freq);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to set frequency in decodeBand: %s", esp_err_to_name(ret));
            }
            return ret;
        }
    }
    
    ESP_LOGW(TAG, "No matching band found for frequency %lu Hz", freq);
    return ESP_ERR_NOT_FOUND;
}
static esp_err_t process_fa_command(const char* command, char* response, size_t response_size)
{
    uint32_t frequency = 0;
    if (sscanf(command, "%lu", &frequency) == 1) {
        ESP_LOGI(TAG, "Setting frequency: %lu Hz", frequency);
        esp_err_t ret = antenna_switch_set_frequency(frequency);
        if (ret == ESP_OK) {
            snprintf(response, response_size, "FA%011lu;", frequency);
            ESP_LOGI(TAG, "Frequency set successfully");
        } else {
            ESP_LOGE(TAG, "Failed to set frequency: %s", esp_err_to_name(ret));
            snprintf(response, response_size, "?;");
        }
    } else {
        ESP_LOGE(TAG, "Invalid frequency format in command: %s", command);
        snprintf(response, response_size, "?;");
    }
    response[response_size - 1] = '\0';  // Ensure null-termination
    return ESP_OK;
}

static esp_err_t process_ai_command(const char* command, char* response, size_t response_size)
{
    int mode = 0;
    if (sscanf(command, "%d", &mode) == 1) {
        ESP_LOGI(TAG, "Setting auto mode: %s", mode ? "ON" : "OFF");
        esp_err_t ret = antenna_switch_set_auto_mode(mode != 0);
        if (ret == ESP_OK) {
            snprintf(response, response_size, "AI%d;", mode);
        } else {
            strncpy(response, "?;", response_size - 1);
        }
    } else {
        strncpy(response, "?;", response_size - 1);
    }
    return ESP_OK;
}

static esp_err_t process_ap_command(const char* command, char* response, size_t response_size)
{
    int ports = 0;
    if (sscanf(command, "%d", &ports) == 1) {
        ESP_LOGI(TAG, "Setting antenna ports: %d", ports);
        antenna_switch_config_t config;
        esp_err_t ret = antenna_switch_get_config(&config);
        if (ret == ESP_OK) {
            config.num_antenna_ports = ports;
            ret = antenna_switch_set_config(&config);
            if (ret == ESP_OK) {
                snprintf(response, response_size, "AP%d;", ports);
            } else {
                strncpy(response, "?;", response_size - 1);
            }
        } else {
            strncpy(response, "?;", response_size - 1);
        }
    } else {
        strncpy(response, "?;", response_size - 1);
    }
    return ESP_OK;
}

esp_err_t cat_parser_process_command(const char* command, char* response, size_t response_size)
{
    ESP_LOGI(TAG, "Processing CAT command: %s", command);

    size_t cmd_len = strlen(command);
    if (cmd_len == 0 || cmd_len >= response_size) {
        strncpy(response, "?;", response_size - 1);
        response[response_size - 1] = '\0';
        return ESP_OK;
    }

    // Remove trailing newline, carriage return, and semicolon
    while (cmd_len > 0 && (command[cmd_len - 1] == '\n' || command[cmd_len - 1] == '\r' || command[cmd_len - 1] == ';')) {
        cmd_len--;
    }

    if (cmd_len < 2) {
        strncpy(response, "?;", response_size - 1);
        response[response_size - 1] = '\0';
        return ESP_OK;
    }

    esp_err_t ret = ESP_OK;
    switch (command[0]) {
        case 'F':
            if (command[1] == 'A') {
                ret = process_fa_command(command + 2, response, response_size);
            } else {
                strncpy(response, "?;", response_size - 1);
            }
            break;
        case 'A':
            if (command[1] == 'I') {
                ret = process_ai_command(command + 2, response, response_size);
            } else if (command[1] == 'P') {
                ret = process_ap_command(command + 2, response, response_size);
            } else {
                strncpy(response, "?;", response_size - 1);
            }
            break;
        default:
            strncpy(response, "?;", response_size - 1);
    }

    response[response_size - 1] = '\0';
    return ret;
}
