# Restart Manager Component

## Overview
The Restart Manager component provides safety mechanisms to prevent boot-loops and manage system restarts. It tracks restart attempts and error states to ensure the system can recover from failure conditions.

## Responsibilities
- Tracks the number of consecutive restart attempts
- Prevents boot-loops by limiting restart attempts
- Stores error states in non-volatile storage for post-restart analysis
- Provides an interface to clear restart counters after successful operation
- Acts as a safety mechanism for system stability

## Key Classes

### RestartManager
A utility class that provides static methods for restart management.

#### Key Methods
- `check_restart_count()`: Checks if the system has restarted too many times and prevents boot-loops
- `clear_restart_count()`: Resets the restart counter after successful system operation
- `store_error_state()`: Stores error information in NVS for later analysis

## Safety Features
- Limits maximum restart attempts to prevent endless boot-loops
- Stores error codes to help diagnose repeated failures
- Keys for persistent storage:
  - RESTART_COUNTER_KEY: Tracks consecutive restart attempts
  - ERROR_STATE_KEY: Stores error codes from previous failures

## Integration Points
- Used by the System Initializer during startup
- Called periodically after successful operation to reset counters
- Provides diagnostics information for debugging
- Helps ensure the system can recover from transient failure states