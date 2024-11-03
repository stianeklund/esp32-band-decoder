#include "wifi_manager.hpp"
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
#include <esp_task_wdt.h>

// Add ESP_RETURN_ON_ERROR macro if not already defined
#ifndef ESP_RETURN_ON_ERROR
#define ESP_RETURN_ON_ERROR(x, tag, msg) do {                                \
        esp_err_t __err_rc = (x);                                           \
        if (__err_rc != ESP_OK) {                                           \
            ESP_LOGE(tag, "%s(%d): %s", msg, __err_rc,                     \
                    esp_err_to_name(__err_rc));                             \
            return __err_rc;                                                \
        }                                                                   \
    } while(0)
#endif

static auto TAG = "WIFI_MANAGER";

WifiManager &WifiManager::instance() {
    static WifiManager instance;
    return instance;
}

WifiManager::~WifiManager() {
    if (m_wifi_event_group) {
        vEventGroupDelete(m_wifi_event_group);
    }
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();
    if (m_sta_netif) {
        esp_netif_destroy_default_wifi(m_sta_netif);
    }
}

esp_err_t WifiManager::save_credentials(const char *ssid, const char *password) {
    nvs_handle_t my_handle;

    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
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

esp_err_t WifiManager::load_credentials(char *ssid, size_t ssid_size, char *password, size_t password_size) {
    nvs_handle_t my_handle;

    esp_err_t err = nvs_open("storage", NVS_READONLY, &my_handle);
    if (err != ESP_OK) return err;

    size_t required_size;
    err = nvs_get_str(my_handle, "wifi_ssid", nullptr, &required_size);
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

    err = nvs_get_str(my_handle, "wifi_password", nullptr, &required_size);
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

void WifiManager::event_handler(void *arg, const esp_event_base_t event_base,
                                const int32_t event_id, void *event_data) {
    auto &instance = WifiManager::instance();

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        // Only start SmartConfig if we're not using saved credentials
        if (!instance.m_using_saved_credentials) {
            xTaskCreate(&WifiManager::smartconfig_task, "smartconfig_task", 4096, nullptr, 3, nullptr);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        instance.m_using_saved_credentials = false;
        instance.m_wifi_connected = false;
        instance.m_ip_obtained = false;
        esp_wifi_connect();
        xEventGroupClearBits(instance.m_wifi_event_group, WIFI_CONNECTED_BIT);
        xEventGroupSetBits(instance.m_wifi_event_group, WIFI_FAIL_BIT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        instance.m_wifi_connected = true;
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        instance.m_ip_obtained = true;
        instance.m_wifi_connected = true; // Make sure this is set
        xEventGroupClearBits(instance.m_wifi_event_group, WIFI_FAIL_BIT);
        xEventGroupSetBits(instance.m_wifi_event_group, WIFI_CONNECTED_BIT);

        ESP_LOGI(TAG, "WiFi Connected");

        // Stop SmartConfig if it's running
        esp_smartconfig_stop();

        // Only set this flag if we actually used SmartConfig
        if (!instance.m_using_saved_credentials) {
            instance.m_using_saved_credentials = true;
        }

        // Add a small delay to ensure connection status propagates
        vTaskDelay(pdMS_TO_TICKS(100));

        if (const esp_err_t ret = webserver_start(); ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start webserver: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "Webserver started successfully");
        }
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_SCAN_DONE) {
        ESP_LOGI(TAG, "Scan done");
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_FOUND_CHANNEL) {
        ESP_LOGI(TAG, "Found channel");
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_GOT_SSID_PSWD) {
        ESP_LOGI(TAG, "Got SSID and password");

        const auto *evt = static_cast<smartconfig_event_got_ssid_pswd_t *>(event_data);

        wifi_config_t wifi_config = {};
        memcpy(wifi_config.sta.ssid, evt->ssid, sizeof(wifi_config.sta.ssid));
        memcpy(wifi_config.sta.password, evt->password, sizeof(wifi_config.sta.password));

        // Save the credentials and verify
        const esp_err_t save_err = instance.save_wifi_config(reinterpret_cast<const char *>(wifi_config.sta.ssid),
                                                             reinterpret_cast<const char *>(wifi_config.sta.password));
        if (save_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save WiFi credentials: %s", esp_err_to_name(save_err));
        } else {
            // Verify saved credentials
            char verify_ssid[33] = {};
            if (char verify_pass[65] = {}; instance.load_wifi_config(verify_ssid, sizeof(verify_ssid), verify_pass,
                                                                      sizeof(verify_pass)) == ESP_OK) {
                if (strcmp(verify_ssid, reinterpret_cast<char *>(wifi_config.sta.ssid)) == 0) {
                    ESP_LOGI(TAG, "WiFi credentials saved and verified successfully");
                }
            }
        }

        ESP_ERROR_CHECK(esp_wifi_disconnect());
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_connect());
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_SEND_ACK_DONE) {
        xEventGroupSetBits(instance.m_wifi_event_group, ESPTOUCH_DONE_BIT);
    }
}

// ReSharper disable once CppParameterNeverUsed (it's used by the task callback)
void WifiManager::smartconfig_task(void *parm) {
    const auto &instance = WifiManager::instance();

    if (esp_task_wdt_status(xTaskGetCurrentTaskHandle()) == ESP_OK) {
        // Add watchdog feed
        esp_task_wdt_reset();
    }
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Check if we're already connected
    if (xEventGroupGetBits(instance.m_wifi_event_group) & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Already connected, stopping SmartConfig task");
        vTaskDelete(nullptr);
        return;
    }

    // Stop any existing SmartConfig session
    esp_smartconfig_stop();
    vTaskDelay(pdMS_TO_TICKS(100));

    // Initialize SmartConfig with error handling
    esp_err_t err = esp_smartconfig_set_type(SC_TYPE_ESPTOUCH);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set SmartConfig type: %s", esp_err_to_name(err));
        vTaskDelete(nullptr);
        return;
    }

    constexpr smartconfig_start_config_t cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
    err = esp_smartconfig_start(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start SmartConfig: %s", esp_err_to_name(err));
        vTaskDelete(nullptr);
        return;
    }

    while (true) {
        // Feed watchdog periodically
        if (esp_task_wdt_status(xTaskGetCurrentTaskHandle()) == ESP_OK) {
            // Add watchdog feed
            esp_task_wdt_reset();
        }

        const EventBits_t uxBits = xEventGroupWaitBits(instance.m_wifi_event_group,
                                                      WIFI_CONNECTED_BIT | ESPTOUCH_DONE_BIT,
                                                      true, false, 
                                                      pdMS_TO_TICKS(100)); // Reduced timeout

        if (uxBits & (WIFI_CONNECTED_BIT | ESPTOUCH_DONE_BIT)) {
            ESP_LOGI(TAG, "SmartConfig task complete");
            esp_smartconfig_stop();
            break;
        }
    }

    esp_smartconfig_stop();
    vTaskDelete(nullptr);
}

esp_err_t WifiManager::init() {
    ESP_LOGI(TAG, "Initializing WiFi manager in STA mode");

    // Delete any existing event group first
    if (m_wifi_event_group) {
        vEventGroupDelete(m_wifi_event_group);
        m_wifi_event_group = nullptr;
    }

    // Create event group
    m_wifi_event_group = xEventGroupCreate();
    if (!m_wifi_event_group) {
        ESP_LOGE(TAG, "Failed to create event group");
        return ESP_ERR_NO_MEM;
    }

    // Initialize TCP/IP adapter with more robust error handling
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TCP/IP adapter initialization failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Add delay after TCP/IP init
    vTaskDelay(pdMS_TO_TICKS(100));

    // Create the default event loop if it doesn't exist
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to create event loop: %s", esp_err_to_name(ret));
        return ret;
    }

    // Add delay after event loop creation
    vTaskDelay(pdMS_TO_TICKS(100));

    // Create default WiFi sta
    m_sta_netif = esp_netif_create_default_wifi_sta();
    if (!m_sta_netif) {
        ESP_LOGE(TAG, "Failed to create WiFi STA interface");
        return ESP_FAIL;
    }

    // Add delay after netif creation
    vTaskDelay(pdMS_TO_TICKS(100));

    // Initialize WiFi with default config
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    // Increase buffer numbers for stability
    cfg.static_rx_buf_num = 16;
    cfg.static_tx_buf_num = 16;

    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi initialization failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Add delay after WiFi init
    vTaskDelay(pdMS_TO_TICKS(100));

    // Set WiFi storage to flash
    ret = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi storage: %s", esp_err_to_name(ret));
        return ret;
    }

    // Set mode to station
    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi mode: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register event handlers with error checking
    ret = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            &event_handler, nullptr, nullptr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WiFi event handler: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            &event_handler, nullptr, nullptr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IP event handler: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_handler_instance_register(SC_EVENT, ESP_EVENT_ANY_ID,
                                            &event_handler, nullptr, nullptr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register SmartConfig event handler: %s", esp_err_to_name(ret));
        return ret;
    }

    // Add delay before starting WiFi
    vTaskDelay(pdMS_TO_TICKS(100));

    // Start WiFi
    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WiFi: %s", esp_err_to_name(ret));
        return ret;
    }

    // Add delay after starting WiFi
    vTaskDelay(pdMS_TO_TICKS(200));

    #ifdef FORCE_SMARTCONFIG
        ESP_LOGI(TAG, "Forcing SmartConfig mode...");
        return start_smartconfig();
    #else
        // Try connecting with saved credentials
        ret = try_connect_with_saved_credentials();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Could not connect with saved credentials: %s", esp_err_to_name(ret));
            ESP_LOGI(TAG, "Starting SmartConfig...");
            return start_smartconfig();
        }
        return ESP_OK;
    #endif
}

esp_err_t WifiManager::wait_for_connection(uint32_t timeout_ms) {
    if (!m_wifi_event_group) {
        return ESP_ERR_INVALID_STATE;
    }

    const EventBits_t bits = xEventGroupWaitBits(m_wifi_event_group,
                                                 WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                                 pdFALSE,
                                                 pdFALSE,
                                                 pdMS_TO_TICKS(timeout_ms));

    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }

    if (bits & WIFI_FAIL_BIT) {
        return ESP_FAIL;
    }

    return ESP_ERR_TIMEOUT;
}

esp_err_t WifiManager::get_ip_info(char *ip_addr, size_t ip_addr_size) {
    if (!ip_addr || ip_addr_size < 16) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_netif_ip_info_t ip_info;
    esp_err_t ret = esp_netif_get_ip_info(m_sta_netif, &ip_info);
    if (ret != ESP_OK) {
        return ret;
    }

    snprintf(ip_addr, ip_addr_size, IPSTR, IP2STR(&ip_info.ip));
    return ESP_OK;
}

esp_err_t WifiManager::connect_sta(const char *ssid, const char *password) {
    if (!ssid || !password) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t wifi_config = {};
    strncpy(reinterpret_cast<char *>(wifi_config.sta.ssid), ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy(reinterpret_cast<char *>(wifi_config.sta.password), password, sizeof(wifi_config.sta.password) - 1);

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config),
                        TAG, "Failed to set WiFi configuration");
    return esp_wifi_connect();
}

esp_err_t WifiManager::disconnect() {
    return esp_wifi_disconnect();
}

esp_err_t WifiManager::get_mac_address(char *mac_addr, size_t mac_addr_size) {
    if (!mac_addr || mac_addr_size < 18) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t mac[6];
    if (const esp_err_t ret = esp_wifi_get_mac(WIFI_IF_STA, mac); ret != ESP_OK) {
        return ret;
    }

    snprintf(mac_addr, mac_addr_size, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return ESP_OK;
}

esp_err_t WifiManager::try_connect_with_saved_credentials() {
    char ssid[33] = {};
    char password[65] = {};

    const esp_err_t err = load_credentials(ssid, sizeof(ssid), password, sizeof(password));

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved credentials found, will start SmartConfig");
        return err;
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error loading credentials: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Found saved credentials, attempting to connect");
    m_using_saved_credentials = true;
    return connect_sta(ssid, password);
}

esp_err_t WifiManager::start_smartconfig() {
    m_using_saved_credentials = false;

    // SmartConfig will be started by the event handler when 
    // WIFI_EVENT_STA_START is received
    return ESP_OK;
}

esp_err_t WifiManager::clear_credentials() {
    nvs_handle_t my_handle;

    // Open NVS
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err != ESP_OK) return err;

    // Erase WiFi credentials
    err = nvs_erase_key(my_handle, "wifi_ssid");
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(my_handle);
        return err;
    }

    err = nvs_erase_key(my_handle, "wifi_password");
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(my_handle);
        return err;
    }

    // Commit changes
    err = nvs_commit(my_handle);
    nvs_close(my_handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "WiFi credentials cleared successfully");
    }

    return err;
}
