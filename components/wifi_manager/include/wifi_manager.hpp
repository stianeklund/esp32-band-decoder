#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_netif.h"


#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define ESPTOUCH_DONE_BIT  BIT2 // Also used as SMARTCONFIG_DONE_BIT in event_handler
#define MANUAL_DISCONNECT_ACK_BIT BIT3

class WifiManager {
public:
    static WifiManager& instance();
    
    // Core functionality
    esp_err_t init();
    [[nodiscard]] bool is_connected() const { return m_wifi_connected; }
    [[nodiscard]] bool is_in_smartconfig_mode() const { return !m_using_saved_credentials; }
    esp_err_t get_ip_info(char* ip_addr, size_t ip_addr_size);
    esp_err_t get_mac_address(char* mac_addr, size_t mac_addr_size);
    esp_err_t wait_for_connection(uint32_t timeout_ms);
    esp_err_t connect_sta(const char* ssid, const char* password);
    esp_err_t disconnect();
    esp_err_t clear_credentials();
    

    esp_err_t start_smartconfig(); // Public for recovery mode
    esp_err_t save_wifi_config(const char* ssid, const char* password) {
        return save_credentials(ssid, password);
    }
    esp_err_t load_wifi_config(char* ssid, size_t ssid_size,
                              char* password, size_t password_size) {
        return load_credentials(ssid, ssid_size, password, password_size);
    }

    /**
     * @brief Sets new Wi-Fi credentials, saves them to NVS, and attempts to reconnect.
     *
     * @param ssid The new SSID.
     * @param password The new password.
     * @return ESP_OK on success, or an error code if saving or setting config fails.
     */
    esp_err_t set_and_apply_credentials(const char* ssid, const char* password);


private:
    WifiManager() = default;
    ~WifiManager();
    
    // Delete copy and move
    WifiManager(const WifiManager&) = delete;
    WifiManager& operator=(const WifiManager&) = delete;
    WifiManager(WifiManager&&) = delete;
    WifiManager& operator=(WifiManager&&) = delete;

    // Core internal methods
    static void event_handler(void* arg, esp_event_base_t event_base, 
                            int32_t event_id, void* event_data);
    esp_err_t try_connect_with_saved_credentials();
    static void smartconfig_task(void* parm);
    
    // Credential management
    static esp_err_t save_credentials(const char* ssid, const char* password);
    esp_err_t load_credentials(char* ssid, size_t ssid_size, 
                             char* password, size_t password_size);

    // Member variables
    EventGroupHandle_t m_wifi_event_group{nullptr};
    esp_netif_t* m_sta_netif{nullptr};
    bool m_wifi_connected{false};
    bool m_ip_obtained{false};
    bool m_using_saved_credentials{false};
    // New state machine for handling reconfiguration
    enum class ReconfigState {
        NONE,                           // Normal operation
        AWAITING_INITIAL_DISCONNECT,    // Called esp_wifi_disconnect(), waiting for the event
        AWAITING_NEW_CONNECTION         // Set new config, called esp_wifi_connect(), waiting for GOT_IP or DISCONNECT
    };
    ReconfigState m_reconfig_state{ReconfigState::NONE};
    static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 10000;  // 10 seconds
    static constexpr uint32_t SMARTCONFIG_TIMEOUT_MS = 120000;  // 2 minutes
};
