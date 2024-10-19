#include "relay_controller.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "RELAY_CONTROLLER";

#define NUM_RELAYS 3
#define RELAY_1_GPIO 18
#define RELAY_2_GPIO 19
#define RELAY_3_GPIO 21

static const gpio_num_t relay_gpios[NUM_RELAYS] = {RELAY_1_GPIO, RELAY_2_GPIO, RELAY_3_GPIO};

esp_err_t relay_controller_init(void)
{
    ESP_LOGI(TAG, "Initializing relay controller");

    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << RELAY_1_GPIO) | (1ULL << RELAY_2_GPIO) | (1ULL << RELAY_3_GPIO),
        .pull_down_en = 0,
        .pull_up_en = 0
    };

    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error configuring GPIO pins: %s", esp_err_to_name(ret));
        return ret;
    }

    // Initialize all relays to OFF state
    for (int i = 0; i < NUM_RELAYS; i++) {
        gpio_set_level(relay_gpios[i], 0);
    }

    return ESP_OK;
}

esp_err_t relay_controller_set_antenna(uint8_t antenna)
{
    ESP_LOGI(TAG, "Setting antenna: %d", antenna);

    if (antenna == 0 || antenna > (1 << NUM_RELAYS)) {
        ESP_LOGE(TAG, "Invalid antenna number: %d", antenna);
        return ESP_ERR_INVALID_ARG;
    }

    for (int i = 0; i < NUM_RELAYS; i++) {
        gpio_set_level(relay_gpios[i], (antenna & (1 << i)) ? 1 : 0);
    }

    return ESP_OK;
}
