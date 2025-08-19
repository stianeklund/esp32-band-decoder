# InputManager Component

## Overview

The `InputManager` component is responsible for managing and interpreting hardware input signals for the antenna switch system, primarily focusing on PTT / TX signals from up to two radios. It interfaces with the underlying `kc868_a16_hw` component to read states from the KC868-A16 I/O expander board.

Key responsibilities include:

*   **Hardware Initialization:** Ensures the `kc868_a16_hw` I2C interface and input expanders are initialized.
*   **PTT Polling:** Continuously polls configured hardware input pins dedicated to PTT signals for Radio A and Radio B (interrupts are not possible with the PF8574 on the KC868-A16 without hw mods)
*   **State Interpretation:** Translates raw hardware input states (which are inverted by the KC868-A16 input circuitry and `kc868_a16_get_all_inputs` function) into logical PTT active/inactive states. This interpretation considers per-radio configuration settings that define whether the PTT signal at the terminal is active-high or active-low.
*   **Change Notification:** Detects changes in the logical PTT states and notifies the `AntennaSwitch` component via callbacks (`on_hw_ptt_a_state_change` and `on_hw_ptt_b_state_change`).
*   **Configuration Aware:** Uses the system configuration (accessed via `ConfigCache` inheritance) to determine which input pins are assigned to PTT A and PTT B, and their respective active logic levels.
*   **Direct Input Access:** Provides an API for other components to query the current state of any individual hardware input or get a bitmask of all input states.

The `InputManager` is implemented as a singleton and runs a dedicated FreeRTOS task for polling PTT inputs to ensure timely detection of state changes.

## Performance

The PTT polling task (`ptt_poll_task`) is designed for low latency to ensure rapid detection of PTT signal changes. It polls hardware inputs every 1 millisecond.

**Overall PTT Detection Latency:**
Recent optimizations have resulted in the `InputManager` detecting a PTT state change and initiating the callback to `AntennaSwitch` in approximately **0.63 milliseconds (630 microseconds)** from the start of the poll cycle in which the change is detected.

**Breakdown of a Typical PTT Detection Cycle (when a change occurs):**
The following timings are based on internal profiling logs, which are output at `ESP_LOGD` level only when a PTT state change is detected to minimize performance overhead during normal operation:

1.  **`kc868_a16_get_all_inputs()`:** ~625-632 microseconds.
    *   This is the time taken to read the state of all 16 hardware inputs from the KC868-A16 board via I2C communication. This is the most significant contributor to the latency within the `InputManager`'s polling cycle.
2.  **`determine_ptt_logical_state()`:** ~1 microsecond.
    *   This function performs the logical interpretation of the raw hardware input based on the configured active level (high/low) for the specific PTT input. Its execution time is negligible.

The sum of these operations, plus minimal overhead for timestamping and task scheduling, results in the ~0.63 ms total time from the start of a poll cycle to the confirmation of a PTT state change.

**Task Priority:**
The `ptt_poll_task` runs at a high priority (`configMAX_PRIORITIES - 2`) to minimize preemption by other tasks and ensure consistent polling intervals.

## API Reference

The `InputManager` provides the following public interface, accessible via its singleton instance:

### `static InputManager& instance()`

Retrieves the singleton instance of the `InputManager`.

*   **Returns:** A reference to the `InputManager` instance.

```cpp
InputManager& im = InputManager::instance();
```

### `esp_err_t init()`

Initializes the `InputManager`. This function:
1.  Initializes the underlying `kc868_a16_hw` hardware if not already done.
2.  Creates and starts the PTT polling FreeRTOS task.
3.  Sets an internal flag indicating successful initialization.

It is safe to call this multiple times; it will only perform full initialization once.

*   **Returns:**
    *   `ESP_OK` on successful initialization.
    *   `ESP_FAIL` if the PTT polling task creation fails.
    *   Other `esp_err_t` codes if `kc868_a16_hw_init()` fails.

```cpp
esp_err_t result = InputManager::instance().init();
if (result != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize InputManager: %s", esp_err_to_name(result));
}
```

### `esp_err_t get_input_state(uint8_t input_num, bool* state)`

Reads the current logical state of a specific hardware input pin. The KC868-A16 board has 16 input pins (0-15).
The returned `state` reflects the logical level at the input terminal, considering the inversion by the optocoupler and PCF8574.
- `true`: Input terminal is considered "active" or "high" (e.g., +12V present, or pulled to ground if configured as active low).
- `false`: Input terminal is considered "inactive" or "low".

*   **Parameters:**
    *   `input_num`: The input pin number to query (0-15).
    *   `state`: A pointer to a boolean where the state will be stored.
*   **Returns:**
    *   `ESP_OK` if the state was successfully read.
    *   `ESP_ERR_INVALID_STATE` if `InputManager` is not initialized.
    *   `ESP_ERR_INVALID_ARG` if `state` is `nullptr` or `input_num` is out of range (validation done by `kc868_a16_get_input_state`).
    *   Other `esp_err_t` codes on I2C communication failure.

```cpp
bool pin5_state;
if (InputManager::instance().get_input_state(5, &pin5_state) == ESP_OK) {
    ESP_LOGI(TAG, "Input 5 is %s", pin5_state ? "ACTIVE" : "INACTIVE");
}
```

### `esp_err_t get_all_inputs(uint16_t* state_mask)`

Reads the raw state of all 16 hardware input pins as a bitmask.
**Important:** The bits in this mask represent the physical state of the PCF8574 input pins.
Due to the KC868-A16's input circuit (optocoupler) and the behavior of `kc868_a16_get_all_inputs()`:
*   A bit set to `1` in `state_mask` means the corresponding PCF8574 pin is **physically LOW**.
*   A bit set to `0` in `state_mask` means the corresponding PCF8574 pin is **physically HIGH**.

This is the raw data used internally by the PTT polling task before logical interpretation.

*   **Parameters:**
    *   `state_mask`: A pointer to a `uint16_t` where the bitmask of input states will be stored. Bit 0 corresponds to input 0, bit 15 to input 15.
*   **Returns:**
    *   `ESP_OK` if the states were successfully read.
    *   `ESP_ERR_INVALID_STATE` if `InputManager` is not initialized.
    *   `ESP_ERR_INVALID_ARG` if `state_mask` is `nullptr`.
    *   Other `esp_err_t` codes on I2C communication failure.

```cpp
uint16_t all_inputs;
if (InputManager::instance().get_all_inputs(&all_inputs) == ESP_OK) {
    ESP_LOGI(TAG, "All inputs raw mask: 0x%04X", all_inputs);
}
```

## PTT / Transmit signal Handling Logic

The `InputManager`'s core PTT handling logic resides in its internal `ptt_poll_task`. This task periodically:
1.  Reads all input states using `kc868_a16_get_all_inputs()`.
2.  Retrieves the cached system configuration to know:
    *   Which input pin is assigned to Radio A PTT (`config.ptt_input_radio_a`).
    *   Whether Radio A's PTT signal is active-high at the terminal (`config.ptt_input_radio_a_active_high`).
    *   Similarly for Radio B (`config.ptt_input_radio_b`, `config.ptt_input_radio_b_active_high`).
3.  For each configured PTT input, it calls the `determine_ptt_logical_state` helper function. This function translates the raw PCF8574 pin state (HIGH/LOW) to a logical PTT state (active/inactive) based on the `_active_high` configuration. The logic is:
    *   **KC868-A16 Input Circuit:**
        *   Terminal HIGH (+12V, e.g., radio PTT active for an active-high setup) -> Optocoupler OFF -> PCF8574 pin HIGH.
        *   Terminal LOW (0V, e.g., radio PTT inactive for an active-high setup) -> Optocoupler ON -> PCF8574 pin LOW.
    *   **If `configured_terminal_active_high` is `true`:** PTT is logically active if the PCF8574 pin is HIGH.
    *   **If `configured_terminal_active_high` is `false`:** PTT is logically active if the PCF8574 pin is LOW.
4.  If the determined logical PTT state for Radio A or Radio B has changed since the last poll, it calls the respective callback on the `AntennaSwitch` instance:
    *   `AntennaSwitch::instance().on_hw_ptt_a_state_change(bool logical_ptt_active)`
    *   `AntennaSwitch::instance().on_hw_ptt_b_state_change(bool logical_ptt_active)`

This ensures that the `AntennaSwitch` is promptly informed of any relevant PTT activity.
