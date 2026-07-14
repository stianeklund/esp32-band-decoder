#include "include/cat_parser.h"
#include "websocket_server.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "my_mqtt_client.h"
#include <string>
#include <string_view>
#include "driver/gpio.h"
#include <chrono>
#include "config_manager.h"
#include "antenna_switch.h"

CatParser::CatParser()
    : uart2_queue(nullptr),
      shutdown_requested(false),
      last_serial_data_time(std::chrono::steady_clock::now() - std::chrono::seconds(SERIAL_DATA_TIMEOUT_S + 1)),
      last_valid_command_time(std::chrono::steady_clock::now() - std::chrono::seconds(SERIAL_DATA_TIMEOUT_S + 1)),
      last_ai_query_time(std::chrono::steady_clock::now() - std::chrono::seconds(AI_QUERY_INTERVAL_S + 1)) {

    // Command handlers are populated by the concrete protocol subclass (KenwoodCat).
}

CatParser::~CatParser() {
    // Request shutdown of UART tasks
    shutdown_requested.store(true);

    // Give tasks time to shut down gracefully
    vTaskDelay(pdMS_TO_TICKS(200));

    // Cleanup UART resources
    if (uart2_queue != nullptr) {
        uart_driver_delete(UART_NUM_2);
        uart2_queue = nullptr;
    }
}

// CatParser::instance() is defined in cat_parser_factory.cpp (runtime protocol selection).


#define UART_TASK_STACK_SIZE 8192
#define UART_QUEUE_SIZE 3

esp_err_t CatParser::init() {
    ESP_LOGD(TAG, "Initializing CAT parser");

    ConfigManager::instance().get_config(current_config);

    // Validate baud rate and set default if invalid
    if (current_config.uart_baud_rate <= 0) {
        ESP_LOGW(TAG, "Invalid baud rate %d, using defaults", current_config.uart_baud_rate);
        
        // Instead of modifying the entire config and risking corruption of other fields,
        // just use a default baud rate for initialization without saving it back
        // This avoids the ESP_ERR_INVALID_ARG error when num_bands might be 0
        ESP_LOGI(TAG, "Using default baud rate 9600 for UART initialization");
        current_config.uart_baud_rate = 9600;
    }

    // Add delay to ensure peripheral initialization is complete
    vTaskDelay(pdMS_TO_TICKS(100));

    // Reset UART state
    uart2_queue = nullptr;

    // Start with very basic UART2 configuration using validated baud rate
    uart_config_t uart2_config = {
        .baud_rate = current_config.uart_baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };

    ESP_LOGV(TAG, "Starting basic UART2 configuration");

    // Validate UART pins and set defaults if needed
    bool pins_config_updated = false;
    if (current_config.uart_tx_pin != -1 && (current_config.uart_tx_pin < 0 || current_config.uart_tx_pin >= GPIO_NUM_MAX)) {
        ESP_LOGW(TAG, "Invalid UART TX pin %d configured, defaulting to GPIO 17.", current_config.uart_tx_pin);
        current_config.uart_tx_pin = 17; 
        pins_config_updated = true;
    }

    if (current_config.uart_rx_pin < 0 || current_config.uart_rx_pin >= GPIO_NUM_MAX) {
        ESP_LOGW(TAG, "Invalid UART RX pin %d configured, defaulting to GPIO 16.", current_config.uart_rx_pin);
        current_config.uart_rx_pin = 16; 
        pins_config_updated = true;
    }

    if (pins_config_updated) {
        esp_err_t save_ret = AntennaSwitch::instance().set_config(&current_config);
        if (save_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save updated UART pin configuration: %s", esp_err_to_name(save_ret));
            // Depending on severity, might return ret here. For now, proceed with corrected pins.
        }
        // Re-fetch if updated
        ConfigManager::instance().get_config(current_config);
    }

    // Configure UART2 with minimal settings
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_2, &uart2_config));
    vTaskDelay(pdMS_TO_TICKS(10));

    // Set configured pins before driver installation
    // current_config.uart_tx_pin can be -1 (UART_PIN_NO_CHANGE)
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_2,
                                (gpio_num_t)current_config.uart_tx_pin,
                                (gpio_num_t)current_config.uart_rx_pin,
                                UART_PIN_NO_CHANGE,
                                UART_PIN_NO_CHANGE)); // CTS not used

    // Disable internal pullups since external ones are present on KC868 for RX
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT,
    io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE,
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pin_bit_mask = (1ULL << current_config.uart_rx_pin);
    // io_conf.pin_bit_mask = (1ULL << current_config.uart_tx_pin);
    ESP_ERROR_CHECK(gpio_config(&io_conf));


    ESP_LOGI(TAG, "Configuring UART2 with RX on GPIO%d, TX on GPIO%d, baud=%d", current_config.uart_rx_pin,
             current_config.uart_tx_pin, current_config.uart_baud_rate);
    vTaskDelay(pdMS_TO_TICKS(50));

    // If basic configuration succeeds, try updating to desired settings
    uart2_config.baud_rate = current_config.uart_baud_rate;
    uart2_config.parity = static_cast<uart_parity_t>(current_config.uart_parity);
    uart2_config.stop_bits = static_cast<uart_stop_bits_t>(current_config.uart_stop_bits);
    uart2_config.flow_ctrl = static_cast<uart_hw_flowcontrol_t>(current_config.uart_flow_ctrl);

    ESP_LOGD(TAG, "Updating UART2 configuration: baud=%d, parity=%d, stop_bits=%d, flow_ctrl=%d",
             uart2_config.baud_rate, uart2_config.parity, uart2_config.stop_bits, uart2_config.flow_ctrl);

    // Update configuration
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_2, &uart2_config));
    vTaskDelay(pdMS_TO_TICKS(50));

    // Create event queue
    QueueHandle_t event_queue;
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_2, BUF_SIZE * 2, BUF_SIZE * 2, UART_QUEUE_SIZE, &event_queue, 0));
    uart2_queue = event_queue;

    ESP_LOGV(TAG, "UART2 configuration complete");

    // Create UART task with minimal priority
    const BaseType_t xReturned = xTaskCreate(
        uart_task_trampoline,
        "cat_parser_uart_task",
        UART_TASK_STACK_SIZE,
        this,
        configMAX_PRIORITIES / 2, // Increased priority
        nullptr
    );

    if (xReturned != pdPASS) {
        ESP_LOGE(TAG, "Failed to create UART task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Initialization complete");
    return ESP_OK;
}

// ReSharper disable once CppDFAUnreachableFunctionCall
void CatParser::uart_task() {
    uart_event_t event;
    size_t buffered_size;
    uint8_t read_buf[128];
    constexpr TickType_t xTicksToWait = pdMS_TO_TICKS(10); // Timeout for xQueueReceive
    std::string command_accumulator;
    command_accumulator.reserve(256); // Pre-reserve to reduce reallocations
    constexpr size_t MAX_COMMAND_ACCUMULATOR_SIZE = 1024; // Max size for the command accumulator buffer
    UBaseType_t stack_high_water_mark = uxTaskGetStackHighWaterMark(nullptr);

    ESP_LOGI(TAG, "CAT parser UART task started (stack high-water mark: %u bytes).",
             static_cast<unsigned>(stack_high_water_mark));

    while (!shutdown_requested.load()) {
        if (xQueueReceive(uart2_queue, &event, xTicksToWait) == pdTRUE) {
            MQTTClient::instance().set_has_serial_data(true);
            last_serial_data_time = std::chrono::steady_clock::now();

            switch (event.type) {
                case UART_DATA: {
                    if (uart_get_buffered_data_len(UART_NUM_2, &buffered_size) == ESP_OK && buffered_size > 0) {
                        const int len = uart_read_bytes(UART_NUM_2, read_buf,
                                                        std::min(buffered_size, sizeof(read_buf)),
                                                        0); 

                        if (len > 0) {
                            // Check if appending would exceed a max size
                            if (command_accumulator.length() + static_cast<size_t>(len) > MAX_COMMAND_ACCUMULATOR_SIZE) {
                                ESP_LOGW(TAG, "UART command accumulator (len %zu) + new data (len %d) would exceed max size (%zu). Clearing accumulator.",
                                         command_accumulator.length(), len, MAX_COMMAND_ACCUMULATOR_SIZE);
                                command_accumulator.clear();
                                // If the new chunk itself is too large, discard it. Otherwise, start fresh with it.
                                if (static_cast<size_t>(len) <= MAX_COMMAND_ACCUMULATOR_SIZE) {
                                    command_accumulator.append(reinterpret_cast<char*>(read_buf), len);
                                } else {
                                    ESP_LOGW(TAG, "Incoming UART data chunk itself (%d bytes) exceeds max accumulator size (%zu). Discarding.", len, MAX_COMMAND_ACCUMULATOR_SIZE);
                                }
                            } else {
                                command_accumulator.append(reinterpret_cast<char*>(read_buf), len);
                            }

                            // Process complete commands from the accumulator
                            // A command sequence is considered complete if it ends with a ';'
                            const size_t last_semicolon_in_accumulator = command_accumulator.rfind(';');

                            if (last_semicolon_in_accumulator != std::string::npos) {
                                // Extract the part of the accumulator that contains complete commands
                                const size_t process_len = last_semicolon_in_accumulator + 1;
                                const std::string_view commands_to_process_view(command_accumulator.data(), process_len);
                                
                                ESP_LOGV(TAG, "Processing from UART accumulator: %.*s", static_cast<int>(commands_to_process_view.length()), commands_to_process_view.data());
                                
                                // Call the centralized process_command
                                 if (const esp_err_t ret = process_command(commands_to_process_view); ret != ESP_OK) {
                                      ESP_LOGW(TAG, "Error processing command block from UART: %s", esp_err_to_name(ret));
                                 }

                                 const UBaseType_t current_high_water_mark = uxTaskGetStackHighWaterMark(nullptr);
                                 if (current_high_water_mark < stack_high_water_mark) {
                                     stack_high_water_mark = current_high_water_mark;
                                     ESP_LOGI(TAG, "CAT parser UART stack high-water mark: %u bytes",
                                              static_cast<unsigned>(stack_high_water_mark));
                                 }

                                // Erase processed commands from the accumulator
                                command_accumulator.erase(0, process_len);
                            }
                        }
                    }
                    break;
                }

                case UART_FIFO_OVF:
                case UART_BUFFER_FULL:
                    ESP_LOGW(TAG, "UART buffer issue detected (type: %d), flushing UART and resetting queue.", event.type);
                    uart_flush_input(UART_NUM_2);
                    xQueueReset(uart2_queue);
                    command_accumulator.clear(); 
                    break;
                case UART_PARITY_ERR:
                    ESP_LOGW(TAG, "UART parity error");
                    break;
                case UART_FRAME_ERR:
                    ESP_LOGW(TAG, "UART frame error");
                    break;
                default:
                    break;
            }
        } else {
            // xQueueReceive timed out
            auto now = std::chrono::steady_clock::now();

            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_serial_data_time).count() > SERIAL_DATA_TIMEOUT_S) {
                MQTTClient::instance().set_has_serial_data(false);
            }

            // Protocol-specific auto-info handshake / polling (e.g. Kenwood AI query).
            poll_for_updates();
        }
        taskYIELD();
    }
    ESP_LOGI(TAG, "CAT parser UART task shutting down.");
}

void CatParser::uart0_to_uart2_task()
{
}


esp_err_t CatParser::update_config() {
    ESP_LOGD(TAG, "Updating CAT parser configuration");

    ConfigManager::instance().get_config(current_config);

    ESP_LOGD(TAG, "CAT parser configuration updated successfully");
    return ESP_OK;
}

esp_err_t CatParser::handle_frequency_change(const uint32_t frequency) {
    if (frequency == current_frequency) {
        return ESP_OK;
    }

    const int new_band_index = get_band_index(frequency);

    // While transverter mode is active the reported frequency is the IF (e.g. 28 MHz),
    // but the real on-air band (e.g. 2 m) is unsupported by the antenna switch. Keep the
    // antenna ports off and simply track the IF frequency for display/restore purposes.
    if (transverter_active.load()) {
        ESP_LOGV(TAG, "Transverter active; skipping antenna switch for IF %lu Hz", frequency);
        current_frequency = frequency;
        current_band_index = new_band_index;
        return ESP_OK;
    }

    // Act only when the band actually changes.
    if (current_band_index != new_band_index) {
        if (new_band_index < 0) {
            // Frequency is not in any configured band (e.g. 6 m / 50 MHz not set up).
            // We cannot select an antenna, so turn off Radio A's selected port but
            // still track/display the frequency. This is not an error.
            ESP_LOGI(TAG, "Frequency %lu Hz not in any configured band; turning off Radio A port", frequency);
            const int active_relay = AntennaSwitch::instance().get_active_relay_for_radio(RadioID::A);
            if (active_relay != 0) {
                if (const esp_err_t ret = AntennaSwitch::instance().set_relay(active_relay, false); ret != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to turn off Radio A relay %d: %s", active_relay, esp_err_to_name(ret));
                }
            }
        } else {
            ESP_LOGV(TAG, "Frequency requires band change, setting new antenna");
            if (const esp_err_t ret = AntennaSwitch::instance().set_frequency(frequency); ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to set frequency %lu Hz: %s", frequency, esp_err_to_name(ret));
                return ret;
            }
        }
    } else {
        ESP_LOGV(TAG, "Frequency is in the same band, skipping antenna switch");
    }

    // Update cached values
    current_frequency = frequency;
    current_band_index = new_band_index;

    return ESP_OK;
}


void CatParser::handle_frequency_update(const uint32_t frequency) {
    ESP_LOGD(TAG, "Handling frequency update: %lu Hz", frequency);
    handle_frequency_change(frequency);
}

esp_err_t CatParser::process_command(const char *command_cstr) {
    if (!command_cstr) {
        return ESP_ERR_INVALID_ARG;
    }
    return process_command(std::string_view(command_cstr));
}

esp_err_t CatParser::process_command(std::string_view commands_str_with_semicolons) {
    size_t start_pos = 0;
    esp_err_t first_error = ESP_OK;

    while (start_pos < commands_str_with_semicolons.length()) {
        size_t end_pos = commands_str_with_semicolons.find(';', start_pos);
        std::string_view command_content;

        if (end_pos == std::string_view::npos) {
            command_content = commands_str_with_semicolons.substr(start_pos);
            start_pos = commands_str_with_semicolons.length(); // Mark as consumed
        } else {
            command_content = commands_str_with_semicolons.substr(start_pos, end_pos - start_pos);
            start_pos = end_pos + 1;
        }

        // Only dispatch if there's actual content
        if (!command_content.empty()) {
            ESP_LOGV(TAG, "Dispatching from process_command: %.*s", static_cast<int>(command_content.length()), command_content.data());
            // dispatch_one_command expects the command *without* the semicolon.
            esp_err_t dispatch_ret = dispatch_one_command(command_content);
            if (dispatch_ret != ESP_OK) {
                ESP_LOGW(TAG, "Error dispatching command '%.*s': %s",
                         static_cast<int>(command_content.length()), command_content.data(),
                         esp_err_to_name(dispatch_ret));
                if (first_error == ESP_OK) {
                    first_error = dispatch_ret;
                }
                // Continue processing remaining commands in the string
            }
        }
        // If command_content is empty (e.g., from ";;" or leading/trailing ";"), skip to next segment.
    }
    return first_error; // Return ESP_OK if all succeeded, or the first error encountered
}

int CatParser::get_band_index(const uint32_t freq) const {
    // Use cached band index if the frequency is current
    if (freq == current_frequency && current_band_index != -1) {
        return current_band_index;
    }

    // Find which band the frequency belongs to
    for (int i = 0; i < current_config.num_bands; i++) {
        if (freq >= current_config.bands[0][i].start_freq &&
            freq <= current_config.bands[0][i].end_freq) {
            return i;
        }
    }
    return -1;
}

bool CatParser::is_same_band(const uint32_t freq1, const uint32_t freq2) const {
    const int band1 = get_band_index(freq1);
    const int band2 = get_band_index(freq2);
    return band1 != -1 && band1 == band2;
}


void CatParser::set_transverter_active(const bool active) {
    if (transverter_active.load() == active) {
        return; // not a transition
    }
    transverter_active.store(active);
    ESP_LOGI(TAG, "Transverter mode %s", active ? "ENABLED" : "DISABLED");

    if (active) {
        // The ARCI XVTR macro does not push XO, but the radio stores the offset
        // and answers a query. Read it for the transverter WebSocket event / info
        // (the radio itself already applies the offset to its reported frequency).
        send_to_radio("XO;");

        // Turn off Radio A's currently selected antenna port: the switch cannot
        // handle the transverter (e.g. 2 m) band anyway.
        const int active_relay = AntennaSwitch::instance().get_active_relay_for_radio(RadioID::A);
        if (active_relay != 0) {
            ESP_LOGI(TAG, "Transverter active: turning off Radio A relay %d", active_relay);
            if (const esp_err_t ret = AntennaSwitch::instance().set_relay(active_relay, false); ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to turn off Radio A relay %d: %s", active_relay, esp_err_to_name(ret));
            }
        }
    } else if (current_frequency != 0) {
        // Leaving transverter mode: re-evaluate the antenna for the true IF
        // frequency so the correct port is restored (call the switch directly to
        // bypass the same-frequency early-return in handle_frequency_change).
        if (const esp_err_t ret = AntennaSwitch::instance().set_frequency(current_frequency); ret != ESP_OK) {
            ESP_LOGD(TAG, "Antenna restore after transverter off returned: %s", esp_err_to_name(ret));
        }
    }

    broadcast_transverter_state();
}

uint32_t CatParser::transverter_rf_from_if(const uint32_t if_freq) const {
    // The radio reports the IF frequency (e.g. 28.174 MHz) over CAT; the on-air
    // (transverter) frequency is IF + offset (plus direction) or IF - offset (minus).
    const int32_t offset = transverter_offset_hz.load();
    if (transverter_minus_dir.load()) {
        if (static_cast<int64_t>(if_freq) - offset < 0) {
            return 0;
        }
        return if_freq - static_cast<uint32_t>(offset);
    }
    return if_freq + static_cast<uint32_t>(offset);
}

void CatParser::broadcast_transverter_state() {
    if (!websocket_server_is_running()) {
        return;
    }
    // The event always reports the true on-air frequency (IF + offset) so an external
    // consumer can drive the physical transverter, independent of the display setting.
    const uint32_t if_freq = current_frequency;
    const uint32_t rf_freq = transverter_active.load() ? transverter_rf_from_if(if_freq) : if_freq;
    WebSocketServer::instance().broadcast_transverter_state_change(
        transverter_active.load(),
        transverter_offset_hz.load(),
        if_freq,
        rf_freq);
}

uint32_t CatParser::get_display_frequency() const {
    // The radio reports the IF frequency (e.g. 28.174 MHz) over CAT even in
    // transverter mode. Show the corrected on-air frequency (IF +/- offset) only
    // when transverter mode is active AND the user has enabled transverter frequency
    // display; otherwise report the raw IF frequency.
    if (!transverter_active.load() || !current_config.transverter_show_frequency) {
        return current_frequency;
    }
    return transverter_rf_from_if(current_frequency);
}

void CatParser::set_transmitting(const bool new_state) {
    const bool old_state = transmitting.load();
    if (old_state != new_state) {
        ESP_LOGD(TAG, "CatParser internal transmit state changing from %s to %s",
                 old_state ? "ON" : "OFF", new_state ? "ON" : "OFF");
        transmitting.store(new_state);

        // Notify AntennaSwitch about this change for Radio A
        ESP_LOGD(TAG, "Notifying AntennaSwitch of CAT TX state change to: %s", new_state ? "ON" : "OFF");
        AntennaSwitch::instance().on_cat_tx_a_state_change(new_state);
        
        // Broadcast transmit state change to WebSocket clients
        if (websocket_server_is_running()) {
            WebSocketServer::instance().broadcast_transmit_state_change(new_state);
        }
    } else {
        ESP_LOGV(TAG, "CatParser transmit state unchanged at: %s", new_state ? "ON" : "OFF");
    }
}


void CatParser::uart_task_trampoline(void *arg) {
    static_cast<CatParser *>(arg)->uart_task();
    vTaskDelete(nullptr);
}

void CatParser::process_serial_data(const uint8_t* data, size_t len) {
    if (!data || len == 0) return;
    
    // Update serial data timestamp (as in original)
    last_serial_data_time = std::chrono::steady_clock::now();
    MQTTClient::instance().set_has_serial_data(true);

    const std::string_view commands_view(reinterpret_cast<const char*>(data), len);
    ESP_LOGV(TAG, "Processing from API (process_serial_data): %.*s", static_cast<int>(commands_view.length()), commands_view.data());
    
    // Call the centralized process_command
    if (const esp_err_t ret = process_command(commands_view); ret != ESP_OK) {
        ESP_LOGW(TAG, "Error processing command block from process_serial_data: %s", esp_err_to_name(ret));
    }
}

void CatParser::clear_serial_data() {
    // Force the serial data timeout
    last_serial_data_time = std::chrono::steady_clock::now() - 
        std::chrono::seconds(SERIAL_DATA_TIMEOUT_S + 1);
    MQTTClient::instance().set_has_serial_data(false);
}

esp_err_t CatParser::send_to_radio(const char* command) {
    if (!command || strlen(command) == 0) {
        ESP_LOGE(TAG, "Invalid command to send to radio");
        return ESP_ERR_INVALID_ARG;
    }

    if (uart2_queue == nullptr) {
        ESP_LOGE(TAG, "UART not initialized, cannot send command to radio");
        return ESP_ERR_INVALID_STATE;
    }

    size_t command_len = strlen(command);
    ESP_LOGD(TAG, "Sending command to radio: %s", command);

    int bytes_written = uart_write_bytes(UART_NUM_2, command, command_len);
    if (bytes_written < 0) {
        ESP_LOGE(TAG, "Failed to write to UART: %d", bytes_written);
        return ESP_FAIL;
    }

    if ((size_t)bytes_written != command_len) {
        ESP_LOGW(TAG, "Partial write to UART: %d of %d bytes", bytes_written, command_len);
        return ESP_ERR_TIMEOUT;
    }

    // Flush the UART to ensure command is sent immediately
    esp_err_t flush_err = uart_wait_tx_done(UART_NUM_2, pdMS_TO_TICKS(100));
    if (flush_err != ESP_OK) {
        ESP_LOGW(TAG, "UART flush timeout: %s", esp_err_to_name(flush_err));
    }

    ESP_LOGV(TAG, "Successfully sent %d bytes to radio", bytes_written);
    return ESP_OK;
}

