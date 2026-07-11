idf_component_register(SRCS "webserver.cpp" "html_content.cpp"
        INCLUDE_DIRS "include"
        REQUIRES relay_controller esp_http_server freertos wifi_manager esp_wifi esp_event antenna_switch cat_parser
        PRIV_REQUIRES esp_common)
