#pragma once

#include <string>
#include "esp_err.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

class UDPClient {
public:
    UDPClient();
    ~UDPClient();

    esp_err_t init(const char* host, uint16_t port);
    esp_err_t send_message(const std::string& message);
    esp_err_t receive_message(std::string& message, int timeout_ms);
    void close();

private:
    int sock;
    struct sockaddr_in dest_addr;
};
