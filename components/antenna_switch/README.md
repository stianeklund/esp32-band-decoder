# Antenna Switch Component

## Overview
The Antenna Switch component is the core logic module for managing antenna switching based on radio frequency, operation mode, and user preferences. It provides a comprehensive API for controlling antenna selection for one or two radios.

## Responsibilities
- Manages the configuration of antenna ports for different frequency bands
- Implements automatic antenna selection based on current operating frequency
- Supports manual antenna selection when auto mode is disabled
- Manages interlock functionality to prevent multiple radios from using the same antenna
- Handles Radio A and Radio B operation in different modes (single, alternating, concurrent)
- Stores user preferences for antenna selection per band
- Maintains state during transmit operations to prevent hot-switching

## Key Classes

### AntennaSwitch
A singleton class that provides the main interface for the antenna switching functionality.

#### Key Methods
- `instance()`: Returns the singleton instance
- `init()`: Initializes the antenna switch
- `set_config()`: Updates the antenna switch configuration
- `get_config()`: Retrieves the current configuration
- `set_frequency()`: Updates the current frequency and triggers antenna switching if needed
- `set_auto_mode()`: Toggles between automatic and manual antenna selection
- `set_relay()`: Directly controls a specific relay
- `set_relay_for_antenna()`: Activates a specific relay for a given band and radio
- `on_radio_a_tx_start()/on_radio_a_tx_stop()`: Handles transmit state changes

### Configuration Structures
- `band_config_t`: Configuration for a specific frequency band
- `antenna_switch_config_t`: Complete configuration for the antenna switch system
- `radio_operation_mode_t`: Defines the operating mode for dual radio operation

## Integration Points
- Interfaces with the Relay Controller component to physically control antenna relays
- Receives frequency information from CAT Parser or MQTT Client
- Configuration is managed through Config Manager
- Exposes status and control through Web Server interfaces