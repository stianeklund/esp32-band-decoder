#ifndef CAT_PARSER_H
#define CAT_PARSER_H

#include "esp_err.h"
#include "driver/uart.h"
#include "antenna_switch.h"
#include <string_view>
#include <unordered_map>
#include <atomic>
#include <string>
#include <chrono>
#define MAX_CAT_COMMAND_LENGTH 32
#define UART_NUM UART_NUM_2
#define UART_BAUD_RATE 9600
#define BUF_SIZE 256  // Reduced buffer size
#define MAX_EVENTS_PER_LOOP 3  // Limit events processed per loop

class CatParser {
public:
    CatParser();

    ~CatParser();

    esp_err_t init();

    esp_err_t process_command(const char *command_cstr);
    void process_serial_data(const uint8_t* data, size_t len);
    void clear_serial_data();

    esp_err_t update_config();

    uint32_t get_frequency() const { return current_frequency; }
    bool is_transmitting() const { return transmitting; }
    void set_transmitting(const bool tx_state) { transmitting = tx_state; }
    bool is_rit_on() const { return rit_on; }
    bool is_xit_on() const { return xit_on; }
    bool is_split_on() const { return split_on; }
    const char *get_mode() const { return current_mode.c_str(); }
    int32_t get_rit_offset() const { return rit_offset; }

    esp_err_t handle_frequency_change(uint32_t frequency);

    void handle_frequency_update(uint32_t frequency);

    // Legacy C-style interface for backward compatibility
    static CatParser &instance();

private:
    using CommandHandler = esp_err_t (CatParser::*)(std::string_view);

    void uart_task();
    static void uart0_to_uart2_task();

    int get_band_index(uint32_t freq) const;
    bool is_same_band(uint32_t freq1, uint32_t freq2) const;

    esp_err_t process_fa_command(std::string_view command);

    esp_err_t process_if_command(std::string_view command);

    esp_err_t process_ap_command(std::string_view command);
    esp_err_t process_ai_command(std::string_view command_payload);
    static std::from_chars_result get_from_chars_result(std::string_view command, unsigned long& ports_val);
    esp_err_t process_tx_command(std::string_view payload);
    esp_err_t process_rx_command(std::string_view payload);

    esp_err_t process_command(std::string_view commands_str_with_semicolons); // New core processor
    esp_err_t dispatch_one_command(std::string_view command_view); 

    static void uart_task_trampoline(void *arg);

    uint32_t get_current_frequency() const { return current_frequency; }

    std::unordered_map<uint16_t, CommandHandler> command_handlers;
    QueueHandle_t uart2_queue;
    antenna_switch_config_t current_config{};
    uint32_t current_frequency{0};
    int current_band_index{-1}; // Cache for current frequency's band
    bool transmitting{false}; // Tracks if radio is transmitting
    bool rit_on{false}; // RIT status
    bool xit_on{false}; // XIT status
    bool split_on{false}; // Split operation status
    std::string current_mode; // Current operating mode
    int32_t rit_offset{0}; // RIT offset in Hz
    static constexpr auto TAG = "CAT_PARSER";

    // static CatParser *instance_; // Removed for pure Meyers' singleton
    std::atomic<bool> shutdown_requested;
    static constexpr int SERIAL_DATA_TIMEOUT_S = 5;  // 5 second timeout
    std::chrono::steady_clock::time_point last_serial_data_time;
    bool radio_provides_auto_updates_{false}; // True if Kenwood AI (Auto Information) from the radio is ON
};

// Legacy C-style interface
inline esp_err_t cat_parser_init() { return CatParser::instance().init(); }

inline esp_err_t cat_parser_process_command(const char *command) {
    return CatParser::instance().process_command(command);
}

inline esp_err_t cat_parser_update_config() { return CatParser::instance().update_config(); }
inline uint32_t cat_parser_get_frequency() { return CatParser::instance().get_frequency(); }
inline uint32_t cat_parser_get_transmit() { return CatParser::instance().is_transmitting(); }
inline void cat_parser_set_transmit(const bool transmitting) { CatParser::instance().set_transmitting(transmitting); }
inline esp_err_t cat_parser_set_frequency(const uint32_t frequency) { return CatParser::instance().handle_frequency_change(frequency); }

#endif // CAT_PARSER_H
