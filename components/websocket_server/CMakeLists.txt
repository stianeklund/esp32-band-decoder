idf_component_register(SRCS "websocket_server.cpp" "websocket_handlers.cpp"
        INCLUDE_DIRS "include"
        REQUIRES relay_controller esp_http_server freertos wifi_manager json esp_wifi esp_event antenna_switch cat_parser config_manager
        PRIV_REQUIRES esp_common)