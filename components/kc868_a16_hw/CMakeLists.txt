idf_component_register(SRCS "kc868_a16_hw.cpp"
        INCLUDE_DIRS "include"
        REQUIRES config_manager relay_controller wifi_manager nvs_flash cat_parser)
