idf_component_register(SRCS "config_manager.cpp"
        INCLUDE_DIRS "include"
        REQUIRES antenna_switch config_manager relay_controller wifi_manager nvs_flash cat_parser)
