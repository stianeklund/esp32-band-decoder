#include "relay_controller.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <array>

static const char *TAG = "RELAY_CONTROLLER";

constexpr int NUM_RELAYS = 3;
constexpr gpio_num_t RELAY_1_GPIO = GPIO_NUM_18;
constexpr gpio_num_t RELAY_2_GPIO = GPIO_NUM_19;
constexpr gpio_num_t RELAY_3_GPIO = GPIO_NUM_21;

static const std::array<gpio_num_t, NUM_RELAYS> relay_gpios = {RELAY_1_GPIO, RELAY_2_GPIO, RELAY_3_GPIO};

esp_err_t relay_controller_init()
{
    ESP_LOGI(TAG, "Initializing relay controller");

    static const std::array<gpio_num_t, NUM_RELAYS> relay_gpios = {RELAY_1_GPIO, RELAY_2_GPIO, RELAY_3_GPIO};

    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << RELAY_1_GPIO) | (1ULL << RELAY_2_GPIO) | (1ULL << RELAY_3_GPIO);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;

    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error configuring GPIO pins: %s", esp_err_to_name(ret));
        return ret;
    }

    // Initialize all relays to OFF state
    for (const auto& gpio : relay_gpios) {
        gpio_set_level(gpio, 0);
    }

    return ESP_OK;
}

esp_err_t relay_controller_set_antenna(const bool* antenna_ports)
{
    ESP_LOGI(TAG, "Setting antenna configuration");

    if (antenna_ports == nullptr) {
        ESP_LOGE(TAG, "Invalid antenna configuration");
        return ESP_ERR_INVALID_ARG;
    }

    for (int i = 0; i < NUM_RELAYS; i++) {
        gpio_set_level(relay_gpios[i], antenna_ports[i] ? 1 : 0);
        ESP_LOGI(TAG, "Relay %d set to %d", i + 1, antenna_ports[i] ? 1 : 0);
    }

    return ESP_OK;
}
