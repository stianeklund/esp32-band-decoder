#ifndef INPUT_MANAGER_H
#define INPUT_MANAGER_H

#include "esp_err.h"
#include <mutex>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config_cache.h"

class InputManager final : public ConfigCache {
public:
    // Singleton instance method
    static InputManager& instance();

    // Delete copy constructor and assignment operator
    InputManager(const InputManager&) = delete;
    InputManager& operator=(const InputManager&) = delete;

    /**
     * @brief Initializes the InputManager and the underlying hardware if not already initialized.
     * This function should be called once at startup.
     * @return ESP_OK on success, or an error code if initialization fails.
     */
    esp_err_t init();

    /**
     * @brief Gets the logical state of a single input.
     * @param input_num The input number (0-15).
     * @param state Pointer to a boolean where the state will be stored (true for active/asserted, false for inactive).
     * @return ESP_OK on success, ESP_ERR_INVALID_ARG if input_num is out of range or state is nullptr,
     *         or an I2C communication error.
     */
    esp_err_t get_input_state(uint8_t input_num, bool* state);

    /**
     * @brief Gets the logical state of all inputs as a bitmask.
     * @param state_mask Pointer to a uint16_t where the input states will be stored.
     *                   Bit 0 corresponds to input 0, bit 1 to input 1, and so on.
     *                   A '1' in a bit position means the corresponding input is active/asserted.
     * @return ESP_OK on success, ESP_ERR_INVALID_ARG if state_mask is nullptr,
     *         or an I2C communication error.
     */
    esp_err_t get_all_inputs(uint16_t* state_mask);

private:
    InputManager();
    ~InputManager() = default;

    static void ptt_poll_task_trampoline(void *arg);
    void handle_ptt_line(int config_pin, bool active_high, bool& last_state, const char* label,
                         uint16_t inputs_mask, const std::function<void(bool)>& on_state_change);
    void handle_ptt_read_error();
    void ptt_poll_task();

    static InputManager* instance_;
    static std::mutex instance_mutex_;

    bool initialized_ = false;
    TaskHandle_t ptt_poll_task_handle_ = nullptr;
    bool ptt_a_last_hw_state_ = false; 
    bool ptt_b_last_hw_state_ = false; 

    // Locally cached PTT configuration
    int ptt_input_radio_a_config_ = -1;
    bool ptt_input_radio_a_active_high_config_ = false;
    int ptt_input_radio_b_config_ = -1;
    bool ptt_input_radio_b_active_high_config_ = false;

    /**
     * @brief Refreshes the locally cached PTT configuration settings 
     *        from the main configuration cache if necessary.
     */
    void refresh_active_ptt_config();
};

#endif // INPUT_MANAGER_H
