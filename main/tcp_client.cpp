#include "tcp_client.h"
#include "esp_log.h"
#include "esp_err.h"
#include "lwip/sockets.h"
#include <arpa/inet.h>
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include <array>

static auto TAG = "TCP_CLIENT";

TCPClient::TCPClient() : sock(-1), port(0), dest_addr(), is_connected(false),
                         connect_timeout_sec_(5), keepalive_idle_(5),
                         keepalive_interval_(3), keepalive_count_(3), last_reconnect_attempt_(0) {
}

TCPClient::~TCPClient() {
    close();
}

int TCPClient::get_sock() const {
    return sock;
}

esp_err_t TCPClient::init(const char *host, const uint16_t port) {
    if (sock >= 0) {
        close();
    }

    this->host = host;
    this->port = port;

    // Add initial connection retry with backoff
    constexpr int MAX_INIT_RETRIES = 3;

    for (int retry = 0; retry < MAX_INIT_RETRIES; retry++) {
        if (retry > 0) {
            constexpr int INITIAL_RETRY_DELAY_MS = 200;
            ESP_LOGW(TAG, "Retrying connection attempt %d/%d after delay", retry + 1, MAX_INIT_RETRIES);
            vTaskDelay(pdMS_TO_TICKS(INITIAL_RETRY_DELAY_MS * (1 << retry)));
        }

        if (const esp_err_t ret = connect_to_server(); ret == ESP_OK) {
            // Successfully connected
            return ESP_OK;
        }

        if (errno == ECONNRESET) {
            ESP_LOGW(TAG, "Connection reset by peer, will retry");
            continue;
        }

        // For other errors, close socket and retry
        close();
    }

    ESP_LOGE(TAG, "Failed to establish connection after %d attempts", MAX_INIT_RETRIES);
    return ESP_FAIL;
}

esp_err_t TCPClient::connect_to_server() {
    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        return ESP_FAIL;
    }

    // Set socket options first (ESP-IDF pattern)
    constexpr int flag = 1;
    if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0) {
        ESP_LOGW(TAG, "Failed to set TCP_NODELAY: errno %d", errno);
    }

    // Configure keepalive with ESP-IDF recommended values
    constexpr int keepalive = 1;
    constexpr int keepidle = 5; // Start probing after 5 seconds of idle
    constexpr int keepintvl = 3; // Probe interval of 3 seconds
    constexpr int keepcnt = 3; // Drop connection after 3 failed probes

    if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive)) < 0) {
        ESP_LOGW(TAG, "Failed to set SO_KEEPALIVE: errno %d", errno);
    }
    if (setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle)) < 0) {
        ESP_LOGW(TAG, "Failed to set TCP_KEEPIDLE: errno %d", errno);
    }
    if (setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl)) < 0) {
        ESP_LOGW(TAG, "Failed to set TCP_KEEPINTVL: errno %d", errno);
    }
    if (setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt)) < 0) {
        ESP_LOGW(TAG, "Failed to set TCP_KEEPCNT: errno %d", errno);
    }

    // Set initial timeouts for connection
    timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        ESP_LOGW(TAG, "Failed to set SO_RCVTIMEO: errno %d", errno);
    }
    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
        ESP_LOGW(TAG, "Failed to set SO_SNDTIMEO: errno %d", errno);
    }

    dest_addr.sin_addr.s_addr = inet_addr(host.c_str());
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(port);

    // Non-blocking connection with timeout (ESP-IDF pattern)
    const int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    int res = connect(sock, reinterpret_cast<struct sockaddr *>(&dest_addr), sizeof(dest_addr));
    if (res == 0) {
        is_connected = true;
        fcntl(sock, F_SETFL, flags); // Set back to blocking
        ESP_LOGI(TAG, "Connected immediately to %s:%d", host.c_str(), port);
        return verify_connection();
    }

    if (errno != EINPROGRESS) {
        ESP_LOGE(TAG, "Socket connect failed: errno %d", errno);
        close();
        return ESP_FAIL;
    }

    // Wait for connection with timeout
    fd_set write_fds;
    FD_ZERO(&write_fds);
    FD_SET(sock, &write_fds);

    res = select(sock + 1, nullptr, &write_fds, nullptr, &timeout);
    if (res <= 0) {
        ESP_LOGE(TAG, "Connection %s", res == 0 ? "timed out" : "select failed");
        close();
        return res == 0 ? ESP_ERR_TIMEOUT : ESP_FAIL;
    }

    // Verify connection success
    int error;
    socklen_t len = sizeof(error);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
        ESP_LOGE(TAG, "Connection failed: errno %d", error ? error : errno);
        close();
        return ESP_FAIL;
    }

    // Set back to blocking mode
    fcntl(sock, F_SETFL, flags);

    ESP_LOGI(TAG, "Successfully connected to %s:%d", host.c_str(), port);
    is_connected = true;

    return verify_connection();
}

esp_err_t TCPClient::ensure_connected() {
    if (!is_connected || !check_connection_status()) {
        const TickType_t now = xTaskGetTickCount();
        if ((now - last_reconnect_attempt_) < pdMS_TO_TICKS(RECONNECT_COOLDOWN_MS)) {
            return ESP_ERR_INVALID_STATE;
        }

        ESP_LOGW(TAG, "Connection lost, attempting to reconnect...");
        close();
        last_reconnect_attempt_ = now;

        constexpr int MAX_RETRIES = 3;
        for (int i = 0; i < MAX_RETRIES; i++) {
            if (i > 0) {
                vTaskDelay(pdMS_TO_TICKS(1000 * (1 << i))); // Exponential backoff
            }

            const esp_err_t ret = connect_to_server();
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "Reconnection successful on attempt %d", i + 1);
                return ESP_OK;
            }

            ESP_LOGW(TAG, "Reconnection attempt %d/%d failed: %s",
                     i + 1, MAX_RETRIES, esp_err_to_name(ret));
        }
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool TCPClient::check_connection_status() {
    if (!is_connected || sock < 0) {
        return false;
    }

    int error = 0;
    socklen_t len = sizeof(error);

    if (const int retval = getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len); retval != 0 || error != 0) {
        ESP_LOGW(TAG, "Connection check failed: %d (%s)", error, strerror(error));
        is_connected = false;
        return false;
    }

    return true;
}

esp_err_t TCPClient::send_message(const std::string &message) {
    std::lock_guard lock(send_mutex_);

    if (!is_connected) {
        ESP_LOGW(TAG, "Not connected, attempting reconnect...");
        if (const esp_err_t err = ensure_connected(); err != ESP_OK) {
            return err;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    size_t total_sent = 0;
    int retry_count = 0;
    constexpr int MAX_RETRIES = 2;

    while (total_sent < message.length() && retry_count < MAX_RETRIES) {
        const int written = send(sock, message.c_str() + total_sent,
                                 message.length() - total_sent, 0);
        if (written < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                ESP_LOGW(TAG, "Send would block, retrying after delay");
                vTaskDelay(pdMS_TO_TICKS(100));
                retry_count++;
                continue;
            }
            ESP_LOGE(TAG, "Send error: errno %d", errno);
            is_connected = false;
            return ESP_FAIL;
        }

        total_sent += written;
        if (total_sent < message.length()) {
            ESP_LOGW(TAG, "Partial send (%zu of %zu bytes), continuing",
                     total_sent, message.length());
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    if (total_sent == message.length()) {
        vTaskDelay(pdMS_TO_TICKS(50));
        ESP_LOGV(TAG, "Sent %zu bytes to %s:%d: %s",
                 total_sent, host.c_str(), port, message.c_str());
        return ESP_OK;
    }

    return ESP_FAIL;
}

esp_err_t TCPClient::receive_message(char *message, const size_t message_size, const int timeout_ms) {
    if (!message || message_size <= 1) {
        return ESP_ERR_INVALID_ARG;
    }

    const int64_t start_time = esp_timer_get_time() / 1000;
    ESP_LOGV(TAG, "Starting receive with %d ms timeout", timeout_ms);

    size_t total_received = 0;

    while (true) {
        const int64_t now = esp_timer_get_time() / 1000;
        const int elapsed_ms = static_cast<int>(now - start_time);
        // we can trip the watchdog here if we're on a band that's not supported by the config
        esp_task_wdt_reset();

        if (elapsed_ms >= timeout_ms) {
            ESP_LOGV(TAG, "Timeout after %d ms, message content: '%.*s'",
                     elapsed_ms, (int)total_received, message);
            return ESP_ERR_TIMEOUT;
        }

        const int remaining_ms = timeout_ms - elapsed_ms;
        ESP_LOGV(TAG, "Remaining time: %d ms", remaining_ms);

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);

        timeval tv{};
        const int select_timeout = std::min(remaining_ms, 250);
        tv.tv_sec = select_timeout / 1000;
        tv.tv_usec = (select_timeout % 1000) * 1000;


        if (const int select_result = select(sock + 1, &readfds, nullptr, nullptr, &tv); select_result > 0) {
            const int len = recv(sock, message + total_received, message_size - total_received - 1, 0);

            if (len < 0) {
                ESP_LOGE(TAG, "Receive error: %d (%s)", errno, strerror(errno));
                return ESP_FAIL;
            }

            if (len == 0) {
                ESP_LOGW(TAG, "Connection closed by peer");
                is_connected = false;
                return ESP_FAIL;
            }

            total_received += len;
            message[total_received] = '\0';

            std::string msg(message, total_received);
            if (msg.find(",OK") != std::string::npos ||
                (msg.find("RELAY-STATE-255") != std::string::npos && msg.find(",") != std::string::npos) ||
                (msg.find("RELAY-SET_ALL-255") != std::string::npos && msg.find(",") != std::string::npos)) {
                ESP_LOGV(TAG, "Received complete message (%zu bytes)", total_received);
                return ESP_OK;
            }

            if (total_received >= message_size - 1) {
                ESP_LOGW(TAG, "Message buffer full");
                return ESP_ERR_NO_MEM;
            }
        } else if (select_result == 0) {
            ESP_LOGV(TAG, "Select result is 0 but we have time remaining: %d", remaining_ms);
            return ESP_OK;
        } else {
            ESP_LOGE(TAG, "Select error: %d (%s)", errno, strerror(errno));
            return ESP_FAIL;
        }
    }
}


void TCPClient::close() {
    if (sock != -1) {
        ::close(sock);
        sock = -1;
    }
    is_connected = false;
}

void TCPClient::set_timeouts(const int connect_timeout_sec,
                             const int keepalive_idle,
                             const int keepalive_interval,
                             const int keepalive_count) {
    connect_timeout_sec_ = connect_timeout_sec;
    keepalive_idle_ = keepalive_idle;
    keepalive_interval_ = keepalive_interval;
    keepalive_count_ = keepalive_count;
}

esp_err_t TCPClient::verify_connection() {
    if (!is_connected) {
        return ESP_FAIL;
    }

    // Set shorter timeout for verification
    timeval verify_timeout;
    verify_timeout.tv_sec = 2; // 2 second timeout
    verify_timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &verify_timeout, sizeof(verify_timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &verify_timeout, sizeof(verify_timeout));

    // Send verification command
    const auto verify_cmd = "RELAY-STATE-255";
    if (const int written = send(sock, verify_cmd, strlen(verify_cmd), 0); written < 0) {
        ESP_LOGW(TAG, "Verification send failed: errno %d", errno);
        close();
        return ESP_FAIL;
    }

    // Wait a bit before receiving
    vTaskDelay(pdMS_TO_TICKS(100));

    // Receive and verify response
    char verify_resp[128];
    const int len = recv(sock, verify_resp, sizeof(verify_resp) - 1, 0);
    if (len < 0) {
        ESP_LOGW(TAG, "Verification receive failed: errno %d", errno);
        close();
        return ESP_FAIL;
    }

    verify_resp[len] = '\0';
    if (strstr(verify_resp, "OK") == nullptr) {
        ESP_LOGW(TAG, "Invalid verification response");
        close();
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Connection verified successfully");
    return ESP_OK;
}
