idf_component_register(SRCS "antenna_switch.cpp"
        INCLUDE_DIRS "include"
        REQUIRES relay_controller config_manager driver nvs_flash)
