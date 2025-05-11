#include "udp_client.h"
#include "esp_log.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <arpa/inet.h>

static const char *TAG = "UDP_CLIENT";

UDPClient::UDPClient() : sock(-1) {}

UDPClient::~UDPClient() {
    close();
}

int UDPClient::get_sock() {
    return sock;
}

esp_err_t UDPClient::init(const char *host, uint16_t port) {
    if (sock >= 0) {
        close();
    }

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        return ESP_FAIL;
    }

    dest_addr.sin_addr.s_addr = inet_addr(host);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(port);

    // Set socket to non-blocking mode
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    return ESP_OK;
}

esp_err_t UDPClient::send_message(const std::string &message) {
    int err = sendto(sock, message.c_str(), message.length(), 0, (struct sockaddr *) &dest_addr, sizeof(dest_addr));
    ESP_LOGI(TAG, "Sending: %s to %s:%d", message.c_str(), inet_ntoa(dest_addr.sin_addr), ntohs(dest_addr.sin_port));
    if (err < 0) {
        ESP_LOGE(TAG, "Error occurred during sending: errno %d (%s)", errno, strerror(errno));
        return ESP_FAIL;
    }
    ESP_LOGV(TAG, "Message sent successfully, %d bytes", err);
    return ESP_OK;
}


esp_err_t UDPClient::receive_message(char* message, size_t message_size, int timeout_ms) const {
    char rx_buffer[128];
    struct sockaddr_in source_addr{};
    socklen_t socklen = sizeof(source_addr);

    // Set timeout for the socket
    struct timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        ESP_LOGE(TAG, "Error setting socket timeout: errno %d (%s)", errno, strerror(errno));
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGV(TAG, "Waiting for response (timeout: %d ms)...", timeout_ms);

    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(sock, &readfds);

    // Wait for the socket to become readable
    int activity = select(sock + 1, &readfds, nullptr, nullptr, &tv);

    if (activity < 0) {
        ESP_LOGE(TAG, "Select error: errno %d (%s)", errno, strerror(errno));
        return ESP_ERR_INVALID_STATE;
    } else if (activity == 0) {
        ESP_LOGW(TAG, "Receive timeout after %d ms", timeout_ms);
        return ESP_ERR_TIMEOUT;
    }

    // Receive data from the socket
    int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0, (struct sockaddr *) &source_addr, &socklen);

    if (len < 0) {
        ESP_LOGE(TAG, "recvfrom failed: errno %d (%s)", errno, strerror(errno));
        return ESP_ERR_INVALID_STATE;
    }

    // Check for buffer overflow
    if (len >= sizeof(rx_buffer)) {
        ESP_LOGE(TAG, "Received message too large for buffer.");
        return ESP_ERR_NO_MEM;
    }

    rx_buffer[len] = '\0';  // Null-terminate received data
    strncpy(message, rx_buffer, message_size - 1);
    message[message_size - 1] = '\0'; // Ensure null-termination

    ESP_LOGV(TAG, "Received %d bytes from %s:%d: %s", len, inet_ntoa(source_addr.sin_addr),
             ntohs(source_addr.sin_port), message);
    return ESP_OK;
}

void UDPClient::close() {
    if (sock != -1) {
        ::close(sock);
        sock = -1;
    }
}
