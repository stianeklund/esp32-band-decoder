idf_component_register(SRCS "relay_controller.cpp"
        INCLUDE_DIRS "include"
        REQUIRES kc868_a16_hw config_manager relay_controller wifi_manager my_mqtt_client nvs_flash cat_parser)
