idf_component_register(SRCS "main.cpp"
        INCLUDE_DIRS "."
        REQUIRES relay_controller wifi_manager restart_manager system_initializer
)