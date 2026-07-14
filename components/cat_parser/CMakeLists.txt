idf_component_register(SRCS "cat_parser.cpp" "kenwood_cat.cpp" "yaesu_cat.cpp" "cat_parser_factory.cpp"
        INCLUDE_DIRS "include"
        REQUIRES antenna_switch freertos esp_driver_uart esp_driver_gpio esp_timer my_mqtt_client websocket_server)
