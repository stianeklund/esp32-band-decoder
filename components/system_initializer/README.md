# System Initializer Component

## Overview
The System Initializer component provides a centralized mechanism for initializing the various subsystems of the antenna switch controller. It ensures proper initialization sequence and error handling during system startup.

## Responsibilities
- Provides structured initialization of system components
- Manages initialization sequence to ensure dependencies are properly handled
- Initializes Non-Volatile Storage (NVS)
- Sets up system-wide task watchdog
- Offers both basic and full initialization options
- Returns initialization errors to the caller

## Key Classes

### SystemInitializer
A utility class that provides static methods for system initialization.

#### Key Methods
- `initialize_basic()`: Performs basic system initialization (NVS, watchdog)
- `initialize_full()`: Performs complete system initialization including relay controller
- `init_nvs()`: Initializes the NVS system for configuration storage
- `init_task_watchdog()`: Sets up the ESP-IDF task watchdog timer

## Initialization Sequence
The initialization process follows a structured approach:
1. Basic initialization (NVS, system services)
2. Hardware components (KC868-A16, relay controller)
3. Communication components (Wi-Fi, MQTT, CAT parser)
4. User interface components (web server)

## Integration Points
- Called by main.cpp during system startup
- Initializes the Relay Controller and provides a pointer to the caller
- Uses the Restart Manager to handle boot safety
- Prepares the system for the Config Manager to load stored settings