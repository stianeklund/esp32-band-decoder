# MQTT Client Component

## Overview
The MQTT Client component enables the antenna switch system to receive frequency and radio status information via MQTT, providing an alternative to direct CAT command parsing over UART. This is particularly useful for integration with digital interfaces or remote operation scenarios.

## Responsibilities
- Establishes and maintains a connection to an MQTT broker
- Subscribes to relevant OmniRig topics for radio information
- Parses JSON radio status information received from MQTT
- Extracts current frequency and transmit state information
- Provides callbacks for frequency changes
- Publishes status information to MQTT topics
- Handles connection management and reconnection

## Key Classes

### MQTTClient
A singleton class that manages the MQTT connection and message processing.

#### Key Methods
- `instance()`: Returns the singleton instance
- `init()`: Initializes the MQTT client
- `connect()`: Establishes connection to the configured MQTT broker
- `publish_message()`: Publishes messages to specified topics
- `set_frequency_callback()`: Sets a callback function for frequency updates
- `subscribe_to_omnirig_topics()`: Subscribes to OmniRig radio information topics
- `handle_radio_info()`: Processes radio information received via MQTT
- `get_current_frequency()`: Returns the current frequency from MQTT data
- `is_transmitting()`: Returns the current transmit status from MQTT data

## Integration Points
- Provides frequency and transmit status information to the Antenna Switch
- Alternative data source to the CAT Parser component
- Configuration is managed through Config Manager
- Can operate alongside CAT Parser for redundant or complementary operation
- Interfaces with OmniRig or similar MQTT-based radio control software