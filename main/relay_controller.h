#ifndef RELAY_CONTROLLER_H
#define RELAY_CONTROLLER_H

#include "esp_err.h"
#include "tcp_client.h"
#include <cstdint>
#include <map>
#include <chrono>
#include <memory>
#include <atomic>

struct RelayChangeRequest {
    int relay_id;
    int band_number;

    explicit RelayChangeRequest(const int rid = 0, const int band = -1) : relay_id(rid), band_number(band) {
    }

    bool operator==(const RelayChangeRequest &other) const {
        return relay_id == other.relay_id && band_number == other.band_number;
    }

    bool operator!=(const RelayChangeRequest &other) const {
        return !(*this == other);
    }
};

class RelayController {
public:
    static constexpr int NUM_RELAYS = 16;
    static constexpr int COOLDOWN_PERIOD_MS = 50;
    static constexpr size_t MAX_RESPONSE_PARTS = 5;

    RelayController();

    ~RelayController();

    esp_err_t init();

    esp_err_t update_relay_states();

    esp_err_t turn_off_all_relays();

    esp_err_t set_relay(int relay_id, bool state);

    bool get_relay_state(int relay_id) const;

    [[nodiscard]] std::map<int, bool> get_all_relay_states() const;

    int get_currently_selected_relay() const { return currently_selected_relay_; }

    esp_err_t update_all_relay_states();

    esp_err_t set_relay_for_antenna(int relay_id, int band_number);

    esp_err_t turn_off_all_relays_except(int relay_to_keep_on);

    int get_last_selected_relay_for_band(int band_number) const;

    bool is_correct_relay_set(int band_number) const;

    esp_err_t update_tcp_settings(const std::string &host, uint16_t port);

    void set_tcp_host(const std::string &host);

    void set_tcp_port(uint16_t port);

    std::string get_tcp_host() const;

    uint16_t get_tcp_port() const;

    static void tcp_task(void *pvParameters);

private:
    static constexpr auto CMD_TERMINATOR = "OK";
    static constexpr auto CMD_DELIMITER = ",";
    static constexpr size_t NUM_DATA_BYTES = 2; // For 16 outputs (D1,D0), this may differ  w/ other models

    std::array<std::string_view, MAX_RESPONSE_PARTS> response_parts_;

    void log_network_diagnostics() const;

    std::unique_ptr<TCPClient> tcp_client;
    std::map<int, bool> relay_states_;
    std::map<int, int> last_selected_relay_for_band_;
    int currently_selected_relay_;
    std::chrono::steady_clock::time_point last_band_change_time_;
    std::string tcp_host_;
    uint16_t tcp_port_;
    std::string last_command_;
    std::mutex command_mutex_;

    bool should_delay() const;

    esp_err_t execute_relay_change(int relay_id, int band_number);

    esp_err_t verify_relay_state(int expected_relay);

    esp_err_t send_command(const std::string &command,
                           const std::string &expected_response,
                           int timeout_ms = 500,
                           int max_retries = 2);

    esp_err_t parse_relay_state_response(const std::string &response);

    uint16_t relay_state_bitfield_;
    TaskHandle_t tcp_task_handle_;
    std::atomic<RelayChangeRequest> latest_request_;
    int last_band_number_;
};

#endif // RELAY_CONTROLLER_H
