# CAT Parser Component

## Overview
The CAT Parser component is responsible for interpreting Computer Aided Transceiver (CAT) commands from radio equipment. It processes serial data to extract critical information such as frequency, transmission status, and operating mode to enable automatic antenna switching.

## Responsibilities
- Parses CAT commands from radio transceivers over UART
- Extracts frequency information for band determination
- Detects transmit/receive status changes
- Processes radio operating parameters (mode, RIT/XIT, split operation)
- Manages an efficient UART communication interface
- Provides both C++ and C-style interfaces for backward compatibility
- Maintains current radio state information

## Key Classes

### CatParser
A singleton class that handles parsing and processing of CAT commands.

#### Key Methods
- `instance()`: Returns the singleton instance
- `init()`: Initializes the CAT parser
- `process_command()`: Processes received CAT commands
- `process_serial_data()`: Handles incoming serial data
- `handle_frequency_change()`: Updates the system when frequency changes
- `get_frequency()`: Returns current operating frequency
- `is_transmitting()`: Returns current transmit status

## Supported Commands
The parser implements handlers for several common CAT commands:
- Frequency commands (FA)
- Information commands (IF)
- Auto-Information control (AI)
- TX/RX status commands
- Antenna port selection (AP)

## Integration Points
- Communicates with the Antenna Switch component to update frequency information
- Provides transmit status information to the Relay Controller
- Configured through the Config Manager
- UART communication for direct connection with radio equipment