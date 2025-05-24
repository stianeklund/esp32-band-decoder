#ifndef ANTENNA_SWITCH_H
#define ANTENNA_SWITCH_H

#include "esp_err.h"
#include <cstdint>
#include <atomic> // Required for std::atomic
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h" // For TimerHandle_t
#include "freertos/semphr.h" // For SemaphoreHandle_t

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
} band_config_t;

// Struct for the base configuration data stored in the "config_base" NVS blob
typedef struct {
    bool auto_mode;
    bool allow_concurrent_data_sources; // This field is part of the base config
    uint8_t num_bands;                  // Actual number of bands used, up to MAX_BANDS
    uint8_t num_antenna_ports;          // Actual number of antenna ports used, up to MAX_ANTENNA_PORTS
    radio_operation_mode_t radio_operation_mode; 
    uint8_t last_used_antenna[2][MAX_BANDS]; // Stores 1-based relay_id
} base_nvs_config_data_t;

typedef struct {
    // Fields managed by base_nvs_config_data_t for NVS persistence
    bool auto_mode;
    bool allow_concurrent_data_sources; 
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

    // PTT input configuration
    int ptt_input_radio_a;      // KC868-A16 input number (0-15) for Radio A PTT, -1 if disabled
    bool ptt_input_radio_a_active_high; // True if PTT active is high, false if active low
    int ptt_input_radio_b;      // KC868-A16 input number (0-15) for Radio B PTT, -1 if disabled (for future use)
    bool ptt_input_radio_b_active_high; // True if PTT active is high, false if active low (for future use)

    bool mqtt_enabled;
    // bool allow_concurrent_data_sources; // This is now part of the block above, managed by base_nvs_config_data_t
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
} antenna_switch_config_t;

// Enum to identify Radio A or Radio B
enum class RadioID : uint8_t {
    A = 0,
    B = 1
};

// AntennaSwitch singleton class
class AntennaSwitch {
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
    const antenna_switch_config_t& get_config_ref() const;
    esp_err_t set_frequency(uint32_t frequency);
    esp_err_t set_auto_mode(bool auto_mode);
    esp_err_t set_relay(int relay_id, bool state);
    esp_err_t get_relay_state(int relay_id, bool *state);
    esp_err_t restart();

    // Set the relay controller reference
    void set_relay_controller(RelayController* controller);

    esp_err_t set_relay_for_antenna(int relay_id, int band_number, RadioID radio, bool state);

    esp_err_t set_relay_for_antenna(const int relay_id, const int band_number, const bool state) {
        return set_relay_for_antenna(relay_id, band_number, RadioID::A, state);
    }

    // Direct Radio B control (non-band selection)
    esp_err_t set_relay_radio_b(int relay_id, bool state);

private:
    // Private constructor for singleton
    AntennaSwitch(); // Will be defined in .cpp to initialize timers/mutex
    
    // Internal state for hardware PTT
    std::atomic<bool> hw_ptt_a_active_{false};
    std::atomic<bool> hw_ptt_b_active_{false};

    // Pointer to relay controller
    RelayController* relay_controller_ = nullptr;
    int pre_tx_active_relay_radio_a_ = 0; // Stores active relay for A if B starts TX
    int pre_tx_active_relay_radio_b_ = 0; // Stores active relay for B if A starts TX
    int auto_resolved_conflict_prev_a_relay_ = 0; // Stores Radio A's relay if turned off by B due to auto-resolved port conflict
    int auto_resolved_conflict_prev_b_relay_ = 0; // Stores Radio B's relay if turned off by A due to auto-resolved port conflict


    // Helper to get active relay for a specific radio
    int get_active_relay_for_radio(RadioID radio) const;

    // Helper to update last used antenna preference
    esp_err_t update_last_used_antenna_preference(int activated_relay_id, RadioID radio_of_activated_relay);

    // Helper to attempt restoration of relays deselected by auto-resolved port conflicts
    void attempt_restore_auto_resolved_radio_a_relay();
    void attempt_restore_auto_resolved_radio_b_relay();

    // Timers for delayed relay restoration
    TimerHandle_t radio_b_restore_delay_timer_ = nullptr;
    TimerHandle_t radio_a_restore_delay_timer_ = nullptr;

    // Mutex for interlock logic
    SemaphoreHandle_t interlock_mutex_ = nullptr;

    // Helper methods for timer callbacks (static)
    static void radio_b_restore_timer_callback(TimerHandle_t xTimer);
    static void radio_a_restore_timer_callback(TimerHandle_t xTimer);

public:
    // Method to get combined TX state, considering HW PTT and CAT parser
    bool is_radio_a_transmitting_effective() const;
    bool is_radio_b_transmitting_effective() const;

    // Callbacks for InputManager to report PTT state changes
    void on_hw_ptt_a_state_change(bool active);
    void on_hw_ptt_b_state_change(bool active);

    // ... existing public members ...
    void on_radio_a_tx_start();
    void on_radio_a_tx_stop();
    void on_radio_b_tx_start(); // For future use when Radio B TX state is known
    void on_radio_b_tx_stop();  // For future use
};

#endif // ANTENNA_SWITCH_H
