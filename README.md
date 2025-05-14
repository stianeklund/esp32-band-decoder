# KC868-A16 ESP32 Antenna Switch Controller


This project uses a Kincony KC868-A16 to drive a antenna switch.

The KC868-A16's mosfet outputs can be used to drive relays to switch between different antennas based on the current operating frequency (limited CAT command support), or manual selection.
This project supports the Kincony KC868-A16 board, but can be compiled for ESP32 / ESP32S3 etc.

## Features

- Automatic antenna switching based on frequency (very fast)
- Hot switch protection (don't switch if transmitting)
- Supports multiple radios (currently only support for 1 CAT driven)
- Different switching modes (Concurrent, Alternating and Radio A only)
- Port conflict resolution functionality
- Manual antenna selection with support for multiple inputs (e.g 2x6 switches)
- Supports both UART or MQTT (or both at the same time) for CAT / band data
- Adjustable number of outputs and supported bands
- ~~TCP Client to interface with KC868-A16 to drive antenna relays~~
- CAT command parsing to extrapolate band information & transmit state
- Web interface for configuration and control
- Wi-Fi connectivity for remote access
- SmartConfig for easy Wi-Fi setup. iOS/Android or Windows apps can be used

## Components

The project consists of several key components:

1. **Relay Controller**: Manages the physical relays connected to different antennas.
2. **Antenna Switch**: Handles the logic for selecting the appropriate antenna based on frequency or user input.
3. **CAT Parser**: Interprets CAT commands for integration with radio transceivers.
4. **TCP Client**: ~~Enables remote control and status updates via TCP protocol.~~
3. **MQTT Client**: Supports MQTT to listen for specific events to parse transceiver status.
4. **UDP Client**: ~~Enables remote control and status updates via UDP protocol.~~
5. **Wi-Fi Manager**: Manages Wi-Fi connectivity for the ESP32.
6. **Web Server**: Provides a web interface for configuration and control.

## Building and Flashing

This project uses the ESP-IDF framework. To build and flash the project:

1. Set up the ESP-IDF environment.
2. Navigate to the project directory.
3. Run `idf.py build` to build the project.
4. Run `idf.py -p (PORT) flash` to flash the ESP32, replacing (PORT) with your device's port.

## Configuration

The antenna switch can be configured through the web interface or by modifying the `antenna_switch_config_t`.
This includes setting up frequency bands, antenna ports, and communication settings.

![](https://github.com/stianeklund/esp32-band-decoder/blob/kc868/screenshots/Antenna_controller.png)
![](https://github.com/stianeklund/esp32-band-decoder/blob/kc868/screenshots/band_definition.png)

---

Transmit state is indicated by the relay / output turning red
![Transmit indication](https://github.com/stianeklund/esp32-band-decoder/blob/kc868/screenshots/Transmit_Indication.png)
Legal / valid antenna alternatives are indicated with a blue button, and green indicates the currently selected port / relay

![Alternative antenna](https://github.com/stianeklund/esp32-band-decoder/blob/kc868/screenshots/Alternative_antenna.png)

## TODO

* Add CAT polling support
* Improve / harden interlock functionality (hasn't been fully tested).
* Remove / refactor TCP & UDP client's: these are leftovers from when this project code interfaced with the KC868-A16 instead of running on it natively.

### NOTE / WARNING:

The interlock functionality is not fully tested or fully featured.
Currently the functionality is built so that it's not possible to select the same antenna port for Radio A or B, but there is no logic
to avoid selecting another antenna for the same band.

CAT data over UART requires data to be fed to UART without the KC868-A16 being able to inquire back, this is due to my own configuration as
I use the RS232 lines on the radio I built a "sniffer" to grab band data.

Future versions will likely include optional support for polling.


## Usage

Once flashed and powered on, the ESP32 will start the antenna switch controller.
TODO Add more details on functionality


## License

This project is licensed under the GNU General Public License v3.0 License. See the LICENSE file for details.
