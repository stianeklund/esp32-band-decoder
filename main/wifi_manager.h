#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"

esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_start_ap(void);
esp_err_t wifi_manager_connect_sta(const char* ssid, const char* password);
esp_err_t wifi_manager_disconnect(void);
bool wifi_manager_is_connected(void);
esp_err_t wifi_manager_get_ip_info(char* ip_addr, size_t ip_addr_size);

#endif // WIFI_MANAGER_H
