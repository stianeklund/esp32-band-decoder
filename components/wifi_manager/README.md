# Wi-Fi Manager Component

## Overview
The Wi-Fi Manager component handles Wi-Fi connectivity for the antenna switch system. It provides connection management, SmartConfig setup, and credential storage functionality.

## Responsibilities
- Initializes and manages Wi-Fi station (STA) mode
- Handles connection and reconnection to configured Wi-Fi networks
- Implements ESP32 SmartConfig for easy setup via mobile apps
- Stores Wi-Fi credentials in Non-Volatile Storage (NVS)
- Provides network status information (IP address, connection state)
- Manages Wi-Fi events and state transitions
- Offers credential management functions (save, load, clear)

## Key Classes

### WifiManager
A singleton class that manages Wi-Fi functionality.

#### Key Methods
- `instance()`: Returns the singleton instance
- `init()`: Initializes Wi-Fi subsystem
- `is_connected()`: Returns current connection status
- `is_in_smartconfig_mode()`: Checks if SmartConfig is active
- `get_ip_info()`: Retrieves current IP address information
- `get_mac_address()`: Gets the device MAC address
- `wait_for_connection()`: Waits for Wi-Fi connection with timeout
- `connect_sta()`: Connects to specified SSID with password
- `disconnect()`: Disconnects from current Wi-Fi network
- `clear_credentials()`: Clears stored Wi-Fi credentials
- `start_smartconfig()`: Initiates SmartConfig setup mode
- `save_wifi_config()`: Saves Wi-Fi credentials to storage
- `load_wifi_config()`: Loads stored Wi-Fi credentials

## SmartConfig
The component implements ESP32 SmartConfig (ESP-TOUCH protocol) allowing easy setup:
- Users can configure the device using smartphone apps
- No hardcoded credentials needed for initial setup
- Timeout mechanism to exit SmartConfig mode if not completed

## Event Management
- Handles Wi-Fi connection events
- Manages IP acquisition
- Tracks connection status with FreeRTOS event groups
- Provides synchronization for connection completion

## Integration Points
- Used by the Web Server to display network information
- Provides connectivity for MQTT Client
- Essential for remote web access to the controller
- Supports system recovery through SmartConfig