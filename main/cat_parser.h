#ifndef CAT_PARSER_H
#define CAT_PARSER_H

#include "esp_err.h"
#include <stdbool.h>

#define MAX_CAT_COMMAND_LENGTH 32

esp_err_t cat_parser_init(void);
esp_err_t cat_parser_process_command(const char* command, char* response, size_t response_size);
esp_err_t cat_parser_update_config(void);

#endif // CAT_PARSER_H
