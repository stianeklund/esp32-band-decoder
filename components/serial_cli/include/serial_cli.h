#ifndef SERIAL_CLI_H
#define SERIAL_CLI_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h" // For esp_err_t

class SerialCli {
public:
    SerialCli();
    esp_err_t start_task();

private:
    static constexpr char TAG[] = "SerialCli";
    static constexpr int TASK_STACK_SIZE = 4096;
    static constexpr char PROMPT[] = "> ";
    static constexpr int MAX_INPUT_SIZE = 200;

    // Buffers for WiFi credentials
    char ssid_buffer_[33]{};
    char password_buffer_[65]{};
    bool ssid_set_;
    bool password_set_;

    TaskHandle_t task_handle_;

    void process_command(char* line);
    static void cli_task_trampoline(void* arg);
    void cli_task_member();
};

#endif // SERIAL_CLI_H
