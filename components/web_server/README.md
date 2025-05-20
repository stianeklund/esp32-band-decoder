# Web Server Component

## Overview
The Web Server component provides a user-friendly web interface for configuring and controlling the antenna switch system. It serves HTML, JavaScript, and handles API requests for system status and configuration.

## Responsibilities
- Implements a lightweight HTTP server for the ESP32
- Serves the web-based user interface for system configuration
- Handles HTTP GET and POST requests for configuration and control
- Provides real-time status information via API endpoints
- Manages relay control through web interfaces
- Implements system management functions (restart, reset configuration)
- Registers URI handlers for different endpoints

## Key Classes

### WebServer
A singleton class that manages the HTTP server and request handling.

#### Key Methods
- `instance()`: Returns the singleton instance
- `init()`: Initializes the web server configuration
- `start()`: Starts the web server
- `stop()`: Stops the web server
- `restart()`: Restarts the web server
- `is_running()`: Checks if the web server is active
- `register_uri_handlers()`: Sets up handlers for different URI endpoints

### Request Handlers
The class implements several HTTP request handlers:
- `root_get_handler()`: Main page handler
- `config_get_handler()`: Retrieves current configuration as JSON
- `status_get_handler()`: Provides current system status
- `config_post_handler()`: Updates system configuration
- `toggle_auto_mode_handler()`: Toggles automatic/manual mode
- `reset_config_handler()`: Resets configuration to defaults
- `restart_handler()`: Triggers system restart
- `reset_wifi_handler()`: Resets Wi-Fi configuration
- `relay_status_handler()`: Provides relay status information
- `relay_control_handler()`: Controls individual relays

## HTML Content
The component works with the HtmlContent class (in html_content.cpp/h) to serve web pages and UI elements.

## Integration Points
- Provides the primary user interface for the antenna switch system
- Uses Config Manager to retrieve and update system configuration
- Interfaces with Antenna Switch and Relay Controller for status and control
- Works with Wi-Fi Manager for network connectivity information
- Triggers system restarts and configuration resets when requested