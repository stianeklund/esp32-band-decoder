#pragma once

#include "esp_err.h"
#include <string>
#include <lwip/sockets.h>
#include <atomic>

class TCPClient {
public:
    TCPClient();

    ~TCPClient();

    // Initialize connection to server
    esp_err_t init(const char *host, uint16_t port);

    // Send a message to the server
    esp_err_t send_message(const std::string &message);

    // Receive a message from the server
    esp_err_t receive_message(char *message, size_t message_size, int timeout_ms);

    // Close the connection
    void close();

    // Check if connected and reconnect if necessary
    esp_err_t ensure_connected();

    // Check current connection status
    bool check_connection_status();

    // Get socket descriptor
    int get_sock() const;

    // Configure connection timeouts
    void set_timeouts(int connect_timeout_sec = 5,
                      int keepalive_idle = 5,
                      int keepalive_interval = 3,
                      int keepalive_count = 3);

    esp_err_t verify_connection();

private:
    int sock;
    std::string host;
    uint16_t port;
    sockaddr_in dest_addr;
    bool is_connected;
    int connect_timeout_sec_;
    int keepalive_idle_;
    int keepalive_interval_;
    int keepalive_count_;

    // Synchronization and timing
    std::mutex send_mutex_;
    TickType_t last_reconnect_attempt_;
    // Constants
    static constexpr int RECONNECT_COOLDOWN_MS = 5000;
    static constexpr int DEFAULT_TIMEOUT_MS = 1000;

    std::string send_buffer_; // Reusable send buffer

    esp_err_t connect_to_server();
};
