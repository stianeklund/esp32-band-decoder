idf_component_register(SRCS "main.cpp"
        INCLUDE_DIRS "."
        REQUIRES serial_cli relay_controller wifi_manager restart_manager system_initializer input_manager websocket_server config_manager kc868_a16_hw
)
