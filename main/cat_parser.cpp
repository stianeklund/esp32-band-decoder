#include "cat_parser.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <cstring>
#include <string>
#include <string_view>
#include <sys/param.h>

// Initialize static member
CatParser *CatParser::instance_ = nullptr;

CatParser::CatParser()
    : uart2_queue(nullptr),
      shutdown_requested(false) {
    if (instance_ == nullptr) {
        instance_ = this;
    }

    // Initialize command handlers
    command_handlers = {
        {('F' << 8) | 'A', &CatParser::process_fa_command},
        {('A' << 8) | 'P', &CatParser::process_ap_command},
        {('I' << 8) | 'F', &CatParser::process_if_command}
    };
}

CatParser::~CatParser() {
    // Request shutdown of UART tasks
    shutdown_requested.store(true);

    // Give tasks time to shut down gracefully
    vTaskDelay(pdMS_TO_TICKS(200));

    if (instance_ == this) {
        instance_ = nullptr;
    }

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


#define UART_TASK_STACK_SIZE 8192
#define UART_QUEUE_SIZE 3

esp_err_t CatParser::init() {
    ESP_LOGI(TAG, "Initializing CAT parser");

    // Get current configuration from antenna switch
    esp_err_t ret = antenna_switch_get_config(&current_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get antenna switch configuration: %s", esp_err_to_name(ret));
        return ret;
    }

    // Validate baud rate and set default if invalid
    if (current_config.uart_baud_rate <= 0) {
        ESP_LOGW(TAG, "Invalid baud rate %d, using default 9600", current_config.uart_baud_rate);
        current_config.uart_baud_rate = 9600;
        ret = antenna_switch_set_config(&current_config);
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
        ret = antenna_switch_set_config(&current_config);
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
    io_conf.mode = GPIO_MODE_INPUT_OUTPUT;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;

    // Configure TX pin
    io_conf.pin_bit_mask = (1ULL << current_config.uart_tx_pin);
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    // Configure RX pin 
    io_conf.pin_bit_mask = (1ULL << current_config.uart_rx_pin);
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
    static char temp_buffer[128];
    constexpr TickType_t xTicksToWait = pdMS_TO_TICKS(10);
    int events_processed = 0;
    std::string command_accumulator;

    while (!shutdown_requested.load()) {
        constexpr int MAX_EVENTS_PER_ITERATION = 5;
        events_processed = 0;

        while (events_processed < MAX_EVENTS_PER_ITERATION &&
               xQueueReceive(uart2_queue, &event, xTicksToWait) == pdTRUE) {
            events_processed++;

            switch (event.type) {
                case UART_DATA: {
                    if (uart_get_buffered_data_len(UART_NUM, &buffered_size) == ESP_OK) {
                        const int len = uart_read_bytes(UART_NUM, temp_buffer,
                                                        std::min(buffered_size, sizeof(temp_buffer) - 1),
                                                        pdMS_TO_TICKS(1));
                        if (len > 0) {
                            temp_buffer[len] = '\0';
                            command_accumulator += temp_buffer;

                            // Process complete commands
                            size_t pos;
                            while ((pos = command_accumulator.find(';')) != std::string::npos) {
                                std::string cmd = command_accumulator.substr(0, pos);
                                command_accumulator = command_accumulator.substr(pos + 1);
                                ESP_LOGI(TAG, "Received:%s",cmd.c_str());

                                // Only process FA and IF commands
                                if (cmd.length() >= 2) {
                                    if (cmd.substr(0, 2) == "FA") {
                                        process_fa_command(cmd.substr(2));
                                    } else if (cmd.substr(0, 2) == "IF") {
                                        process_if_command(cmd.substr(2));
                                    }
                                    // Ignore all other commands
                                }
                            }
                        }
                    }
                    break;
                }

                case UART_FIFO_OVF:
                case UART_BUFFER_FULL:
                    ESP_LOGW(TAG, "Buffer issue detected, flushing UART");
                    uart_flush_input(UART_NUM);
                    xQueueReset(uart2_queue);
                    command_accumulator.clear(); // Clear accumulated data
                    break;

                default:
                    break;
            }
        }
        // Always yield after processing events or timeout
        taskYIELD();
    }
}

esp_err_t CatParser::update_config() {
    ESP_LOGD(TAG, "Updating CAT parser configuration");

    if (const esp_err_t ret = antenna_switch_get_config(&current_config); ret != ESP_OK) {
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

    // Get new frequency's band index
    const int new_band_index = get_band_index(frequency);

    // Switch antenna if band changed or no valid band was set
    if (current_band_index != new_band_index) {
        ESP_LOGV(TAG, "Frequency requires band change, setting new antenna");

        if (const esp_err_t ret = antenna_switch_set_frequency(frequency); ret != ESP_OK) {
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

esp_err_t CatParser::process_ap_command(const std::string_view command) {
    char *endptr;

    // Convert string_view to C string for strtoul
    const std::string ports_str(command);
    const unsigned long ports = strtoul(ports_str.c_str(), &endptr, 10);

    if (*endptr != '\0') {
        ESP_LOGE(TAG, "Invalid ports format in command: %.*s",
                 static_cast<int>(command.length()), command.data());
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGD(TAG, "Setting antenna ports: %lu", ports);
    antenna_switch_config_t config;
    esp_err_t ret = antenna_switch_get_config(&config);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get config: %s", esp_err_to_name(ret));
        return ret;
    }

    config.num_antenna_ports = ports;
    ret = antenna_switch_set_config(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set config: %s", esp_err_to_name(ret));
        return ret;
    }
    return ESP_OK;
}

esp_err_t CatParser::process_command(const char *command) {
    if (!command) {
        return ESP_ERR_INVALID_ARG;
    }

    std::string_view cmd_str(command);
    size_t start = 0;
    size_t commands_processed = 0;

    while (start < cmd_str.length()) {
        if (constexpr size_t MAX_COMMANDS_PER_BATCH = 5; commands_processed >= MAX_COMMANDS_PER_BATCH) {
            taskYIELD(); // Allow other tasks to run
            commands_processed = 0;
        }
        size_t end = cmd_str.find(';', start);
        if (end == std::string::npos) {
            end = cmd_str.length();
        }

        if (end - start >= 2) {
            std::string_view cmd_view = cmd_str.substr(start, end - start);
            uint16_t cmd_code = (static_cast<uint16_t>(cmd_view[0]) << 8) | cmd_view[1];

            if (auto handler_it = command_handlers.find(cmd_code); handler_it != command_handlers.end()) {
                std::string_view param = (cmd_view.length() > 2) ? cmd_view.substr(2) : std::string_view();
                if (handler_it->second) {
                    const CommandHandler handler = handler_it->second;
                    if (const esp_err_t ret = (this->*handler)(std::string(param)); ret != ESP_OK) {
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
    // Use cached band index if frequency is current
    if (freq == current_frequency && current_band_index != -1) {
        return current_band_index;
    }

    // Find which band the frequency belongs to
    for (int i = 0; i < current_config.num_bands; i++) {
        if (freq >= current_config.bands[i].start_freq &&
            freq <= current_config.bands[i].end_freq) {
            return i;
        }
    }
    return -1;
}

bool CatParser::is_same_band(const uint32_t freq1, const uint32_t freq2) const {
    const int band1 = get_band_index(freq1);
    const int band2 = get_band_index(freq2);
    return (band1 != -1) && (band1 == band2);
}

esp_err_t CatParser::process_if_command(const std::string_view command) {
    if (command.length() < 35) {
        ESP_LOGW(TAG, "IF command too short: %.*s",
                 static_cast<int>(command.length()), command.data());
        return ESP_OK;
    }

    char *endptr;
    // Parse frequency (first 11 chars after "IF")
    const std::string freq_str(command.substr(0, 11));
    const auto frequency = strtoul(freq_str.c_str(), &endptr, 10);
    if (*endptr != '\0') {
        ESP_LOGW(TAG, "Invalid frequency in IF command: %s", freq_str.c_str());
        return ESP_OK;
    }

    const auto new_tx_state = (command[26] == '1');

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
    char *endptr;

    const std::string freq_str(command);
    const auto frequency = strtoul(freq_str.c_str(), &endptr, 10);

    if (*endptr != '\0') {
        ESP_LOGE(TAG, "Invalid frequency format in command: %.*s",
                 static_cast<int>(command.length()), command.data());
        return ESP_OK;
    }

    ESP_LOGV(TAG, "FA command frequency: %lu Hz", frequency);
    return handle_frequency_change(frequency);
}

void CatParser::uart_task_trampoline(void *arg) {
    ESP_LOGI(TAG, "UART_TASK_TRAMPOLINE");
    static_cast<CatParser *>(arg)->uart_task();
    vTaskDelete(nullptr);
}
