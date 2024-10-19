#ifndef WEBSERVER_H
#define WEBSERVER_H

#include "esp_err.h"

esp_err_t webserver_init(void);
esp_err_t webserver_start(void);
esp_err_t webserver_stop(void);
esp_err_t webserver_restart(void);
bool webserver_is_running(void);

#endif // WEBSERVER_H
