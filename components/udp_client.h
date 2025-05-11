#pragma once

#include <string>
#include "esp_err.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

class UDPClient {
public:
    UDPClient();

    ~UDPClient();

    esp_err_t init(const char *host, uint16_t port);

    esp_err_t send_message(const std::string &message);

    esp_err_t receive_message(char *message, size_t message_size, int timeout_ms) const;

    int get_sock();
    void close();

private:
    int sock;
    struct sockaddr_in dest_addr{};

};
