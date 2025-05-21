idf_component_register(SRCS "system_initializer.cpp"
        INCLUDE_DIRS "include"
        REQUIRES input_manager config_manager relay_controller wifi_manager nvs_flash cat_parser)
