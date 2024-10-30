# ESP32 Antenna Switch Controller

This project implements an antenna switch controller using an ESP32 microcontroller. The controller manages multiple
relays to switch between different antennas based on the current operating frequency or manual selection.
The ESP32 itself interfaces with a Kincony KC868-A16 board, however the long term goal is to have this code run on that
board itself.

## Features

- Automatic antenna switching based on frequency
- Manual antenna selection (somewhat..)
- TCP Client to interface with KC868-A16 to drive antenna relays
- CAT command parsing to extrapolate frequency information
- Web interface for configuration and control
- Wi-Fi connectivity for remote access
- SmartConfig for easy Wi-Fi setup, requires iOS / Android app I think

## Components

The project consists of several key components:

1. **Relay Controller**: Manages the physical relays connected to different antennas.
2. **Antenna Switch**: Handles the logic for selecting the appropriate antenna based on frequency or user input.
3. **CAT Parser**: Interprets CAT commands for integration with radio transceivers.
4. **TCP Client**: Enables remote control and status updates via TCP protocol.
5. **Wi-Fi Manager**: Manages Wi-Fi connectivity for the ESP32.
6. **Web Server**: Provides a web interface for configuration and control.

## Building and Flashing

This project uses the ESP-IDF framework. To build and flash the project:

1. Set up the ESP-IDF environment.
2. Navigate to the project directory.
3. Run `idf.py build` to build the project.
4. Run `idf.py -p (PORT) flash` to flash the ESP32, replacing (PORT) with your device's port.

## Configuration

The antenna switch can be configured through the web interface or by modifying the `antenna_switch_config_t` structure
in the code. This includes setting up frequency bands, antenna ports, and TCP communication settings.

![](https://github.com/stianeklund/esp32-band-decoder/blob/master/webconfig.png)

## TODO

* Hot switch protection (don't switch if transmitting)
* Add support for RS485 to interface with the KC868 directly rather than over WiFI
* Interlock
* Port to to run on kc868 directly

### NOTE / WARNING: 

Every time we change the output state of the mosfets on the KC868 it saves the state to NVS, 
This needs to somehow be turned off or custom firmware needs to be written (porting this project) to minimize the
amount of writes to NVS for longevity reasons

## Usage

Once flashed and powered on, the ESP32 will start the antenna switch controller. You can interact with it via:

1. The web interface (connect to the ESP32's IP address)
2. TCP commands sent to the configured IP and port
3. CAT commands via the UART interface

## License

This project is licensed under the GNU General Public License v3.0 License. See the LICENSE file for details.
