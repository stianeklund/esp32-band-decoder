#ifndef CAT_PARSER_H
#define CAT_PARSER_H

#include "esp_err.h"
#include "esp_log.h"
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

// CatParser is the protocol-neutral base: it owns the UART engine, ';'-framing,
// band-decode/antenna handoff, the TX-safety chokepoint (set_transmitting), the
// transverter state/math, and every getter the rest of the firmware consumes.
// Concrete radio protocols derive from it (see KenwoodCat / YaesuCat) and supply
// only the wire-format parsing: their own command map + dispatch_one_command()
// and, optionally, poll_for_updates() for protocol-specific auto-info/polling.
// Consumers keep using CatParser::instance() unchanged; the factory (in
// cat_parser_factory.cpp) picks the subclass from config at first init.
class CatParser {
public:
    CatParser();

    virtual ~CatParser();

    esp_err_t init();

    esp_err_t process_command(const char *command_cstr);
    void process_serial_data(const uint8_t* data, size_t len);
    void clear_serial_data();

    esp_err_t update_config();

    uint32_t get_frequency() const { return current_frequency; }
    bool is_transmitting() const { return transmitting; }
    void set_transmitting(bool tx_state); // Definition moved to .cpp
    bool is_rit_on() const { return rit_on; }
    bool is_xit_on() const { return xit_on; }
    bool is_split_on() const { return split_on; }
    const char *get_mode() const { return current_mode.c_str(); }
    int32_t get_rit_offset() const { return rit_offset; }

    // Transverter (XVTR) state, driven by the radio's EX056 answer and XO offset.
    bool is_transverter_active() const { return transverter_active.load(); }
    int32_t get_transverter_offset_hz() const { return transverter_offset_hz.load(); }
    // Frequency to display/report: the corrected on-air frequency (IF + offset) when
    // transverter mode is active and transverter frequency display is enabled in the
    // config, otherwise the raw IF frequency the radio reports.
    uint32_t get_display_frequency() const;

    esp_err_t handle_frequency_change(uint32_t frequency);

    void handle_frequency_update(uint32_t frequency);

    // Send commands to the radio via UART
    esp_err_t send_to_radio(const char* command);

    // Request async AI/auto-info probe (non-blocking, executed by UART task via
    // poll_for_updates()). Kept protocol-neutral so callers don't care which
    // radio protocol is active.
    void request_ai_probe() { ai_probe_requested_.store(true); }
    bool is_ai_probe_pending() const { return ai_probe_requested_.load(); }

    // Returns the active parser; the concrete subclass is chosen from config the
    // first time this is called and lives for the whole session (see the
    // load-bearing identity invariant in cat_parser_factory.cpp).
    static CatParser &instance();

protected:
    // --- Protocol hooks implemented by subclasses ---
    // Parse one command (already stripped of its trailing ';'). Subclasses route
    // through dispatch_lookup() with their own typed command map.
    virtual esp_err_t dispatch_one_command(std::string_view command_view) = 0;
    // Called from the UART task idle branch; subclasses use it for protocol-specific
    // auto-info handshakes / polling. Default: nothing to poll.
    virtual void poll_for_updates() {}

    // Shared dispatch: look up a 2-char command code in a subclass-owned map of
    // member-function pointers, invoke it, and keep the shared logging + valid-command
    // bookkeeping in one place. Zero-heap (no std::function).
    template <class D>
    esp_err_t dispatch_lookup(const std::unordered_map<uint16_t, esp_err_t (D::*)(std::string_view)>& handlers,
                              D* self, std::string_view command_view) {
        if (command_view.length() < 2) { // Command must be at least 2 chars
            if (!command_view.empty()) {
                ESP_LOGD(TAG, "Short command received (length < 2): %.*s",
                         static_cast<int>(command_view.length()), command_view.data());
            }
            return ESP_OK;
        }

        const uint16_t cmd_code = static_cast<uint16_t>(command_view[0]) << 8 | command_view[1];
        const std::string_view cmd_payload = command_view.length() > 2 ? command_view.substr(2) : std::string_view();

        const auto handler_it = handlers.find(cmd_code);
        if (handler_it != handlers.end()) {
            if (handler_it->second) { // Check if the function pointer is not null
                const esp_err_t ret = (self->*(handler_it->second))(cmd_payload);
                if (ret != ESP_OK) {
                    ESP_LOGW(TAG, "Command handler for '%.*s' returned error %s",
                             static_cast<int>(command_view.substr(0, 2).length()), command_view.substr(0, 2).data(),
                             esp_err_to_name(ret));
                } else {
                    last_valid_command_time = std::chrono::steady_clock::now();
                    ESP_LOGV(TAG, "Successfully processed command: %.*s",
                             static_cast<int>(command_view.length()), command_view.data());
                }
                return ret;
            }
        } else {
            ESP_LOGV(TAG, "No handler for command prefix: '%.*s'",
                     static_cast<int>(command_view.substr(0, 2).length()), command_view.substr(0, 2).data());
        }
        return ESP_OK; // No handler found or handler was null
    }

    void uart_task();
    static void uart0_to_uart2_task();

    int get_band_index(uint32_t freq) const;
    bool is_same_band(uint32_t freq1, uint32_t freq2) const;

    // Apply a transverter active/inactive transition (offset query, port off, broadcast).
    void set_transverter_active(bool active);
    // Broadcast the current transverter state to WebSocket clients (if running).
    void broadcast_transverter_state();
    // Convert a radio-reported IF frequency to the on-air frequency using the XO offset.
    uint32_t transverter_rf_from_if(uint32_t if_freq) const;

    esp_err_t process_command(std::string_view commands_str_with_semicolons); // ';'-framing → dispatch

    static void uart_task_trampoline(void *arg);

    uint32_t get_current_frequency() const { return current_frequency; }

    QueueHandle_t uart2_queue;
    antenna_switch_config_t current_config{};
    uint32_t current_frequency{0};  // Frequency of the currently active VFO
    uint32_t vfo_a_frequency{0};    // VFO A frequency
    uint32_t vfo_b_frequency{0};    // VFO B frequency
    uint8_t active_vfo{0};          // 0 = VFO A, 1 = VFO B (from IF P10 field)
    int current_band_index{-1}; // Cache for current frequency's band
    std::atomic<bool> transmitting{false}; // Tracks if radio is transmitting (atomic for thread safety)
    bool rit_on{false}; // RIT status
    bool xit_on{false}; // XIT status
    bool split_on{false}; // Split operation status
    std::string current_mode; // Current operating mode
    int32_t rit_offset{0}; // RIT offset in Hz

    // Transverter state (atomic: written by UART task, read by web/WS handlers)
    std::atomic<bool> transverter_active{false};    // Radio reported EX056=1
    std::atomic<int32_t> transverter_offset_hz{0};  // Offset from XO command (Hz)
    std::atomic<uint8_t> transverter_minus_dir{0};  // XO direction: 0=plus, 1=minus
    static constexpr auto TAG = "CAT_PARSER";

    std::atomic<bool> shutdown_requested;
    std::atomic<bool> ai_probe_requested_{false};  // Request async AI probe from UART task
    static constexpr int SERIAL_DATA_TIMEOUT_S = 5;  // 5 second timeout
    std::chrono::steady_clock::time_point last_serial_data_time;
    std::chrono::steady_clock::time_point last_valid_command_time;  // Track when we last parsed a valid CAT command
    std::chrono::steady_clock::time_point last_ai_query_time;  // Track when we last sent an AI query
    static constexpr int AI_QUERY_INTERVAL_S = 5;  // Retry AI query every 5 seconds if no response
};

// Legacy C-style interface
inline esp_err_t cat_parser_init() { return CatParser::instance().init(); }

inline esp_err_t cat_parser_process_command(const char *command) {
    return CatParser::instance().process_command(command);
}

inline esp_err_t cat_parser_update_config() { return CatParser::instance().update_config(); }
inline uint32_t cat_parser_get_frequency() { return CatParser::instance().get_frequency(); }
inline bool cat_parser_get_transmit() { return CatParser::instance().is_transmitting(); } // Return type bool
inline void cat_parser_set_transmit(const bool transmitting) { CatParser::instance().set_transmitting(transmitting); }
inline esp_err_t cat_parser_set_frequency(const uint32_t frequency) { return CatParser::instance().handle_frequency_change(frequency); }
inline bool cat_parser_is_transverter_active() { return CatParser::instance().is_transverter_active(); }
inline int32_t cat_parser_get_transverter_offset_hz() { return CatParser::instance().get_transverter_offset_hz(); }
inline uint32_t cat_parser_get_display_frequency() { return CatParser::instance().get_display_frequency(); }

#endif // CAT_PARSER_H
