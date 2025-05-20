# KC868-A16 Hardware Component

## Overview
This component provides a hardware abstraction layer for the Kincony KC868-A16 relay control board. It handles the low-level I2C communication with the PCF8574 I/O expanders that control the relays.

## Responsibilities
- Initializes and manages I2C communication with the KC868-A16 board
- Controls the 16 relay outputs via PCF8574 I/O expanders
- Provides both individual and batch control of relay outputs
- Retrieves current relay states
- Handles low-level hardware communication details

## Key Functions

### Public API
- `kc868_a16_hw_init()`: Initializes the I2C interface and hardware configuration
- `kc868_a16_set_output()`: Sets the state of a single relay output (0-15)
- `kc868_a16_get_output_state()`: Retrieves the current state of a single relay output
- `kc868_a16_set_all_outputs()`: Sets all 16 outputs at once using a bit mask
- `kc868_a16_get_all_outputs()`: Gets the state of all 16 outputs as a bit mask

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