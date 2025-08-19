# ESP32 Band Decoder - Antenna Switch Controller

## Overview
This project implements an intelligent antenna switch controller for amateur radio using an ESP32 microcontroller and the Kincony KC868-A16 relay board. 
It automatically selects the appropriate antenna based on the operating frequency of connected radios, supporting both basic Kenwood style CAT commands via RS232 / UART and MQTT for radio control integration.

Both the ESP32 and ESP32S3 and similar should be supported.

## Key Features

- **Automatic Antenna Switching**: Automatically selects the appropriate antenna based on frequency band
- **Dual Radio Support**: Controls antennas for two radios with three operation modes:
  - Single Radio A: Only Radio A is active
  - Alternating A/B: Either Radio A or B can be active (not simultaneously)
  - Concurrent A/B: Both radios can be active simultaneously on different antennas
- **Transmit Protection**: Prevents antenna switching during transmission (hot-switching protection)
- **Multiple Control Interfaces**:
  - UART/RS232 for direct CAT command parsing
  - MQTT for integration with OmniRig or other radio control software
  - Web interface for configuration and manual control
- **Port Conflict Resolution**: Interlock functionality prevents multiple radios from using the same antenna
- **Manual Control**: Manual antenna selection with intuitive web interface
- **Persistent Configuration**: Stored in non-volatile memory with asynchronous saving
- **Easy Wi-Fi Setup**: SmartConfig support for easy network configuration
- **Band Configuration**: Configurable frequency bands and antenna mapping
- **Relay Customization**: Support for naming relays and custom antenna configurations
- **Visual Feedback**: Web interface shows transmit state, available antennas, and active selections
- **Configuration Management**: Import/export configuration files for backup and sharing
- **Data Source Monitoring**: Real-time indication of frequency data source (Serial vs MQTT)

## Hardware Requirements

- ESP32 microcontroller (or ESP32-S3)
- Kincony KC868-A16 relay control board
- Antenna switch with 12v relays
- Optional RS232 TTL interface for direct CAT connection

## Communication Methods

- **Serial/UART**: Processes standard Kenwood-style CAT commands via RS232/UART connection
- **MQTT**: Integration with MQTT brokers for radio status information (OmniRig compatible)
- **Web Interface**: Comprehensive control and configuration via browser
- **Wi-Fi**: Wireless connectivity for remote access and monitoring
- **Data Source Priority**: Serial connection takes precedence over MQTT when both are active

## System Architecture

The project is built as a modular ESP-IDF application with the following components:

### Core Components

- **Antenna Switch**: Central logic for antenna selection based on frequency and configuration
- **Relay Controller**: Manages physical relay outputs with safety features
- **KC868-A16 Hardware**: Low-level I2C communication with the relay board
- **Config Manager**: Handles persistent configuration with observer notifications

### Communication Components

- **CAT Parser**: Interprets radio CAT commands over Serial/UART connection
- **MQTT Client**: Processes radio data via MQTT
- **Wi-Fi Manager**: Handles network connectivity and setup
- **Web Server**: Provides configuration UI and control interface

### System Services

- **System Initializer**: Coordinates system startup sequence
- **Restart Manager**: Prevents boot-loops and manages safe restarts

### Legacy Components (Planned for Removal or Rewrite)

- **TCP Client**: TCP socket communication
- **UDP Client**: UDP datagram communication

## Configuration and Operation

### Web Interface
The system provides a comprehensive web interface for configuration, accessible via the ESP32's IP address. 

Key configuration sections include:

- **Band Definition**: Configure frequency ranges and allowed antennas
- **Relay Names**: Custom names for each relay/antenna
- **Communication Settings**: UART and MQTT configuration
- **Operation Mode**: Select single radio, alternating, or concurrent mode  
- **Manual Control**: Direct relay control for testing or manual operation
- **Configuration Backup**: Export configuration to JSON file for backup
- **Configuration Restore**: Import previously saved configuration files
- **Data Source Display**: Shows whether frequency data comes from Serial or MQTT connection

![Antenna Controller](https://github.com/stianeklund/esp32-band-decoder/blob/kc868/screenshots/Antenna_controller.png)
![Band Definition](https://github.com/stianeklund/esp32-band-decoder/blob/kc868/screenshots/band_definition.png)

### Status Indication
The web interface provides comprehensive visual feedback:
- **Green button**: Currently selected antenna/relay
- **Blue button**: Valid alternate antenna options for current frequency
- **Red highlight**: Active transmission state (hot-switching protection active)
- **Data Source**: Shows "Serial" when using direct radio connection or "MQTT" when using network data
- **Real-time Updates**: Frequency, antenna selection, and transmission status update automatically

![Transmit Indication](https://github.com/stianeklund/esp32-band-decoder/blob/kc868/screenshots/Transmit_Indication.png)
![Alternative Antenna](https://github.com/stianeklund/esp32-band-decoder/blob/kc868/screenshots/Alternative_antenna.png)

### Configuration Management
The system includes robust configuration backup and restore capabilities:

- **Export Configuration**: Download your complete system configuration as a JSON file
- **Import Configuration**: Upload and restore previously saved configuration files
- **Version Validation**: Ensures imported configurations are compatible with current firmware
- **Backup Workflow**: Easy backup and restore for system maintenance or sharing configurations
- **Error Handling**: Clear feedback during import/export operations with validation errors

Configuration files include all settings: band definitions, relay names, communication parameters, operation modes, and system preferences.

## Performance and Optimization

The system includes several optimizations for reliable operation:

- **Memory Management**: Chunked HTML generation prevents memory allocation issues
- **Optimized Web Interface**: Minified CSS and JavaScript reduce memory footprint by 23%
- **Efficient Data Transfer**: Streaming content delivery eliminates large string allocations
- **Dark/Light Theme**: Automatic theme detection with manual toggle support
- **Responsive Design**: Mobile-friendly interface for remote monitoring

## Building and Flashing

This project uses the ESP-IDF framework. To build and flash:

1. Set up the ESP-IDF environment
2. Navigate to the project directory
3. Run `idf.py build` to build the project
4. Run `idf.py -p (PORT) flash` to flash the ESP32, replacing (PORT) with your device's port
5. Optional: `idf.py -p (PORT) monitor` to view debug output

## Future Development

- Add CAT polling support for radios that don't provide automatic updates
- Improve interlock functionality with more robust conflict resolution
- Remove or rewrite legacy TCP & UDP client components
- Support additional radio manufacturers' CAT commands
- Add frequency history and usage statistics

## Important Notes

### Interlock Functionality
The interlock functionality is still under development. 
Currently, it prevents selection of the same antenna port for Radio A and B. 
There is no logic to avoid selecting another antenna for the same band.

### CAT Data Requirements
CAT data over UART is currently designed to work without needing to query the radio, functioning as a passive "sniffer" for band data. 
Future versions may include optional polling support.

## License

This project is licensed under the GNU General Public License v3.0. See the LICENSE file for details.