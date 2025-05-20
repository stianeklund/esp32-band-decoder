# TCP Client Component

## Overview
The TCP Client component provides a robust TCP socket interface for communication with remote servers. While noted as a legacy component that may be removed in future versions (according to the main README), it currently offers reliable TCP communication capabilities.

## Responsibilities
- Establishes and maintains TCP socket connections to remote servers
- Handles connection management, including timeouts and reconnection
- Provides mechanisms for sending and receiving messages
- Implements connection verification and status checking
- Manages socket configuration including keepalive options
- Implements synchronization for thread-safe operation

## Key Classes

### TCPClient
A class that manages a TCP socket connection to a server.

#### Key Methods
- `init()`: Initializes the connection to the specified host and port
- `send_message()`: Sends a message to the connected server
- `receive_message()`: Receives a message from the server with timeout
- `close()`: Closes the current connection
- `ensure_connected()`: Checks connection status and reconnects if necessary
- `check_connection_status()`: Returns the current connection status
- `set_timeouts()`: Configures socket timeout and keepalive parameters
- `verify_connection()`: Verifies that the connection is still valid

## Connection Management Features
- Configurable connect timeout
- Keepalive parameters (idle time, interval, count)
- Reconnection cooldown to prevent rapid connection attempts
- Thread-safe send operations with mutex protection
- Reusable send buffer to reduce memory allocation

## Integration Points
- Used for remote control or status reporting
- Can be integrated with other components for remote management
- Provides TCP communication capabilities for components that need it
- Note: According to the main README, this component may be removed in future versions