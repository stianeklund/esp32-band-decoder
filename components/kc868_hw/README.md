# KC868 Hardware Component

## Overview
This component provides a hardware abstraction layer for the Kincony KC868 relay control boards (KC868-A16 and KC868-A8, selected at build time via the "Band Decoder Board" Kconfig choice). It handles the low-level I2C communication with the PCF8574 I/O expanders that control the relays.

## Responsibilities
- Initializes and manages I2C communication with the KC868 board (A16: two PCF8574 output + two input expanders; A8: one of each)
- Controls the relay outputs via PCF8574 I/O expanders (16 on the A16, 8 on the A8)
- Provides both individual and batch control of relay outputs
- Retrieves current relay states
- Handles low-level hardware communication details

## Key Functions

### Public API
- `kc868_hw_init()`: Initializes the I2C interface and hardware configuration
- `kc868_hw_set_output()`: Sets the state of a single relay output (0-15)
- `kc868_hw_get_output_state()`: Retrieves the current state of a single relay output
- `kc868_hw_set_all_outputs()`: Sets all 16 outputs at once using a bit mask
- `kc868_hw_get_all_outputs()`: Gets the state of all 16 outputs as a bit mask

## Hardware Configuration
- Uses two PCF8574 I/O expanders for outputs (16 outputs total)
- Uses two PCF8574 I/O expanders for inputs (16 inputs total)
- I2C communication runs at 100kHz
- Default I2C configuration:
  - SCL: GPIO 5
  - SDA: GPIO 4
  - I2C port: I2C_NUM_0

## Integration Points
- Used by the Relay Controller component to physically control relays
- Provides the hardware abstraction layer between software and the physical relay board
- Simple C API designed for easy integration with other components