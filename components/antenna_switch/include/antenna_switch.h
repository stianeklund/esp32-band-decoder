#ifndef ANTENNA_SWITCH_H
#define ANTENNA_SWITCH_H

#include "esp_err.h"
#include <cstdint>
#include <atomic> // Required for std::atomic
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h" // For TimerHandle_t
#include "freertos/semphr.h" // For SemaphoreHandle_t
#include "esp_timer.h" // For esp_timer_handle_t

// don't #include relay_controller.h here to avoid circular dependency;
// just forward-declare RelayController for the C++ API
class RelayController;

// Constants and structs

// Enum for Radio Operation Mode - must be defined before antenna_switch_config_t
typedef enum {
    RADIO_OP_MODE_SINGLE_A,         // Only Radio A is active. Radio B controls are disabled.
    RADIO_OP_MODE_ALTERNATING_AB,   // Radio A or Radio B can be active, but not simultaneously. Selecting one deactivates the other.
    RADIO_OP_MODE_CONCURRENT_AB     // Radio A and Radio B can be active simultaneously on different antennas.
} radio_operation_mode_t;

// Enum for CAT protocol family. Selected at runtime and applied on reboot (the
// CAT parser object is built exactly once per session; see cat_parser_factory.cpp).
typedef enum {
    RADIO_PROTOCOL_KENWOOD, // Kenwood TS-590SG and compatible
    RADIO_PROTOCOL_YAESU,   // Modern Yaesu (FT-891/FT-991A/FTDX-10/101)
} radio_protocol_t;

// Enum for Antenna Switch operation mode
// TODO implement whether or not we should rely on polling or AI2 for updates
typedef enum {
    AUTOMATIC, // Automatically selects antenna port based on band selection / setup
    MANUAL,    // Requires manual selection
} switch_operation_mode_t;

#define MAX_BANDS 10
#define MAX_ANTENNA_PORTS 8

typedef struct band_config {
    char description[32];
    uint32_t start_freq;
    uint32_t end_freq;
    bool antenna_ports[MAX_ANTENNA_PORTS];
    uint8_t rx_antenna_port;  // 1-based relay ID for separate RX antenna, 0 = use TX antenna
} band_config_t;

// Struct for the base configuration data stored in the "config_base" NVS blob
typedef struct {
    bool auto_mode;
    bool ai_mode;                       // Enable Auto Information (AI2) mode
    bool allow_concurrent_data_sources; // This field is part of the base config
    bool websocket_enabled;             // Enable/disable WebSocket server
    bool rx_antenna_enabled;            // Enable separate RX antenna feature
    uint8_t num_bands;                  // Actual number of bands used, up to MAX_BANDS
    uint8_t num_antenna_ports;          // Actual number of antenna ports used, up to MAX_ANTENNA_PORTS
    radio_operation_mode_t radio_operation_mode;
    uint8_t last_used_antenna[2][MAX_BANDS]; // Stores 1-based relay_id
} base_nvs_config_data_t;

typedef struct antenna_switch_config {
    // Fields managed by base_nvs_config_data_t for NVS persistence
    bool auto_mode;
    bool ai_mode;                       // Enable Auto Information (AI2) mode
    bool allow_concurrent_data_sources;
    bool rx_antenna_enabled;            // Enable separate RX antenna feature
    uint8_t num_bands;
    uint8_t num_antenna_ports;
    radio_operation_mode_t radio_operation_mode;
    uint8_t last_used_antenna[2][MAX_BANDS];

    // Bands - stored separately in NVS as individual blobs per band/radio
    band_config_t bands[2][MAX_BANDS];

    // Other fields, stored individually or in other specific blobs in NVS
    int uart_baud_rate;
    uint8_t uart_parity;
    uint8_t uart_stop_bits;
    uint8_t uart_flow_ctrl;
    int8_t uart_tx_pin;  // GPIO pin for UART TX
    int8_t uart_rx_pin;  // GPIO pin for UART RX

    // CAT protocol family (Kenwood/Yaesu). Persisted as its own NVS key
    // ("radio_proto"), NOT in base_nvs_config_data_t, so adding it never changes
    // that blob's sizeof() and never trips the size-mismatch config wipe.
    radio_protocol_t radio_protocol;

    // PTT input configuration
    int ptt_input_radio_a;      // KC868 input number (0-15) for Radio A PTT, -1 if disabled
    bool ptt_input_radio_a_active_high; // True if PTT active is high, false if active low
    int ptt_input_radio_b;      // KC868 input number (0-15) for Radio B PTT, -1 if disabled (for future use)
    bool ptt_input_radio_b_active_high; // True if PTT active is high, false if active low (for future use)

    bool mqtt_enabled;
    // bool allow_concurrent_data_sources; // This is now part of the block above, managed by base_nvs_config_data_t
    bool websocket_enabled;                // Enable/disable WebSocket server
    bool interlock_auto_resolves_conflict; // Renamed from interlock_enabled
    bool auto_restore_on_conflict_resolution; // New setting
    char mqtt_broker[64];
    uint16_t mqtt_port;
    char mqtt_rig_id[16];
    char mqtt_username[32];
    char mqtt_password[32];
    char mqtt_client_id[32];
    char mqtt_topic[64];
    // radio_operation_mode_t radio_operation_mode; // This is now part of the block above
    // uint8_t last_used_antenna[2][MAX_BANDS]; // This is now part of the block above
    char relay_names[16][32]; // Custom names for each relay (16 relays, 32 chars each)
    uint16_t radio_restore_delay_ms; // Delay in milliseconds for interlock relay restoration
    bool transverter_show_frequency; // Show corrected on-air frequency while transverter (XVTR) mode is active
} antenna_switch_config_t;

// Enum to identify Radio A or Radio B
enum class RadioID : uint8_t {
    A = 0,
    B = 1
};

// Now include config_cache.h after the struct is defined
#include "config_cache.h"

// AntennaSwitch singleton class
class AntennaSwitch : public ConfigCache {
public:
    // Singleton instance method
    static AntennaSwitch& instance();

    // Delete copy constructor and assignment operator
    AntennaSwitch(const AntennaSwitch&) = delete;
    AntennaSwitch& operator=(const AntennaSwitch&) = delete;

    // Core functionality
    esp_err_t init();
    esp_err_t set_config(const antenna_switch_config_t *config);
    esp_err_t get_config(antenna_switch_config_t *config);
    antenna_switch_config_t get_config_ref() const;
    esp_err_t set_frequency(uint32_t frequency);
    esp_err_t set_auto_mode(bool auto_mode);
    esp_err_t set_relay(int relay_id, bool state);
    esp_err_t get_relay_state(int relay_id, bool *state);
    esp_err_t restart();

    // Set the relay controller reference
    void set_relay_controller(RelayController* controller);

    // update_preference=false skips recording this relay as the band's "last used"
    // TX antenna. Used when auto-selecting the RX antenna, which must never become
    // the TX preference (the PTT RX->TX swap would then transmit on the RX antenna).
    esp_err_t set_relay_for_antenna(int relay_id, int band_number, RadioID radio, bool state, bool update_preference = true);

    esp_err_t set_relay_for_antenna(const int relay_id, const int band_number, const bool state) {
        return set_relay_for_antenna(relay_id, band_number, RadioID::A, state);
    }

    // Direct Radio B control (non-band selection)
    esp_err_t set_relay_radio_b(int relay_id, bool state);

    // Get active relay for a specific radio
    int get_active_relay_for_radio(RadioID radio) const;

private:
    AntennaSwitch();
    ~AntennaSwitch();
    
    // Internal state for hardware PTT
    std::atomic<bool> hw_ptt_a_active_{false}; // HW input ptt signal for Radio A
    std::atomic<bool> hw_ptt_b_active_{false}; // HW input ptt signal for Radio B
    std::atomic<bool> cat_tx_a_active_{false}; // Stores CAT-reported TX state for Radio A
    std::atomic<bool> cat_tx_b_active_{false}; // Stores CAT-reported TX state for Radio B

    // PTT debounce: track when HW PTT went active to require sustained signal
    // before clearing CAT TX state (prevents glitches from causing mid-TX antenna switching)
    static constexpr int64_t PTT_DEBOUNCE_US = 50000;  // 50ms debounce
    int64_t hw_ptt_a_active_since_us_{0};  // Timestamp when HW PTT A went active
    int64_t hw_ptt_b_active_since_us_{0};  // Timestamp when HW PTT B went active

    // Pointer to relay controller
    RelayController* relay_controller_ = nullptr;
    int pre_tx_active_relay_radio_a_ = 0;         // Stores active relay for A if B starts TX
    mutable int pre_tx_active_relay_radio_b_ = 0; // Stores active relay for B if A starts TX
    int auto_resolved_conflict_prev_a_relay_ = 0; // Stores Radio A's relay if turned off by B due to auto-resolved port conflict
    int auto_resolved_conflict_prev_b_relay_ = 0; // Stores Radio B's relay if turned off by A due to auto-resolved port conflict

    esp_err_t update_last_used_antenna_preference(int activated_relay_id, RadioID radio_of_activated_relay, int band_idx_for_preference);

    // TX-start safety helpers. These run WITHOUT interlock_mutex_ so PTT
    // protection is never blocked by restore-state bookkeeping (see 0.1).
    // deenergize_other_radio_for_tx returns the relay it turned off (0 if none).
    int deenergize_other_radio_for_tx(RadioID transmitting_radio, const antenna_switch_config_t& config);
    void apply_radio_a_rx_to_tx_swap(const antenna_switch_config_t& config);

    void attempt_restore_auto_resolved_radio_a_relay();
    void attempt_restore_auto_resolved_radio_b_relay();

    // Timers for delayed relay restoration (using esp_timer for sub-10ms precision)
    esp_timer_handle_t radio_b_restore_delay_timer_ = nullptr;
    esp_timer_handle_t radio_a_restore_delay_timer_ = nullptr;

    // Retry counters for timer rescheduling (prevent infinite loops)
    static constexpr int MAX_TIMER_RETRIES = 5;
    int radio_a_restore_retry_count_ = 0;
    int radio_b_restore_retry_count_ = 0;

    SemaphoreHandle_t interlock_mutex_ = nullptr;

    static void radio_b_restore_timer_callback(void* arg);
    static void radio_a_restore_timer_callback(void* arg);

public:
    // Method to get combined TX state, considering HW PTT and CAT parser
    bool is_radio_a_transmitting_effective() const;
    bool is_radio_b_transmitting_effective() const;

    // Callbacks for InputManager to report PTT state changes
    void on_hw_ptt_a_state_change(bool active);
    void on_hw_ptt_b_state_change(bool active);

    // Callback for CatParser to report TX state changes (primarily for Radio A)
    void on_cat_tx_a_state_change(bool active);
    void on_radio_a_tx_start();
    void on_radio_a_tx_stop();
    void on_radio_b_tx_start();
    void on_radio_b_tx_stop();
};

#endif // ANTENNA_SWITCH_H
