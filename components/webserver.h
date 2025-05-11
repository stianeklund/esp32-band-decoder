#ifndef WEBSERVER_H
#define WEBSERVER_H

#include "esp_err.h"

esp_err_t webserver_init();

esp_err_t webserver_start();

esp_err_t webserver_stop();

esp_err_t webserver_restart();

bool webserver_is_running();

#endif // WEBSERVER_H
