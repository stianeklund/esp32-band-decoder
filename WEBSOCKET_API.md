# RS232 Band Decoder WebSocket API

A real-time API for controlling antenna switching and monitoring system status.

## Quick Start

**Connect to**: `ws://your-device-ip/ws`

**Send JSON messages** in this format:
```json
{
  "id": "unique-id",
  "type": "request",
  "action": "action_name",
  "data": { /* parameters */ }
}
```

**Receive responses** in this format:
```json
{
  "id": "unique-id",
  "type": "response",
  "data": { /* results */ }
}
```

## Essential Concepts

### Frequency-Based Antenna Filtering
🔑 **Key Concept**: The system only allows switching between antennas that are **configured for the current frequency/band**.

- Each antenna is configured for specific frequency ranges (bands)
- `available_antennas` in status responses shows only compatible antennas
- Next/previous switching cycles only through compatible antennas
- Direct antenna selection validates compatibility before switching

### Radio Selection
- `"A"`: Primary radio (relays 1-8)  
- `"B"`: Secondary radio (relays 9-16) - if dual-radio mode enabled

---

## API Actions

### 1. Get System Status

**Purpose**: Get current frequency, active antenna, and available antennas for current band.

```json
// REQUEST
{
  "id": "status-1",
  "type": "request",
  "action": "status"
}

// RESPONSE
{
  "id": "status-1", 
  "type": "response",
  "data": {
    "frequency": 14205000,           // Hz
    "frequency_mhz": 14.205,         // MHz (convenience)
    "antenna": "Antenna 3",          // Currently active
    "transmitting": false,
    "data_source": "Serial",         // "Serial", "MQTT", or "None"
    "available_antennas": [1,2,3,4], // Compatible with current frequency
    
    // Dual-radio fields (if enabled)
    "antenna_b": "None",
    "frequency_b": 0,
    "transmitting_b": false,
    "data_source_b": "Serial",
    "available_antennas_b": []
  }
}
```

**Implementation Notes:**
- Call this first to understand current system state
- `available_antennas` changes based on frequency - always check before switching
- Single-radio mode: only `_b` fields will be present if dual-radio is configured

### 2. Switch to Next/Previous Antenna

**Purpose**: Cycle through antennas compatible with current frequency.

```json
// REQUEST - Next antenna
{
  "id": "next-1",
  "type": "request", 
  "action": "antenna_switch",
  "data": {
    "radio": "A",
    "action": "next"      // or "previous"
  }
}

// RESPONSE
{
  "id": "next-1",
  "type": "response",
  "data": {
    "status": "success",
    "radio": "A",
    "frequency": 14205000,
    "frequency_mhz": 14.205,
    "band": "20M",
    "previous_antenna": 2,
    "new_antenna": 3,
    "available_antennas": [1,2,3,4]
  }
}
```

**Implementation Notes:**
- Wraparound: last antenna → first antenna, first antenna → last antenna
- Only cycles through `available_antennas` for current frequency
- If current antenna unknown, starts with first available

### 3. Select Specific Antenna

**Purpose**: Switch directly to a specific antenna number.

```json
// REQUEST - Direct selection
{
  "id": "select-5",
  "type": "request",
  "action": "antenna_switch", 
  "data": {
    "radio": "A",
    "antenna": 5           // Antenna number (1-16)
  }
}

// RESPONSE - Same as above
{
  "id": "select-5",
  "type": "response", 
  "data": {
    "status": "success",
    "radio": "A",
    "frequency": 14205000,
    "frequency_mhz": 14.205,
    "band": "20M", 
    "previous_antenna": 3,
    "new_antenna": 5,
    "available_antennas": [1,2,3,4,5,6]
  }
}
```

**Implementation Notes:**
- Antenna must be in `available_antennas` for current frequency
- Returns error if antenna not compatible with current frequency
- Recommended: Check `available_antennas` from status before selecting

### 4. Control Individual Relays

**Purpose**: Direct relay control (bypasses frequency filtering).

```json
// REQUEST
{
  "id": "relay-1",
  "type": "request",
  "action": "relay_control",
  "data": {
    "relay": 5,           // Relay number (1-16)
    "state": true         // true=ON, false=OFF  
  }
}

// RESPONSE
{
  "id": "relay-1",
  "type": "response",
  "data": {
    "state": true         // Actual state after operation
  }
}
```

**Implementation Notes:**
- Direct hardware control - bypasses frequency compatibility checks
- Use for manual override or testing
- For normal operation, prefer `antenna_switch` actions

### 5. Get Antenna Names

**Purpose**: Get custom names/labels for each antenna/relay.

```json
// REQUEST
{
  "id": "names-1", 
  "type": "request",
  "action": "relay_names"
}

// RESPONSE  
{
  "id": "names-1",
  "type": "response",
  "data": {
    "1": "40M Yagi North",
    "2": "40M Yagi South", 
    "3": "20M Beam",
    "4": "20M Vertical",
    "5": "15M Yagi",
    "6": "10M Beam",
    "7": "Relay 7",        // Default name if not customized
    "8": "Relay 8",
    // ... continues through 16
  }
}
```

### 6. Get Basic Configuration

**Purpose**: Get system configuration info.

```json
// REQUEST
{
  "id": "config-1",
  "type": "request", 
  "action": "config_basic"
}

// RESPONSE
{
  "id": "config-1",
  "type": "response",
  "data": {
    "auto_mode": true,                    // Automatic switching enabled
    "num_bands": 8,                       // Number of configured bands
    "num_antenna_ports": 8,               // Number of antenna ports per radio
    "mqtt_enabled": false,                // MQTT integration status
    "radio_operation_mode": "SINGLE_A"    // "SINGLE_A", "ALTERNATING_AB", "CONCURRENT_AB"
  }
}
```

---

## Real-Time Events

**Purpose**: Receive automatic notifications when system state changes.

### Subscribe to Events

```json
// REQUEST - Subscribe to events
{
  "id": "sub-1",
  "type": "request", 
  "action": "subscribe",
  "data": {
    "events": ["relay_state_changes", "status_updates"]
  }
}

// RESPONSE
{
  "id": "sub-1",
  "type": "response",
  "data": {
    "status": "success",
    "message": "Subscribed to events"
  }
}
```

**Available Event Types:**
- `status_updates` - Complete status changes (includes current frequency)
- `relay_state_changes` - Individual relay on/off
- `transmit_state_changes` - TX/RX state changes
- `config_changes` - Configuration updates

**Event Subscription Best Practices:**

💡 **Frequency Information**: Current frequency is included in `status_updates` events, providing frequency data without overwhelming the system with high-frequency updates during radio tuning. This is the recommended way to get frequency information.

**Recommended Subscription Patterns:**

```json
// For antenna switching UIs (most common)
{
  "events": ["relay_state_changes", "status_updates"]
}

// For logging/monitoring applications  
{
  "events": ["status_updates", "transmit_state_changes", "config_changes"]
}

// For minimal traffic (status polling alternative)
{
  "events": ["relay_state_changes"]
}

// For complete system monitoring
{
  "events": ["status_updates", "relay_state_changes", "transmit_state_changes", "config_changes"]
}
```

💡 **Tip**: `status_updates` events include the current frequency along with other system state, providing frequency information without the noise of every frequency change.

### Unsubscribe from Events

```json
// REQUEST - Unsubscribe from specific events
{
  "id": "unsub-1",
  "type": "request",
  "action": "unsubscribe", 
  "data": {
    "events": ["status_updates"]  // Stop receiving these events
  }
}

// RESPONSE
{
  "id": "unsub-1",
  "type": "response",
  "data": {
    "status": "success", 
    "message": "Unsubscribed from events"
  }
}
```

### Event Examples

```json
// Relay changed
{
  "type": "event",
  "event": "relay_state_changed", 
  "data": {
    "relay": 3,
    "state": true
  }
}


// Full status update (includes frequency)
{
  "type": "event", 
  "event": "status_update",
  "data": {
    "frequency": 21205000,
    "frequency_mhz": 21.205,
    "antenna": "Antenna 5", 
    "transmitting": false,
    "available_antennas": [1,5,6,7]  // Updates when frequency changes affect antenna availability
  }
}
```

---

## Error Handling

**All errors return:**
```json
{
  "id": "your-request-id",
  "type": "error", 
  "data": {
    "status": "error",
    "message": "Descriptive error message"
  }
}
```

**Common Errors:**

| Error Message | Cause | Solution |
|---------------|-------|----------|
| `"Invalid antenna switch data"` | Missing required fields | Include either `action` or `antenna` parameter |
| `"Invalid radio (must be 'A' or 'B')"` | Wrong radio value | Use `"A"` or `"B"` |  
| `"Requested antenna not available for current frequency"` | Antenna incompatible with frequency | Check `available_antennas` in status first |
| `"No frequency available for specified radio"` | No CAT/MQTT data | Ensure radio is connected and sending frequency data |
| `"No antennas available for current frequency"` | No configured antennas for band | Configure antennas for this frequency range |
| `"Failed to set relay"` | Hardware error | Check relay controller hardware |
| `"Invalid relay ID: X"` | Relay number out of range | Use relay numbers 1-16 |

---

## Implementation Guide

### Basic Client Pattern

```javascript
// 1. Connect
const ws = new WebSocket('ws://192.168.1.100/ws');

// 2. Handle connection
ws.onopen = async () => {
  console.log('Connected to antenna controller');
  
  // Get initial status
  const status = await sendRequest('status', {});
  console.log('Available antennas:', status.available_antennas);
  
  // Subscribe to events
  await sendRequest('subscribe', {
    events: ['relay_state_changes', 'status_updates']
  });
};

// 3. Handle messages
ws.onmessage = (event) => {
  const msg = JSON.parse(event.data);
  
  if (msg.type === 'response') {
    handleResponse(msg.id, msg.data);
  } else if (msg.type === 'event') {
    handleRealtimeEvent(msg.event, msg.data);
  } else if (msg.type === 'error') {
    console.error('API Error:', msg.data.message);
  }
};

// 4. Helper function
function sendRequest(action, data) {
  return new Promise((resolve, reject) => {
    const id = 'req-' + Date.now();
    
    // Store promise resolver
    pendingRequests[id] = { resolve, reject };
    
    // Send request
    ws.send(JSON.stringify({
      id,
      type: 'request',
      action,
      data
    }));
    
    // Timeout after 5 seconds
    setTimeout(() => {
      if (pendingRequests[id]) {
        delete pendingRequests[id];
        reject(new Error('Request timeout'));
      }
    }, 5000);
  });
}
```

### UI Integration Pattern

```javascript
// Safe antenna selection with validation
async function selectAntenna(antennaNumber) {
  try {
    // 1. Get current status to check availability
    const status = await sendRequest('status', {});
    
    // 2. Validate antenna is available 
    if (!status.available_antennas.includes(antennaNumber)) {
      throw new Error(`Antenna ${antennaNumber} not available for current frequency (${status.frequency_mhz} MHz)`);
    }
    
    // 3. Switch antenna
    const result = await sendRequest('antenna_switch', {
      radio: 'A',
      antenna: antennaNumber
    });
    
    console.log(`Switched to antenna ${result.new_antenna}`);
    return result;
    
  } catch (error) {
    console.error('Antenna selection failed:', error.message);
    throw error;
  }
}

// Real-time UI updates
function handleRealtimeEvent(eventType, data) {
  switch (eventType) {
    case 'status_update':
      // Update frequency display
      document.getElementById('frequency').textContent = 
        data.frequency_mhz.toFixed(3) + ' MHz';
      
      // Update antenna display
      document.getElementById('antenna').textContent = data.antenna;
      
      // Update available antennas (they may have changed due to frequency change!)
      updateAvailableAntennas(data.available_antennas);
      break;
      
    case 'relay_state_changed': 
      // Update antenna button states
      updateAntennaButton(data.relay, data.state);
      break;
  }
}

function updateAvailableAntennas(availableAntennas) {
  // Update UI to show only available antennas
  for (let i = 1; i <= 16; i++) {
    const button = document.getElementById(`antenna-${i}`);
    if (button) {
      button.disabled = !availableAntennas.includes(i);
      if (button.disabled) {
        button.title = 'Not available for current frequency';
      }
    }
  }
}

async function refreshAvailableAntennas() {
  const status = await sendRequest('status', {});
  updateAvailableAntennas(status.available_antennas);
}
```

### Connection Management

```javascript
class AntennaController {
  constructor(host) {
    this.host = host;
    this.ws = null;
    this.reconnectTimer = null;
    this.pendingRequests = new Map();
  }
  
  connect() {
    this.ws = new WebSocket(`ws://${this.host}/ws`);
    
    this.ws.onopen = () => {
      console.log('Connected to antenna controller');
      this.clearReconnectTimer();
      this.onConnected?.();
    };
    
    this.ws.onclose = (event) => {
      console.log('Connection closed:', event.reason);
      this.scheduleReconnect();
    };
    
    this.ws.onerror = (error) => {
      console.error('WebSocket error:', error);
    };
    
    this.ws.onmessage = (event) => {
      const msg = JSON.parse(event.data);
      this.handleMessage(msg);
    };
  }
  
  scheduleReconnect() {
    if (this.reconnectTimer) return;
    
    this.reconnectTimer = setTimeout(() => {
      console.log('Attempting to reconnect...');
      this.connect();
    }, 5000);
  }
  
  clearReconnectTimer() {
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
  }
  
  async sendRequest(action, data) {
    if (this.ws?.readyState !== WebSocket.OPEN) {
      throw new Error('Not connected to antenna controller');
    }
    
    return new Promise((resolve, reject) => {
      const id = `req-${Date.now()}-${Math.random().toString(36).substr(2, 9)}`;
      
      this.pendingRequests.set(id, { resolve, reject });
      
      this.ws.send(JSON.stringify({
        id,
        type: 'request',
        action,
        data: data || {}
      }));
      
      // Request timeout
      setTimeout(() => {
        if (this.pendingRequests.has(id)) {
          this.pendingRequests.delete(id);
          reject(new Error(`Request timeout: ${action}`));
        }
      }, 10000);
    });
  }
  
  handleMessage(msg) {
    if (msg.type === 'response' || msg.type === 'error') {
      const pending = this.pendingRequests.get(msg.id);
      if (pending) {
        this.pendingRequests.delete(msg.id);
        if (msg.type === 'error') {
          pending.reject(new Error(msg.data.message));
        } else {
          pending.resolve(msg.data);
        }
      }
    } else if (msg.type === 'event') {
      this.onEvent?.(msg.event, msg.data);
    }
  }
}

// Usage
const controller = new AntennaController('192.168.1.100');

controller.onConnected = async () => {
  // Subscribe to events
  await controller.sendRequest('subscribe', {
    events: ['relay_state_changes', 'status_updates']
  });
  
  // Get initial status
  const status = await controller.sendRequest('status', {});
  console.log('System ready:', status);
};

controller.onEvent = (eventType, data) => {
  console.log('Event:', eventType, data);
  // Update your UI here
};

controller.connect();
```

## Technical Details

**Connection Limits:** 5 concurrent clients  
**Message Size:** 8KB maximum per frame  
**Keepalive:** 30 second ping/pong  
**Timeout:** 60 seconds idle timeout  
**Protocol:** RFC 6455 WebSocket over ESP-IDF native implementation

**Performance:**
- Message processing: <5ms typical
- Event delivery: <10ms typical  
- Connection setup: <100ms typical
- Max throughput: 100+ messages/second

**Reliability:**
- Automatic reconnection recommended
- Handle connection drops gracefully
- Validate responses for all requests
- Subscribe to events after each reconnection