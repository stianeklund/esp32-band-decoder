#include "include/cat_parser.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "my_mqtt_client.h"
#include <string>
#include <string_view>
#include "driver/gpio.h"
#include <chrono>
#include <charconv>
#include <memory> // Keep for other uses if any, or remove if not needed elsewhere
#include "config_manager.h"
#include "antenna_switch.h"

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

    // Get current configuration from antenna switch using get_config_ref()
    // current_config is a member, so this assigns a copy of the referenced config.
    current_config = AntennaSwitch::instance().get_config_ref();

    // Validate baud rate and set default if invalid
    if (current_config.uart_baud_rate <= 0) {
        ESP_LOGW(TAG, "Invalid baud rate %d, using defaults", current_config.uart_baud_rate);
        current_config.uart_baud_rate = 9600;
        // Pass the modified current_config (which is a copy) to set_config
        esp_err_t ret = AntennaSwitch::instance().set_config(&current_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save default baud rate: %s", esp_err_to_name(ret));
            return ret;
        }
        // Re-fetch the config if it was updated by set_config to ensure consistency,
        // especially if set_config does more than just pass to ConfigManager.
        current_config = AntennaSwitch::instance().get_config_ref();
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
        current_config = AntennaSwitch::instance().get_config_ref();
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
            if (auto now = std::chrono::steady_clock::now(); std::chrono::duration_cast<std::chrono::seconds>(
                    now - last_serial_data_time).count() > SERIAL_DATA_TIMEOUT_S) {
                MQTTClient::instance().set_has_serial_data(false);
            }
        }
        taskYIELD();
    }
    ESP_LOGI(TAG, "CAT parser UART task shutting down.");
}

void CatParser::uart0_to_uart2_task()
{
}

esp_err_t CatParser::dispatch_one_command(std::string_view command_view) {
    if (command_view.length() < 2) { // Command must be at least 2 chars
        if (!command_view.empty()) {
            ESP_LOGD(TAG, "Short command received (length < 2): %.*s", static_cast<int>(command_view.length()), command_view.data());
        }
        return ESP_OK; // Consistent with previous behavior of not erroring on short/empty segments
    }

    uint16_t cmd_code = static_cast<uint16_t>(command_view[0]) << 8 | command_view[1];
    const std::string_view cmd_payload = command_view.length() > 2 ? command_view.substr(2) : std::string_view();

    auto handler_it = command_handlers.find(cmd_code);
    if (handler_it != command_handlers.end()) {
        if (handler_it->second) { // Check if the function pointer is not null
            const CommandHandler handler = handler_it->second;

            const esp_err_t ret = (this->*handler)(cmd_payload);

            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Command handler for '%.*s' returned error %s",
                         static_cast<int>(command_view.substr(0,2).length()), command_view.substr(0,2).data(),
                         esp_err_to_name(ret));
            }
            return ret; // Return the handler's result
        }
    } else {
        ESP_LOGV(TAG, "No handler for command prefix: '%.*s'", static_cast<int>(command_view.substr(0,2).length()), command_view.substr(0,2).data());
    }
    return ESP_OK; // No handler found or handler was null
}

esp_err_t CatParser::update_config() {
    ESP_LOGD(TAG, "Updating CAT parser configuration");

    // Update the member current_config by copying from the reference
    current_config = AntennaSwitch::instance().get_config_ref();
    // No esp_err_t to check from get_config_ref itself.
    // If AntennaSwitch or ConfigManager had an issue providing the ref, it'd be a deeper problem.

    ESP_LOGD(TAG, "CAT parser configuration updated successfully");
    return ESP_OK;
}

esp_err_t CatParser::handle_frequency_change(const uint32_t frequency) {
    if (frequency == current_frequency) {
        return ESP_OK;
    }

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

esp_err_t CatParser::process_ai_command(const std::string_view command_payload) {
    if (command_payload.empty()) {
        // This is a query command: AI;
        // A Kenwood radio would respond AI<P1>; e.g. AI0; if no auto information,or AI2; if auto information
        // For now, we just log that we received the query.
        ESP_LOGV(TAG, "AI Query (AI;) received. Radio Auto Information is currently %s. Polling %s.",
                 radio_provides_auto_updates_ ? "ON" : "OFF",
                 radio_provides_auto_updates_ ? "not required" : "required");
        // TODO: Implement response if this system needs to act as a device responding to AI;
        return ESP_OK;
    }

    if (command_payload.length() == 1) {
        // This is a set command: AI<P1>;
        const char p1_val = command_payload[0];
        ESP_LOGD(TAG, "Processing Kenwood AI Set command (AI%c;)", p1_val);

        switch (p1_val) {
        case '0': // Auto Information OFF
            radio_provides_auto_updates_ = false;
            ESP_LOGI(TAG, "Kenwood AI Set: Radio Auto Information OFF. CAT polling will be required for updates.");
            break;
        case '1': // Auto Information ON (1 second interval)
        case '2': // Auto Information ON (2 second interval)
            radio_provides_auto_updates_ = true;
            ESP_LOGI(TAG, "Kenwood AI Set: Radio Auto Information ON (P1=%c). CAT polling not required.", p1_val);
            break;
        default:
            ESP_LOGE(TAG, "Kenwood AI Set: Invalid parameter P1='%c'. Expected '0' through '6'.", p1_val);
            return ESP_ERR_INVALID_ARG;
        }
        // The command has been processed, and the state `radio_provides_auto_updates_` is updated.
        // Other parts of the system can use this state to decide on polling.
        // No direct call to AntennaSwitch auto_mode, as Kenwood AI is about radio data reporting.
        return ESP_OK;
    }
    
    // Invalid payload length for AI command (e.g., AI12;)
    ESP_LOGE(TAG, "Kenwood AI command '%.*s' has invalid payload length. Expected empty or 1 char.",
             static_cast<int>(command_payload.length()), command_payload.data());
    return ESP_ERR_INVALID_ARG;
}

std::from_chars_result CatParser::get_from_chars_result(const std::string_view command, unsigned long& ports_val){
    const char* const start_ptr = command.data();
    const auto end_ptr = command.data() + command.length();
    return std::from_chars(start_ptr, end_ptr, ports_val);
}

// ReSharper disable once CppMemberFunctionMayBeStatic
esp_err_t CatParser::process_ap_command(const std::string_view command) {
    unsigned long ports_val;
    const auto end_ptr = command.data() + command.length();
    const auto result = get_from_chars_result(command, ports_val);

    if (result.ec == std::errc() && result.ptr == end_ptr) {
        ESP_LOGD(TAG, "Setting antenna ports (AP): %lu", ports_val);

        if (ports_val > UINT8_MAX) {
            ESP_LOGE(TAG, "Ports value %lu for AP command exceeds uint8_t max", ports_val);
            return ESP_ERR_INVALID_ARG;
        }

        // Get a const reference to the current config
        const auto& current_cfg_ref = AntennaSwitch::instance().get_config_ref();
        // Create a mutable copy on the stack
        antenna_switch_config_t temp_config = current_cfg_ref;

        // Modify the copy
        temp_config.num_antenna_ports = static_cast<uint8_t>(ports_val);
        
        // Set the modified config
        esp_err_t ret = AntennaSwitch::instance().set_config(&temp_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set config for AP command: %s", esp_err_to_name(ret));
            return ret;
        }
        // If set_config was successful, update CatParser's own current_config member
        // to reflect this change immediately.
        current_config = AntennaSwitch::instance().get_config_ref();
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Invalid ports format in AP command: %.*s",
             static_cast<int>(command.length()), command.data());
    return ESP_ERR_INVALID_ARG;
}

void CatParser::handle_frequency_update(const uint32_t frequency) {
    ESP_LOGD(TAG, "Handling frequency update: %lu Hz", frequency);
    handle_frequency_change(frequency);
}

// Public C-string version calls the string_view version
esp_err_t CatParser::process_command(const char *command_cstr) {
    if (!command_cstr) {
        return ESP_ERR_INVALID_ARG;
    }
    return process_command(std::string_view(command_cstr)); // Call the string_view version
}

// New core process_command implementation
esp_err_t CatParser::process_command(std::string_view commands_str_with_semicolons) {
    size_t start_pos = 0;
    esp_err_t first_error = ESP_OK; // To track the first error encountered

    while (start_pos < commands_str_with_semicolons.length()) {
        size_t end_pos = commands_str_with_semicolons.find(';', start_pos);
        std::string_view command_content;

        if (end_pos == std::string_view::npos) { // No more semicolons, process the rest
            command_content = commands_str_with_semicolons.substr(start_pos);
            start_pos = commands_str_with_semicolons.length(); // Mark as consumed
        } else { // Semicolon found
            command_content = commands_str_with_semicolons.substr(start_pos, end_pos - start_pos);
            start_pos = end_pos + 1; // Move past the semicolon for next iteration
        }

        if (!command_content.empty()) { // Only dispatch if there's actual content
            ESP_LOGV(TAG, "Dispatching from process_command: %.*s", static_cast<int>(command_content.length()), command_content.data());
            // dispatch_one_command expects the command *without* the semicolon.
            esp_err_t dispatch_ret = dispatch_one_command(command_content);
            if (dispatch_ret != ESP_OK) {
                ESP_LOGW(TAG, "Error dispatching command '%.*s': %s",
                         static_cast<int>(command_content.length()), command_content.data(),
                         esp_err_to_name(dispatch_ret));
                if (first_error == ESP_OK) { // Store the first error encountered
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
    std::string new_mode_str; // Renamed to avoid conflict with member `current_mode`

    switch (mode_char) {
        case '1': new_mode_str = "LSB"; break;
        case '2': new_mode_str = "USB"; break;
        case '3': new_mode_str = "CW-U"; break;
        case '4': new_mode_str = "FM"; break;
        case '5': new_mode_str = "AM"; break;
        case '6': new_mode_str = "DIG-L"; break;
        case '7': new_mode_str = "CW-L"; break;
        case '9': new_mode_str = "DIG-U"; break;
        default:  new_mode_str = "UNKNOWN";
    }

    const bool tx_state_changed = new_tx_state != transmitting; // Compare new with current member state
    std::string old_mode = current_mode;                   // Capture current mode before update

    // Update internal states
    transmitting = new_tx_state;
    current_mode = new_mode_str;


    if (tx_state_changed) {
        ESP_LOGV(TAG, "Radio %s (IF command)", transmitting ? "started transmitting" : "stopped transmitting");
        if (transmitting) {
            AntennaSwitch::instance().on_radio_a_tx_start();
        } else {
            AntennaSwitch::instance().on_radio_a_tx_stop();
        }
    }

    if (current_mode != old_mode && !tx_state_changed) {
        ESP_LOGV(TAG, "Mode changed from %s to %s", old_mode.c_str(), current_mode.c_str());
    } else if (current_mode != old_mode && tx_state_changed) {
        ESP_LOGV(TAG, "Mode also changed from %s to %s", old_mode.c_str(), current_mode.c_str());
    }


    ESP_LOGV(TAG, "IF command: freq=%lu Hz, mode=%s, tx=%d", frequency, current_mode.c_str(), transmitting);

    return handle_frequency_change(frequency);
}

esp_err_t CatParser::process_fa_command(const std::string_view command) {
    uint32_t frequency;

    const char* const start_ptr = command.data();
    const char* const end_ptr = command.data() + command.length();

    // ReSharper disable once CppLocalVariableMayBeConst
    // ReSharper disable once CppTooWideScopeInitStatement
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

esp_err_t CatParser::process_tx_command(const std::string_view payload) {
    (void)payload; // ignore the payload, not needed here
    if (!transmitting) {
        transmitting = true;
        ESP_LOGD(TAG, "Radio started transmitting (TX command)");
        AntennaSwitch::instance().on_radio_a_tx_start(); // Notify AntennaSwitch
    }
    return ESP_OK;
}

esp_err_t CatParser::process_rx_command(const std::string_view payload) {
    (void)payload;
    if (transmitting) {
        transmitting = false;
        ESP_LOGD(TAG, "Radio stopped transmitting (RX command)");
        AntennaSwitch::instance().on_radio_a_tx_stop(); // Notify AntennaSwitch
    }
    return ESP_OK;
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
