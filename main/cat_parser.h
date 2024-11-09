#ifndef CAT_PARSER_H
#define CAT_PARSER_H

#include "esp_err.h"
#include "driver/uart.h"
#include "antenna_switch.h"
#include <string_view>
#include <unordered_map>
#include <atomic>
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

    esp_err_t process_command(const char *command);

    esp_err_t update_config();

    uint32_t get_frequency() const { return current_frequency; }
    bool is_transmitting() const { return transmitting; }
    bool is_rit_on() const { return rit_on; }
    bool is_xit_on() const { return xit_on; }
    bool is_split_on() const { return split_on; }
    const char *get_mode() const { return current_mode.c_str(); }
    int32_t get_rit_offset() const { return rit_offset; }

    esp_err_t handle_frequency_change(uint32_t frequency);

    // Legacy C-style interface for backward compatibility
    static CatParser &instance();

private:
    using CommandHandler = esp_err_t (CatParser::*)(std::string_view);

    void uart_task();

    void uart0_to_uart2_task() const;

    int get_band_index(uint32_t freq) const;

    bool is_same_band(uint32_t freq1, uint32_t freq2) const;

    esp_err_t process_fa_command(std::string_view command);

    esp_err_t process_if_command(std::string_view command);

    esp_err_t process_ap_command(std::string_view command);

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

    static CatParser *instance_;
    std::atomic<bool> shutdown_requested;
};

// Legacy C-style interface
inline esp_err_t cat_parser_init() { return CatParser::instance().init(); }

inline esp_err_t cat_parser_process_command(const char *command) {
    return CatParser::instance().process_command(command);
}

inline esp_err_t cat_parser_update_config() { return CatParser::instance().update_config(); }
inline uint32_t cat_parser_get_frequency() { return CatParser::instance().get_frequency(); }

#endif // CAT_PARSER_H
