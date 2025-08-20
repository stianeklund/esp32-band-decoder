# RS232 Band Decoder - Antenna Switch API Documentation

## Overview

This document provides comprehensive documentation for the HTTP REST API of the RS232 Band Decoder antenna switch controller. The system provides both automatic band-based antenna switching and manual relay control capabilities.

## Base URL

All API endpoints are relative to your device's IP address:
```
http://<device-ip>/
```

## Authentication

Currently, no authentication is required for API access.

## API Endpoints

### 1. System Status and Information

#### GET `/`
**Description:** Main dashboard page with current system status and controls  
**Content-Type:** `text/html`  
**Returns:** HTML page with real-time status updates and relay controls

#### GET `/status`
**Description:** Get current system status including frequency, active antennas, and transmission state  
**Content-Type:** `application/json`

**Response Example:**
```json
{
  "frequency": 14205000,
  "antenna": "Antenna 3",
  "transmitting": false,
  "data_source": "Serial",
  "available_antennas": [1, 2, 3, 4],
  "antenna_b": "Antenna 10",
  "frequency_b": 0,
  "transmitting_b": false,
  "data_source_b": "Serial",
  "available_antennas_b": []
}
```

**Response Fields:**
- `frequency`: Current frequency in Hz (Radio A)
- `antenna`: Active antenna description (Radio A)
- `transmitting`: Transmission state (Radio A)
- `data_source`: Source of frequency data ("Serial", "MQTT", or "None")
- `available_antennas`: Array of antenna numbers available for current frequency
- `antenna_b`: Active antenna for Radio B (if dual-radio mode)
- `frequency_b`: Current frequency in Hz (Radio B)
- `transmitting_b`: Transmission state (Radio B)
- `data_source_b`: Source of frequency data for Radio B
- `available_antennas_b`: Available antennas for Radio B

### 2. Relay Control

#### GET `/relay/status`
**Description:** Get current state of all relays  
**Content-Type:** `application/json`

**Response Example:**
```json
{
  "states": 65535
}
```

**Response Fields:**
- `states`: 16-bit bitmask representing relay states (bit 0 = relay 1, bit 1 = relay 2, etc.)
  - `0` = relay active (antenna connected)
  - `1` = relay inactive (antenna disconnected)

#### POST `/relay/control`
**Description:** Control individual relay state  
**Content-Type:** `application/json`

**Request Body:**
```json
{
  "relay": 3,
  "state": true
}
```

**Request Fields:**
- `relay`: Relay number (1-16)
  - Relays 1-8: Radio A
  - Relays 9-16: Radio B
- `state`: Desired relay state
  - `true` = activate relay (connect antenna)
  - `false` = deactivate relay (disconnect antenna)

**Response Example:**
```json
{
  "state": true
}
```

**Response Fields:**
- `state`: Actual relay state after command execution

**Notes:**
- Relay control respects interlock policies configured in the system
- Only one antenna per radio can be active at a time
- Setting a relay to active will automatically deactivate other relays for the same radio

### 3. Configuration Management

#### GET `/config`
**Description:** Configuration interface page  
**Content-Type:** `text/html`  
**Returns:** HTML form for system configuration

#### POST `/config`
**Description:** Update system configuration  
**Content-Type:** `application/json`

**Request Body Example:**
```json
{
  "auto_mode": true,
  "allow_concurrent_data_sources": false,
  "radio_operation_mode": "SINGLE_A",
  "interlock_auto_resolves_conflict": true,
  "auto_restore_on_conflict_resolution": true,
  "radio_restore_delay_ms": 200,
  "uart_baud_rate": 9600,
  "uart_parity": 0,
  "uart_stop_bits": 1,
  "uart_flow_ctrl": 0,
  "uart_tx_pin": 17,
  "uart_rx_pin": 16,
  "ptt_input_radio_a": -1,
  "ptt_input_radio_a_active_high": true,
  "ptt_input_radio_b": -1,
  "ptt_input_radio_b_active_high": true,
  "mqtt_enabled": false,
  "mqtt_broker": "192.168.1.100",
  "mqtt_port": 1883,
  "mqtt_rig_id": "radio1",
  "mqtt_username": "",
  "mqtt_password": "",
  "mqtt_client_id": "core-mosquitto",
  "mqtt_topic": "omnirig/frequent/radio_info",
  "num_bands": 8,
  "num_antenna_ports": 6,
  "relay_names": ["Dipole", "Beam", "Vertical", "", "", "", "", "", "", "", "", "", "", "", "", ""],
  "bands": [
    {
      "description": "20m",
      "antenna_ports_a": [true, true, false, false, false, false, false, false],
      "antenna_ports_b": [false, false, true, true, false, false, false, false]
    }
  ]
}
```

**Key Configuration Fields:**
- `auto_mode`: Enable automatic band-based antenna switching
- `radio_operation_mode`: Operating mode
  - `"SINGLE_A"`: Only Radio A active
  - `"ALTERNATING_AB"`: Radio A or B, not simultaneously
  - `"CONCURRENT_AB"`: Both radios can be active simultaneously
- `uart_*`: Serial port configuration for CAT control
- `ptt_input_*`: PTT input pin configuration (-1 to disable)
- `mqtt_*`: MQTT configuration for remote frequency data
- `relay_names`: Custom names for each relay (16 elements)
- `bands`: Band configuration with antenna port assignments

#### GET `/api/config/export`
**Description:** Export current configuration as JSON file  
**Content-Type:** `application/json`  
**Headers:** `Content-Disposition: attachment; filename="kc868_config.json"`

#### POST `/api/config/import`
**Description:** Import configuration from JSON  
**Content-Type:** `application/json`

**Request Body:** Complete configuration JSON (same format as export)

**Response Example:**
```json
{
  "status": "success",
  "message": "Configuration imported successfully"
}
```

### 4. System Control

#### POST `/toggle-auto-mode`
**Description:** Toggle automatic antenna switching mode  
**Content-Type:** `application/x-www-form-urlencoded`  
**Response:** HTTP 303 redirect to `/`

#### POST `/reset-config`
**Description:** Reset configuration to factory defaults  
**Content-Type:** `application/x-www-form-urlencoded`  
**Response:** HTTP 303 redirect to `/config`

#### POST `/restart`
**Description:** Restart the device  
**Content-Type:** `text/plain`  
**Response:** `"Restarting..."`

#### POST `/reset-wifi`
**Description:** Clear WiFi credentials and reset network settings  
**Content-Type:** `application/x-www-form-urlencoded`  
**Response:** HTTP 303 redirect to `/`

## Band Information

The system supports the following amateur radio bands with predefined frequency ranges:

| Band | Start Frequency (Hz) | End Frequency (Hz) |
|------|--------------------|--------------------|
| 160m | 1,800,000 | 2,000,000 |
| 80m | 3,500,000 | 4,000,000 |
| 40m | 7,000,000 | 7,300,000 |
| 30m | 10,100,000 | 10,150,000 |
| 20m | 14,000,000 | 14,350,000 |
| 17m | 18,068,000 | 18,168,000 |
| 15m | 21,000,000 | 21,450,000 |
| 12m | 24,890,000 | 24,990,000 |
| 10m | 28,000,000 | 29,700,000 |
| 6m | 50,000,000 | 54,000,000 |

## Relay Mapping

The system supports up to 16 relays with the following mapping:

- **Relays 1-8**: Radio A antenna ports
- **Relays 9-16**: Radio B antenna ports

## Data Sources

The system can receive frequency information from multiple sources:

1. **Serial/UART**: CAT control protocol from connected radio
2. **MQTT**: Network-based frequency updates
3. **Manual**: User-controlled operation

## Operation Modes

### Auto Mode
When enabled, the system automatically selects the appropriate antenna based on:
- Current operating frequency
- Band configuration
- Available antenna ports for the detected band

### Manual Mode
When disabled, all antenna switching must be performed manually via the relay control API.

## Error Handling

### HTTP Status Codes
- `200 OK`: Successful operation
- `303 See Other`: Redirect after form submission
- `400 Bad Request`: Invalid request data
- `500 Internal Server Error`: System error

### Error Response Format
```json
{
  "status": "error",
  "message": "Error description",
  "error_code": "ESP_ERROR_CODE"
}
```

## Rate Limiting

No explicit rate limiting is implemented, but the system has the following constraints:
- Maximum 3 concurrent connections
- 5-second receive timeout per request
- Maximum POST size: 8KB (32KB for config import)

## Security Considerations

- No authentication required (suitable for local network use only)
- No HTTPS support (plain HTTP only)
- Configuration changes take effect immediately
- System restart may be required for some configuration changes

## Example Usage

### Python Example - Get Status
```python
import requests

response = requests.get('http://192.168.1.100/status')
status = response.json()
print(f"Current frequency: {status['frequency']/1000000:.3f} MHz")
print(f"Active antenna: {status['antenna']}")
```

### Python Example - Switch Antenna
```python
import requests

# Activate relay 3 (antenna 3)
payload = {'relay': 3, 'state': True}
response = requests.post('http://192.168.1.100/relay/control', json=payload)
result = response.json()
print(f"Relay 3 state: {result['state']}")
```

### cURL Example - Get Relay Status
```bash
curl -X GET http://192.168.1.100/relay/status
```

### cURL Example - Control Relay
```bash
curl -X POST http://192.168.1.100/relay/control \
  -H "Content-Type: application/json" \
  -d '{"relay": 5, "state": true}'
```

## Notes

- All frequencies are specified in Hz
- Relay numbers are 1-indexed (1-16)
- The system uses active-low relay logic internally
- Configuration changes may require a system restart to take full effect
- MQTT configuration requires restart to apply changes