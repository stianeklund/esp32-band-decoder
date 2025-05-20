# UDP Client Component

## Overview
The UDP Client component provides UDP socket communication capabilities for lightweight, connectionless messaging. Similar to the TCP Client, this is noted as a legacy component that may be removed in future versions according to the main README.

## Responsibilities
- Creates and manages a UDP socket for communication
- Provides methods for sending UDP messages to specified hosts
- Implements message reception with timeout capability
- Handles socket lifecycle (creation, usage, and cleanup)
- Offers simple UDP communication without connection state overhead

## Key Classes

### UDPClient
A class that manages a UDP socket for sending and receiving datagrams.

#### Key Methods
- `init()`: Initializes the UDP socket with a target host and port
- `send_message()`: Sends a UDP message to the configured destination
- `receive_message()`: Receives a UDP message with specified timeout
- `get_sock()`: Returns the socket descriptor for external use
- `close()`: Closes the UDP socket

## Features
- Connectionless communication for lightweight messaging
- Simple API for UDP communication
- No reconnection or state management (unlike TCP client)
- Efficient for simple status updates or commands

## Integration Points
- Used for lightweight remote control or status updates
- Simpler alternative to TCP when reliable delivery is not critical
- Can be integrated with other components that need UDP communication
- Note: According to the main README, this component may be removed in future versions