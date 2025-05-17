idf_component_register(SRCS "system_initializer.cpp"
        INCLUDE_DIRS "include"
        REQUIRES config_manager relay_controller wifi_manager nvs_flash cat_parser)
