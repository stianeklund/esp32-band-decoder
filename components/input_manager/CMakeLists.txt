idf_component_register(SRCS "input_manager.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES kc868_hw esp_common log config_manager antenna_switch)
