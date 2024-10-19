idf_component_register(SRCS "html_content.cpp" "cat_parser.cpp" "webserver.cpp" "wifi_manager.cpp" "tcp_client.cpp" "relay_controller.cpp" "antenna_switch.cpp" "main.cpp" "config_manager.cpp" "restart_manager.cpp" "system_initializer.cpp"
        INCLUDE_DIRS "."
     REQUIRES freertos esp_wifi nvs_flash esp_http_server driver esp_netif esp_timer json
)

