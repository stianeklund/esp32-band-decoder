#ifndef RELAY_CONTROLLER_H
#define RELAY_CONTROLLER_H

#include "esp_err.h"
#include "udp_client.h"
#include <cstdint>
#include <vector>
#include <map>
#include <chrono>
#include <memory>

class RelayController {
public:
    static constexpr int NUM_RELAYS = 16;
    static constexpr int COOLDOWN_PERIOD_MS = 100;

    RelayController();
    ~RelayController();

    esp_err_t init();
    esp_err_t update_relay_states();
    esp_err_t turn_off_all_relays();
    esp_err_t set_relay(int relay_id, bool state);
    bool get_relay_state(int relay_id) const;
    std::map<int, bool> get_all_relay_states() const;
    esp_err_t update_all_relay_states();
    esp_err_t set_relay_for_antenna(int relay_id, int band_number);
    esp_err_t turn_off_all_relays_except(int relay_to_keep_on);
    int get_last_selected_relay_for_band(int band_number) const;
    bool is_correct_relay_set(int band_number) const;
    esp_err_t update_udp_settings(const std::string &host, uint16_t port);
    void set_udp_host(const std::string& host);
    void set_udp_port(uint16_t port);
    std::string get_udp_host() const;
    uint16_t get_udp_port() const;

private:
    std::unique_ptr<UDPClient> udp_client;
    std::map<int, bool> relay_states_;
    std::map<int, int> last_selected_relay_for_band_;
    int currently_selected_relay_;
    std::chrono::steady_clock::time_point last_band_change_time_;
    std::string udp_host_;
    uint16_t udp_port_;

    bool should_delay() const;
    esp_err_t execute_relay_change(int relay_id, int band_number);
    esp_err_t send_command_and_validate_response(const std::string& command, const std::string& expected_response);
    bool send_command_with_retry(const std::string& command, const std::string& expectedResponsePattern);
};

#endif // RELAY_CONTROLLER_H
