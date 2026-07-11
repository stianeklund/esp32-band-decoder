idf_component_register(SRCS "config_manager.cpp" "config_cache.cpp"
        INCLUDE_DIRS "include"
        REQUIRES antenna_switch nvs_flash
        PRIV_REQUIRES esp_driver_uart esp_driver_gpio)
