#ifndef WEBSERVER_H
#define WEBSERVER_H

#include "esp_err.h"

esp_err_t webserver_init(void);
esp_err_t webserver_start(void);
esp_err_t webserver_stop(void);

#endif // WEBSERVER_H
