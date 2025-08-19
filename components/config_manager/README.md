# Config Manager Component

## Overview
The Config Manager component provides centralized configuration management for the entire antenna switch system. It implements a singleton pattern with observer notifications and asynchronous NVS (Non-Volatile Storage) saving.

## Responsibilities
- Manages system-wide configuration settings
- Provides a centralized access point for all components to retrieve configuration
- Implements the observer pattern to notify components of configuration changes
- Handles saving configuration to Non-Volatile Storage (NVS)
- Implements asynchronous saving to prevent blocking operations
- Provides default configuration initialization
- Loads stored configuration from NVS on startup

## Key Classes

### ConfigManager
A singleton class that manages the system configuration.

#### Key Methods
- `instance()`: Returns the singleton instance
- `init()`: Initializes the configuration manager with default values if needed
- `get_config()`: Retrieves the current configuration (const reference)
- `update_config()`: Updates the configuration and notifies observers
- `save_to_nvs()`: Saves the current configuration to NVS
- `load_from_nvs()`: Loads configuration from NVS
- `add_observer()`: Adds a function to be called when configuration changes
- `flush_pending_save()`: Forces pending configuration changes to be saved

## Asynchronous NVS Writing
The component implements an asynchronous NVS writing mechanism:
- Changes are flagged as "dirty" when configuration is updated
- A dedicated FreeRTOS task handles NVS writes in the background
- Semaphore signaling for efficient task wakeup
- Prevents blocking the main application during storage operations

## Integration Points
- Used by all components that need configuration access
- Primary configuration source for the Antenna Switch component
- Observers typically include components that need to react to configuration changes
- Web Server uses this component to persist user configuration changes