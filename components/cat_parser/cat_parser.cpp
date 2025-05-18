#include "include/cat_parser.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <cstring>
#include "my_mqtt_client.h"
#include <string>
#include <string_view>
#include <sys/param.h>
#include "driver/gpio.h"
#include <chrono>
#include <charconv>

CatParser::CatParser()
    : uart2_queue(nullptr),
      shutdown_requested(false),
      last_serial_data_time(std::chrono::steady_clock::now()) {

    // Initialize command handlers
    command_handlers = {
        {'F' << 8 | 'A', &CatParser::process_fa_command},
        {'A' << 8 | 'I', &CatParser::process_ai_command},
        {'A' << 8 | 'P', &CatParser::process_ap_command},
        {'I' << 8 | 'F', &CatParser::process_if_command},
        {'T' << 8 | 'X', &CatParser::process_tx_command},
        {'R' << 8 | 'X', &CatParser::process_rx_command}
    };
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

CatParser &CatParser::instance() {
    static CatParser instance;
    return instance;
}


#define UART_TASK_STACK_SIZE 4096
#define UART_QUEUE_SIZE 3

esp_err_t CatParser::init() {
    ESP_LOGD(TAG, "Initializing CAT parser");

    // Get current configuration from antenna switch
    esp_err_t ret = AntennaSwitch::instance().get_config(&current_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get antenna switch configuration: %s", esp_err_to_name(ret));
        return ret;
    }

    // Validate baud rate and set default if invalid
    if (current_config.uart_baud_rate <= 0) {
        ESP_LOGW(TAG, "Invalid baud rate %d, using default 9600", current_config.uart_baud_rate);
        current_config.uart_baud_rate = 9600;
        ret = AntennaSwitch::instance().set_config(&current_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save default baud rate: %s", esp_err_to_name(ret));
            return ret;
        }
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
    if (current_config.uart_tx_pin < 0 || current_config.uart_rx_pin < 0) {
        ESP_LOGW(TAG, "Invalid UART pins, using defaults TX=17, RX=16");
        current_config.uart_tx_pin = 33; // HT2
        current_config.uart_rx_pin = 32; // HT1
        ret = AntennaSwitch::instance().set_config(&current_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save default UART pins: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    // Configure UART2 with minimal settings
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_2, &uart2_config));
    vTaskDelay(pdMS_TO_TICKS(10));

    // Set configured pins before driver installation
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_2, 
                                current_config.uart_tx_pin,
                                current_config.uart_rx_pin,
                                UART_PIN_NO_CHANGE,
                                UART_PIN_NO_CHANGE));

    // Disable internal pullups since external ones are present on KC868
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
        tskIDLE_PRIORITY + 1, nullptr
    );

    if (xReturned != pdPASS) {
        ESP_LOGE(TAG, "Failed to create UART task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Initialization complete");
    return ESP_OK;
}

void CatParser::uart_task() {
    uart_event_t event;
    size_t buffered_size;
    uint8_t read_buf[128]; // Non-static local buffer
    constexpr TickType_t xTicksToWait = pdMS_TO_TICKS(10); // Timeout for xQueueReceive
    std::string command_accumulator;
    // Optional: Pre-reserve capacity if you have an estimate of typical command backlog size
    // command_accumulator.reserve(256); 

    ESP_LOGI(TAG, "CAT parser UART task started.");

    while (!shutdown_requested.load()) {
        if (xQueueReceive(uart2_queue, &event, xTicksToWait) == pdTRUE) {
            MQTTClient::instance().set_has_serial_data(true);
            last_serial_data_time = std::chrono::steady_clock::now();

            switch (event.type) {
                case UART_DATA: {
                    if (uart_get_buffered_data_len(UART_NUM_2, &buffered_size) == ESP_OK && buffered_size > 0) {
                        const int len = uart_read_bytes(UART_NUM_2, read_buf,
                                                        std::min(buffered_size, sizeof(read_buf)),
                                                        0); // No need to wait, data is already there
                        if (len > 0) {
                            command_accumulator.append(reinterpret_cast<char*>(read_buf), len);
                            process_accumulated_commands(command_accumulator);
                        }
                    }
                    break;
                }

                case UART_FIFO_OVF:
                case UART_BUFFER_FULL:
                    ESP_LOGW(TAG, "UART buffer issue detected (type: %d), flushing UART and resetting queue.", event.type);
                    uart_flush_input(UART_NUM_2);
                    xQueueReset(uart2_queue);
                    command_accumulator.clear(); // Clear any partial commands
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
            // xQueueReceive timed out, no new UART event
            if (auto now = std::chrono::steady_clock::now(); std::chrono::duration_cast<std::chrono::seconds>(
                    now - last_serial_data_time).count() > SERIAL_DATA_TIMEOUT_S) {

                // Check current state before setting to avoid redundant calls if already false
                // This assumes MQTTClient::instance().get_has_serial_data() or similar exists,
                // or that set_has_serial_data(false) is cheap.
                // For now, using the existing direct call:
                MQTTClient::instance().set_has_serial_data(false);
            }
        }
        // Yield to allow other tasks to run. This is important for system responsiveness.
        taskYIELD();
    }
    ESP_LOGI(TAG, "CAT parser UART task shutting down.");
}

void CatParser::process_accumulated_commands(std::string& accumulator) {
    size_t process_start_pos = 0; 

    while (process_start_pos < accumulator.length()) {
        const size_t end_delim_pos = accumulator.find(';', process_start_pos);
        
        if (end_delim_pos == std::string::npos) {
            // No more complete commands (no ';') in the remaining part of the accumulator.
            // Break and leave the partial command (if any) for the next data arrival.
            break; 
        }

        // Create a view for the command string (from current start up to delimiter)
        std::string_view cmd_full_str_view(&accumulator[process_start_pos], end_delim_pos - process_start_pos);
        
        ESP_LOGV(TAG, "Processing from accumulator: %.*s", static_cast<int>(cmd_full_str_view.length()), cmd_full_str_view.data());

        if (cmd_full_str_view.length() >= 2) { // Command must be at least 2 chars
            uint16_t cmd_code = static_cast<uint16_t>(cmd_full_str_view[0]) << 8 | cmd_full_str_view[1];

            const std::string_view cmd_payload = cmd_full_str_view.length() > 2 ?
                                           cmd_full_str_view.substr(2) :
                                           std::string_view();

            if (auto handler_it = command_handlers.find(cmd_code); handler_it != command_handlers.end()) {
                if (handler_it->second) { // Check if the function pointer is not null
                    const CommandHandler handler = handler_it->second;
                    if (const esp_err_t ret = (this->*handler)(cmd_payload); ret != ESP_OK) {
                        ESP_LOGW(TAG, "Command handler for '%.*s' returned error %s", 
                                 static_cast<int>(cmd_full_str_view.substr(0,2).length()), cmd_full_str_view.substr(0,2).data(), 
                                 esp_err_to_name(ret));
                    }
                }
            } else {
                ESP_LOGV(TAG, "No handler for command prefix: '%.*s'", static_cast<int>(cmd_full_str_view.substr(0,2).length()), cmd_full_str_view.substr(0,2).data());
            }
        } else if (!cmd_full_str_view.empty()) {
            ESP_LOGD(TAG, "Short command received (length < 2): %.*s", static_cast<int>(cmd_full_str_view.length()), cmd_full_str_view.data());
        }

        // Advance start position past the processed command and its delimiter
        process_start_pos = end_delim_pos + 1;
    }

    // After processing all complete commands, remove them from the beginning of the accumulator string
    if (process_start_pos > 0) {
        accumulator.erase(0, process_start_pos);
    }
}

esp_err_t CatParser::update_config() {
    ESP_LOGD(TAG, "Updating CAT parser configuration");

    if (const esp_err_t ret = AntennaSwitch::instance().get_config(&current_config); ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get antenna switch configuration: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGD(TAG, "CAT parser configuration updated successfully");
    return ESP_OK;
}

esp_err_t CatParser::handle_frequency_change(const uint32_t frequency) {
    // Early return if frequency hasn't changed
    if (frequency == current_frequency) {
        return ESP_OK;
    }

    // Get a new frequency's band index
    const int new_band_index = get_band_index(frequency);

    // Switch antenna if the band changed or no valid band was set
    if (current_band_index != new_band_index) {
        ESP_LOGV(TAG, "Frequency requires band change, setting new antenna");

        if (const esp_err_t ret = AntennaSwitch::instance().set_frequency(frequency); ret != ESP_OK) {
            if (ret == ESP_ERR_NOT_FOUND) {
                ESP_LOGW(TAG, "Frequency %lu Hz not supported by any configured band", frequency);
            } else {
                ESP_LOGE(TAG, "Failed to set frequency: %s", esp_err_to_name(ret));
            }
            return ret;
        }
    } else {
        ESP_LOGV(TAG, "Frequency is in the same band, skipping antenna switch");
    }

    // Update cached values
    current_frequency = frequency;
    current_band_index = new_band_index;
    return ESP_OK;
}

// ReSharper disable once CppMemberFunctionMayBeStatic
esp_err_t CatParser::process_ai_command(const std::string_view command) {
    unsigned long ports_val;
    const char* const start_ptr = command.data();
    const char* const end_ptr = command.data() + command.length();
    auto result = std::from_chars(start_ptr, end_ptr, ports_val);

    if (result.ec == std::errc() && result.ptr == end_ptr) {
        ESP_LOGD(TAG, "Setting antenna ports (AI): %lu", ports_val);

        if (ports_val > UINT8_MAX) {
            ESP_LOGE(TAG, "Ports value %lu for AI command exceeds uint8_t max", ports_val);
            return ESP_ERR_INVALID_ARG;
        }

        antenna_switch_config_t config; // Stack allocation of large struct
        esp_err_t ret = AntennaSwitch::instance().get_config(&config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to get config for AI command: %s", esp_err_to_name(ret));
            return ret;
        }

        config.num_antenna_ports = static_cast<uint8_t>(ports_val);
        ret = AntennaSwitch::instance().set_config(&config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set config for AI command: %s", esp_err_to_name(ret));
            return ret;
        }
        return ESP_OK;
    }
    ESP_LOGE(TAG, "Invalid ports format in AI command: %.*s",
             static_cast<int>(command.length()), command.data());
    return ESP_ERR_INVALID_ARG;
}

// ReSharper disable once CppMemberFunctionMayBeStatic
esp_err_t CatParser::process_ap_command(const std::string_view command) {
    unsigned long ports_val;
    const char* const start_ptr = command.data();
    const char* const end_ptr = command.data() + command.length();
    auto result = std::from_chars(start_ptr, end_ptr, ports_val);

    if (result.ec == std::errc() && result.ptr == end_ptr) {
        ESP_LOGD(TAG, "Setting antenna ports (AP): %lu", ports_val);

        if (ports_val > UINT8_MAX) {
            ESP_LOGE(TAG, "Ports value %lu for AP command exceeds uint8_t max", ports_val);
            return ESP_ERR_INVALID_ARG;
        }

        antenna_switch_config_t config; // Stack allocation of large struct
        esp_err_t ret = AntennaSwitch::instance().get_config(&config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to get config for AP command: %s", esp_err_to_name(ret));
            return ret;
        }

        config.num_antenna_ports = static_cast<uint8_t>(ports_val);
        ret = AntennaSwitch::instance().set_config(&config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set config for AP command: %s", esp_err_to_name(ret));
            return ret;
        }
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "Invalid ports format in AP command: %.*s",
                 static_cast<int>(command.length()), command.data());
        return ESP_ERR_INVALID_ARG;
    }
}

void CatParser::handle_frequency_update(const uint32_t frequency) {
    ESP_LOGD(TAG, "Handling frequency update: %lu Hz", frequency);
    handle_frequency_change(frequency);
}

esp_err_t CatParser::process_command(const char *command) {
    if (!command) {
        return ESP_ERR_INVALID_ARG;
    }


    std::string_view cmd_str(command);
    size_t start = 0;
    size_t commands_processed = 0;

    while (start < cmd_str.length()) {
        if (constexpr size_t MAX_COMMANDS_PER_BATCH = 2; commands_processed >= MAX_COMMANDS_PER_BATCH) {
            taskYIELD(); // Allow other tasks to run
            commands_processed = 0;
        }
        size_t end = cmd_str.find(';', start);
        if (end == std::string::npos) {
            end = cmd_str.length();
        }

        if (end - start >= 2) {
            std::string_view cmd_view = cmd_str.substr(start, end - start);
            uint16_t cmd_code = static_cast<uint16_t>(cmd_view[0]) << 8 | cmd_view[1];

            if (auto handler_it = command_handlers.find(cmd_code); handler_it != command_handlers.end()) {
                const std::string_view param = cmd_view.length() > 2 ? cmd_view.substr(2) : std::string_view();
                if (handler_it->second) {
                    const CommandHandler handler = handler_it->second;
                    // Pass param directly as std::string_view, avoiding temporary std::string
                    if (const esp_err_t ret = (this->*handler)(param); ret != ESP_OK) {
                        return ret;
                    }
                }
            }
            commands_processed++;
        }
        start = end + 1;
    }

    return ESP_OK;
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

esp_err_t CatParser::process_if_command(const std::string_view command) {
    if (command.length() < 35) {
        ESP_LOGW(TAG, "IF command too short: %.*s",
                 static_cast<int>(command.length()), command.data());
        return ESP_OK;
    }

    uint32_t frequency;
    // Parse frequency (first 11 chars of the command payload)
    const std::string_view freq_sv = command.substr(0, 11);
    const char* const freq_start_ptr = freq_sv.data();
    const char* const freq_end_ptr = freq_sv.data() + freq_sv.length();

    // ReSharper disable once CppUseStructuredBinding
    auto fc_result = std::from_chars(freq_start_ptr, freq_end_ptr, frequency);

    if (fc_result.ec != std::errc() || fc_result.ptr != freq_end_ptr) {
        ESP_LOGW(TAG, "Invalid frequency in IF command: %.*s", static_cast<int>(freq_sv.length()), freq_sv.data());
        return ESP_OK; // Keep original behavior
    }

    const auto new_tx_state = command[26] == '1';

    // Parse mode
    const char mode_char = command[27];
    std::string new_mode;

    switch (mode_char) {
        case '1':
            new_mode = "LSB";
            break;
        case '2':
            new_mode = "USB";
            break;
        case '3':
            new_mode = "CW-U";
            break;
        case '4':
            new_mode = "FM";
            break;
        case '5':
            new_mode = "AM";
            break;
        case '6':
            new_mode = "DIG-L";
            break;
        case '7':
            new_mode = "CW-L";
            break;
        case '9':
            new_mode = "DIG-U";
            break;
        default:
            new_mode = "UNKNOWN";
    }

    if (new_tx_state != transmitting) {
        ESP_LOGI(TAG, "Radio %s", new_tx_state ? "started transmitting" : "stopped transmitting");
    }

    if (new_mode != current_mode) {
        ESP_LOGV(TAG, "Mode changed to %s", new_mode.c_str());
    }

    // Update states
    transmitting = new_tx_state;
    current_mode = new_mode;

    ESP_LOGV(TAG, "IF command: freq=%lu Hz, mode=%s, tx=%d", frequency, current_mode.c_str(), transmitting);

    return handle_frequency_change(frequency);
}

esp_err_t CatParser::process_fa_command(const std::string_view command) {
    uint32_t frequency;
    const char* const start_ptr = command.data();
    const char* const end_ptr = command.data() + command.length();
    auto result = std::from_chars(start_ptr, end_ptr, frequency);

    if (result.ec == std::errc() && result.ptr == end_ptr) {
        ESP_LOGV(TAG, "FA command frequency: %lu Hz", frequency);
        return handle_frequency_change(frequency);
    }

    ESP_LOGE(TAG, "Invalid frequency format in FA command: %.*s",
             static_cast<int>(command.length()), command.data());
    // We don't want to error here (I guess)
    return ESP_OK;
}

esp_err_t CatParser::process_tx_command(std::string_view payload) {
    (void)payload; // Mark as unused, as TX command typically doesn't have a payload
    if (!transmitting) { // Only log and update if state actually changes
        transmitting = true;
        ESP_LOGI(TAG, "Radio started transmitting (TX command)");
        // If other components need to be notified about TX state change directly from here, add calls.
        // For example: AntennaSwitch::instance().notify_tx_state(true);
    }
    return ESP_OK;
}

esp_err_t CatParser::process_rx_command(std::string_view payload) {
    (void)payload; // Mark as unused
    if (transmitting) { // Only log and update if state actually changes
        transmitting = false;
        ESP_LOGI(TAG, "Radio stopped transmitting (RX command)");
        // If other components need to be notified:
        // AntennaSwitch::instance().notify_tx_state(false);
    }
    return ESP_OK;
}

void CatParser::uart_task_trampoline(void *arg) {
    static_cast<CatParser *>(arg)->uart_task();
    vTaskDelete(nullptr);
}

void CatParser::process_serial_data(const uint8_t* data, size_t len) {
    if (!data || len == 0) return;
    
    // Create a temporary buffer for the command
    char temp_buffer[128];
    const size_t copy_len = std::min(len, sizeof(temp_buffer) - 1);

    memcpy(temp_buffer, data, copy_len);
    temp_buffer[copy_len] = '\0';

    // Process the command
    process_command(temp_buffer);
    
    // Update serial data timestamp
    last_serial_data_time = std::chrono::steady_clock::now();
    MQTTClient::instance().set_has_serial_data(true);
}

void CatParser::clear_serial_data() {
    // Force the serial data timeout
    last_serial_data_time = std::chrono::steady_clock::now() - 
        std::chrono::seconds(SERIAL_DATA_TIMEOUT_S + 1);
    MQTTClient::instance().set_has_serial_data(false);
}
