idf_component_register(SRCS "serial_cli.cpp"
                    INCLUDE_DIRS "include"
                    REQUIRES wifi_manager # For WifiManager::instance()
                             # esp_hw_support is usually a transitive dependency from esp_system or similar
                             # nvs_flash might be needed if CLI directly interacts with NVS, but here it's via WifiManager
                    PRIV_REQUIRES esp_common # For esp_err_to_name, esp_log etc.
                                  freertos # For FreeRTOS API
                                  esp_system # For esp_restart
                    )
