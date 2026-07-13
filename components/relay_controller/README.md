# Relay Controller Component

## Overview
The Relay Controller component provides a high-level interface for controlling the physical relay outputs. It manages the logical mapping between antenna band selection and physical relay activation, handles radio-specific relay allocation, and implements safety features to prevent hot-switching.

## Responsibilities
- Controls physical relay outputs through the KC868 hardware interface
- Implements safety features to prevent switching during transmission
- Maintains state information about currently active relays
- Manages last selected relays for each band
- Enforces delay between relay operations to prevent rapid switching
- Supports dual radio operation (Radio A and Radio B)
- Handles relay activation for specific antennas and bands
- Provides methods to query current relay states

## Key Classes

### RelayController
A singleton class that provides the main interface for relay control.

#### Key Methods
- `instance()`: Returns the singleton instance
- `init()`: Initializes the relay controller
- `turn_off_all_relays()`: Deactivates all relays
- `set_relay()`: Directly controls a specific relay
- `get_relay_state()`: Returns the current state of a specific relay
- `get_all_relay_states()`: Returns states of all relays
- `set_relay_for_antenna()`: Activates a relay for a specific band and radio
- `turn_off_all_relays_except()`: Turns off all relays except the specified one
- `get_last_selected_relay_for_band()`: Retrieves the last used relay for a band
- `is_correct_relay_set()`: Checks if the correct relay is set for the current band

## Safety Features
- Checks transmit status before allowing relay changes
- Implements cooldown period between relay activations
- Maintains separate relay masks for Radio A and Radio B
- Thread-safe implementation with mutex protection

## Integration Points
- Used by the Antenna Switch component to control physical relays
- Interfaces with the KC868-A16 Hardware component for physical control
- Checks transmit status via the CAT Parser
- Maintains state information for the Web Server status display


![PTT detection latency](https://github.com/stianeklund/esp32-band-decoder/blob/kc868/screenshots/SDS00030.png)

The KC868‐A16 polls the radio’s PTT line (blue) via I²C (no interrupts possible through the IO expanders, unfortunately).
The firmware takes on average ~1.4 ms (±0.5 ms jitter) to switch off any unwanted MOSFET output (yellow). 

Radio and configuration: 

* Some radios use 10 ms to key external amplifiers, others up to 25 ms, depending on the configuration.
* 
I made a crude RF detector (purple) to try to detect _when_ RF power reaches the dummy load. 
![Time from ptt detection to RF out](https://github.com/stianeklund/esp32-band-decoder/blob/kc868/screenshots/SDS00029.png)

The scope readings seem to indicate this is roughly 11.3–13 ms after PTT, giving us roughly a margin of 
9–10 ms to protect downstream devices, provided relay‐release times are short. 

I haven't tested this measuring the actual relays.. the mosfet outputs on the KC868-A16 are extremely fast.