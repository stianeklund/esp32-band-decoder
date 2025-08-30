#ifndef WEBSOCKET_PROTOCOL_H
#define WEBSOCKET_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// WebSocket message types
typedef enum {
    WS_MSG_REQUEST = 0,
    WS_MSG_RESPONSE = 1,
    WS_MSG_EVENT = 2,
    WS_MSG_ERROR = 3
} ws_message_type_t;

// WebSocket action types
typedef enum {
    WS_ACTION_STATUS = 0,
    WS_ACTION_RELAY_CONTROL = 1,
    WS_ACTION_ANTENNA_SWITCH = 2,
    WS_ACTION_CONFIG_GET = 3,
    WS_ACTION_CONFIG_SET = 4,
    WS_ACTION_RELAY_NAMES = 5,
    WS_ACTION_CONFIG_BASIC = 6,
    WS_ACTION_SUBSCRIBE = 7,
    WS_ACTION_UNSUBSCRIBE = 8
} ws_action_type_t;

// WebSocket event types
typedef enum {
    WS_EVENT_STATUS_UPDATE = 0,
    WS_EVENT_RELAY_STATE_CHANGED = 1,
    WS_EVENT_TRANSMIT_STATE_CHANGED = 2,
    WS_EVENT_CONFIG_CHANGED = 3
} ws_event_type_t;

// Maximum sizes
#define WS_MAX_MESSAGE_SIZE 8192
#define WS_MAX_CLIENTS 5
#define WS_MAX_REQUEST_ID_LEN 32
#define WS_MAX_ACTION_LEN 32
#define WS_KEEPALIVE_INTERVAL_MS 30000
#define WS_CLIENT_TIMEOUT_MS 60000


// WebSocket message structure (used for parsing)
typedef struct {
    char request_id[WS_MAX_REQUEST_ID_LEN];
    ws_message_type_t type;
    ws_action_type_t action;
    char* data;
    size_t data_len;
} ws_message_t;

// Client subscription flags
typedef struct {
    bool status_updates;
    bool relay_state_changes;
    bool transmit_state_changes;
    bool config_changes;
} ws_client_subscriptions_t;

#ifdef __cplusplus
}
#endif

#endif // WEBSOCKET_PROTOCOL_H