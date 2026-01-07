#ifndef WEBSOCKET_SERVER_H
#define WEBSOCKET_SERVER_H

#include "esp_err.h"
#include "esp_http_server.h"
#include "websocket_protocol.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "freertos/queue.h"
#include <vector>
#include <memory>

// Forward declarations
#include "antenna_switch.h"

// Forward declaration for friend class
class WebSocketHandlers;

class WebSocketServer {
    friend class WebSocketHandlers;
    
public:
    // Singleton access
    static WebSocketServer& instance();
    
    // Delete copy constructor and assignment operator
    WebSocketServer(const WebSocketServer&) = delete;
    WebSocketServer& operator=(const WebSocketServer&) = delete;

    // Core functionality
    esp_err_t init();
    esp_err_t start();
    esp_err_t stop();
    bool is_running() const;
    
    // Register with HTTP server for WebSocket upgrade
    esp_err_t register_with_http_server(httpd_handle_t server);
    
    // Event broadcasting
    esp_err_t broadcast_status_update();
    esp_err_t broadcast_relay_state_change(int relay_id, bool state);
    esp_err_t broadcast_transmit_state_change(bool transmitting);
    esp_err_t broadcast_config_change();
    
    // Client management
    size_t get_active_client_count() const;
    esp_err_t disconnect_all_clients();

private:
    struct WebSocketClient {
        int sockfd;
        ws_client_subscriptions_t subscriptions;
        uint64_t last_activity;          // Only inbound activity (messages, PONGs)
        uint64_t last_ping_sent;         // Track when we sent last ping
        uint8_t missed_pong_count;       // Count consecutive missed PONGs
        bool is_active;
        bool awaiting_pong;              // Flag: waiting for PONG response
        
        WebSocketClient(int fd) : sockfd(fd), last_activity(0), last_ping_sent(0), 
                                 missed_pong_count(0), is_active(true), awaiting_pong(false) {
            // Default subscriptions - all enabled
            subscriptions.status_updates = true;
            subscriptions.relay_state_changes = true;
            subscriptions.transmit_state_changes = true;
            subscriptions.config_changes = true;
        }
    };

    WebSocketServer();
    ~WebSocketServer();
    
    // WebSocket handlers
    static esp_err_t websocket_handler(httpd_req_t *req);
    
    // HTTP fallback handlers
    static esp_err_t websocket_status_handler(httpd_req_t *req);
    static esp_err_t websocket_relay_handler(httpd_req_t *req);
    static esp_err_t websocket_antenna_handler(httpd_req_t *req);
    esp_err_t handle_websocket_message(int sockfd, uint8_t *buf, size_t len);
    
    // Message processing
    esp_err_t parse_message(const char* json_str, ws_message_t* message);
    esp_err_t handle_request(int sockfd, const ws_message_t* message);
    esp_err_t send_response(int sockfd, const char* request_id, ws_message_type_t type, const char* data);
    esp_err_t send_error(int sockfd, const char* request_id, const char* error_msg);
    
    // Event broadcasting helpers
    esp_err_t broadcast_event(ws_event_type_t event_type, const char* data);
    esp_err_t send_to_client(int sockfd, const char* message);
    
    // Client management
    esp_err_t add_client(int sockfd);
    esp_err_t remove_client(int sockfd);
    WebSocketClient* find_client(int sockfd);
    esp_err_t cleanup_inactive_clients();
    
    // Keepalive and cleanup
    static void keepalive_timer_callback(TimerHandle_t timer);
    esp_err_t send_ping_to_client(int sockfd);
    
    // Buffer pool for WebSocket frame processing
    struct FrameBuffer {
        uint8_t* data;
        size_t size;
        bool in_use;
        
        FrameBuffer(size_t buffer_size) : size(buffer_size), in_use(false) {
            data = (uint8_t*)malloc(buffer_size);
        }
        
        ~FrameBuffer() {
            if (data) {
                free(data);
            }
        }
    };
    
    // Buffer pool management
    esp_err_t init_buffer_pool();
    esp_err_t destroy_buffer_pool();
    FrameBuffer* get_buffer();
    void return_buffer(FrameBuffer* buffer);
    
    // Member variables
    httpd_handle_t m_http_server;
    std::vector<std::unique_ptr<WebSocketClient>> m_clients;
    mutable SemaphoreHandle_t m_clients_mutex;
    TimerHandle_t m_keepalive_timer;
    bool m_initialized;
    bool m_running;
    
    // Buffer pool for performance optimization
    std::vector<std::unique_ptr<FrameBuffer>> m_buffer_pool;
    mutable SemaphoreHandle_t m_buffer_pool_mutex;
    static const size_t WS_BUFFER_POOL_SIZE = 4;  // Pool of 4 buffers
    static const size_t WS_BUFFER_SIZE = WS_MAX_MESSAGE_SIZE + 128;  // Extra space for headers

    // Mutex for protecting static response buffers from concurrent access
    mutable SemaphoreHandle_t m_response_buffer_mutex;

    static const char* TAG;
};

// C-style function wrappers for integration with existing code
extern "C" {
    esp_err_t websocket_server_init();
    esp_err_t websocket_server_start();
    esp_err_t websocket_server_stop();
    bool websocket_server_is_running();
    esp_err_t websocket_server_register_with_http(httpd_handle_t server);
}

#endif // WEBSOCKET_SERVER_H