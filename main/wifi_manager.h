#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>

esp_err_t wifi_manager_init(void);
bool wifi_manager_is_connected(void);
esp_err_t wifi_manager_get_ip_info(char* ip_addr, size_t ip_addr_size);
esp_err_t wifi_manager_get_mac_address(char* mac_addr, size_t mac_addr_size);
esp_err_t wifi_manager_connect_sta(const char* ssid, const char* password);
esp_err_t wifi_manager_disconnect(void);

#endif // WIFI_MANAGER_H
