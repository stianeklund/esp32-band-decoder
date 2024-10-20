#include "udp_client.h"
#include "esp_log.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <arpa/inet.h>

static const char* TAG = "UDP_CLIENT";

UDPClient::UDPClient() : sock(-1) {}

UDPClient::~UDPClient() {
    close();
}

esp_err_t UDPClient::init(const char* host, uint16_t port) {
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        return ESP_FAIL;
    }

    dest_addr.sin_addr.s_addr = inet_addr(host);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(port);

    return ESP_OK;
}

esp_err_t UDPClient::send_message(const std::string& message) {
    int err = sendto(sock, message.c_str(), message.length(), 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err < 0) {
        ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t UDPClient::receive_message(std::string& message, int timeout_ms) {
    char rx_buffer[128];
    struct sockaddr_in source_addr;
    socklen_t socklen = sizeof(source_addr);

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0, (struct sockaddr *)&source_addr, &socklen);

    if (len < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            ESP_LOGW(TAG, "Receive timeout");
            return ESP_ERR_TIMEOUT;
        }
        ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
        return ESP_FAIL;
    } else {
        rx_buffer[len] = 0; // Null-terminate whatever we received and treat like a string
        message = std::string(rx_buffer);
        return ESP_OK;
    }
}

void UDPClient::close() {
    if (sock != -1) {
        ::close(sock);
        sock = -1;
    }
}
