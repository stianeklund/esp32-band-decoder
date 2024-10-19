
#include "wifi_manager.h"
#include "lwip/ip4_addr.h"
#include "webserver.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/event_groups.h"
#include "esp_timer.h"
#include "esp_smartconfig.h"
#include "webserver.h"
#include "nvs_flash.h"
#include "nvs.h"

#include <cstring>

constexpr EventBits_t WIFI_CONNECTED_BIT = BIT0;
constexpr EventBits_t WIFI_FAIL_BIT = BIT1;
constexpr EventBits_t ESPTOUCH_DONE_BIT = BIT2;

static const char *TAG = "WIFI_MANAGER";
static EventGroupHandle_t s_wifi_event_group;
static esp_netif_t *s_sta_netif = nullptr;

static void smartconfig_task(void* parm);

static esp_err_t save_wifi_config(const char* ssid, const char* password)
{
    nvs_handle_t my_handle;
    esp_err_t err;

    err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(my_handle, "wifi_ssid", ssid);
    if (err != ESP_OK) {
        nvs_close(my_handle);
        return err;
    }

    err = nvs_set_str(my_handle, "wifi_password", password);
    if (err != ESP_OK) {
        nvs_close(my_handle);
        return err;
    }

    err = nvs_commit(my_handle);
    nvs_close(my_handle);
    return err;
}

static esp_err_t load_wifi_config(char* ssid, size_t ssid_size, char* password, size_t password_size)
{
    nvs_handle_t my_handle;
    esp_err_t err;

    err = nvs_open("storage", NVS_READONLY, &my_handle);
    if (err != ESP_OK) return err;

    size_t required_size;
    err = nvs_get_str(my_handle, "wifi_ssid", NULL, &required_size);
    if (err != ESP_OK) {
        nvs_close(my_handle);
        return err;
    }

    if (required_size > ssid_size) {
        nvs_close(my_handle);
        return ESP_ERR_NVS_INVALID_LENGTH;
    }

    err = nvs_get_str(my_handle, "wifi_ssid", ssid, &ssid_size);
    if (err != ESP_OK) {
        nvs_close(my_handle);
        return err;
    }

    err = nvs_get_str(my_handle, "wifi_password", NULL, &required_size);
    if (err != ESP_OK) {
        nvs_close(my_handle);
        return err;
    }

    if (required_size > password_size) {
        nvs_close(my_handle);
        return ESP_ERR_NVS_INVALID_LENGTH;
    }

    err = nvs_get_str(my_handle, "wifi_password", password, &password_size);
    nvs_close(my_handle);
    return err;
}

static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        xTaskCreate(smartconfig_task, "smartconfig_task", 4096, NULL, 3, NULL);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        
        // Save the WiFi credentials
        wifi_config_t wifi_config;
        if (esp_wifi_get_config(WIFI_IF_STA, &wifi_config) == ESP_OK) {
            save_wifi_config((const char*)wifi_config.sta.ssid, (const char*)wifi_config.sta.password);
        }

        esp_err_t ret = webserver_start();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start webserver: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "Webserver started successfully");
        }

        // Stop SmartConfig if it's running
        esp_smartconfig_stop();
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_SCAN_DONE) {
        ESP_LOGI(TAG, "Scan done");
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_FOUND_CHANNEL) {
        ESP_LOGI(TAG, "Found channel");
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_GOT_SSID_PSWD) {
        ESP_LOGI(TAG, "Got SSID and password");

        smartconfig_event_got_ssid_pswd_t *evt = (smartconfig_event_got_ssid_pswd_t *)event_data;
        wifi_config_t wifi_config;
        memset(&wifi_config, 0, sizeof(wifi_config_t));
        memcpy(wifi_config.sta.ssid, evt->ssid, sizeof(wifi_config.sta.ssid));
        memcpy(wifi_config.sta.password, evt->password, sizeof(wifi_config.sta.password));

        ESP_ERROR_CHECK(esp_wifi_disconnect());
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        esp_wifi_connect();
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_SEND_ACK_DONE) {
        xEventGroupSetBits(s_wifi_event_group, ESPTOUCH_DONE_BIT);
    }
}

static void smartconfig_task(void* parm)
{
    EventBits_t uxBits;
    ESP_ERROR_CHECK(esp_smartconfig_set_type(SC_TYPE_ESPTOUCH));
    smartconfig_start_config_t cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
    esp_err_t err = esp_smartconfig_start(&cfg);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "SmartConfig is already running, stopping previous instance");
        esp_smartconfig_stop();
        vTaskDelay(pdMS_TO_TICKS(1000)); // Wait for 1 second
        err = esp_smartconfig_start(&cfg);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start SmartConfig: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    
    while (1) {
        uxBits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | ESPTOUCH_DONE_BIT, true, false, portMAX_DELAY);
        if (uxBits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "Connected to AP");
            esp_smartconfig_stop();
            vTaskDelete(NULL);
            return;
        }
        if (uxBits & ESPTOUCH_DONE_BIT) {
            ESP_LOGI(TAG, "SmartConfig over");
            esp_smartconfig_stop();
            vTaskDelete(NULL);
            return;
        }
    }
}

esp_err_t wifi_manager_init()
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(SC_EVENT, ESP_EVENT_ANY_ID, &event_handler, nullptr, nullptr));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Try to connect using saved credentials
    char ssid[33] = {0};
    char password[65] = {0};
    if (load_wifi_config(ssid, sizeof(ssid), password, sizeof(password)) == ESP_OK) {
        wifi_config_t wifi_config = {0};
        strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
        strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
        
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_connect());
        
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                pdFALSE,
                pdFALSE,
                10000 / portTICK_PERIOD_MS);
        
        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "Connected to AP using saved credentials");
            return ESP_OK;
        }
    }

    ESP_LOGI(TAG, "Failed to connect using saved credentials. Starting SmartConfig.");
    xTaskCreate(smartconfig_task, "smartconfig_task", 4096, NULL, 3, NULL);

    return ESP_OK;
}

esp_err_t wifi_manager_connect_sta(const char* ssid, const char* password)
{
    if (ssid == nullptr || password == nullptr) {
        ESP_LOGE(TAG, "Invalid SSID or password");
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_connect());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP");
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect");
        return ESP_FAIL;
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
        return ESP_FAIL;
    }
}

esp_err_t wifi_manager_disconnect()
{
    return esp_wifi_disconnect();
}

bool wifi_manager_is_connected()
{
    return (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
}

esp_err_t wifi_manager_get_ip_info(char* ip_addr, size_t ip_addr_size)
{
    if (!ip_addr || ip_addr_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_netif_ip_info_t ip_info;
    esp_err_t ret = esp_netif_get_ip_info(s_sta_netif, &ip_info);
    if (ret != ESP_OK) {
        return ret;
    }

    snprintf(ip_addr, ip_addr_size, IPSTR, IP2STR(&ip_info.ip));
    return ESP_OK;
}
