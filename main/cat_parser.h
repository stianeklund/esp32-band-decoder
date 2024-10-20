#ifndef CAT_PARSER_H
#define CAT_PARSER_H

#include "esp_err.h"
#include "driver/uart.h"
#include <stdbool.h>

#define MAX_CAT_COMMAND_LENGTH 32
#define UART_NUM UART_NUM_2
#define UART_TX_PIN 17
#define UART_RX_PIN 16
#define UART_BAUD_RATE 9600
#define BUF_SIZE 1024

esp_err_t cat_parser_init(void);
esp_err_t cat_parser_process_command(const char* command, char* response, size_t response_size);
esp_err_t cat_parser_update_config(void);
static void cat_parser_uart_task(void *pvParameters);

#endif // CAT_PARSER_H
