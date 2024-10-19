#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H
#include "freertos/FreeRTOS.h"
#include "esp_err.h"
#include "freertos/event_groups.h"
#include <stdbool.h>

// Event group bits
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT     BIT1
#define ESPTOUCH_DONE_BIT BIT2


esp_err_t wifi_manager_init(void);

bool wifi_manager_is_connected(void);
esp_err_t wifi_manager_wait_for_connection(uint32_t timeout_ms);

esp_err_t wifi_manager_get_ip_info(char *ip_addr, size_t ip_addr_size);

esp_err_t wifi_manager_get_mac_address(char *mac_addr, size_t mac_addr_size);

esp_err_t wifi_manager_connect_sta(const char *ssid, const char *password);

esp_err_t wifi_manager_disconnect(void);

esp_err_t wifi_manager_clear_credentials(void);

#endif // WIFI_MANAGER_H
