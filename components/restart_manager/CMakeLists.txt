idf_component_register(SRCS "restart_manager.cpp"
        INCLUDE_DIRS "include"
        REQUIRES config_manager relay_controller wifi_manager nvs_flash esp_timer cat_parser)
