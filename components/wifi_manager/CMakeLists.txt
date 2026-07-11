idf_component_register(SRCS "wifi_manager.cpp"
        INCLUDE_DIRS "include"
        REQUIRES web_server esp_wifi nvs_flash esp_http_server driver esp_netif esp_timer)
