#ifndef WEBSERVER_H
#define WEBSERVER_H

#include "esp_err.h"
#include "esp_http_server.h"

class HtmlContent;

class WebServer {
public:
    /**
     * Get the singleton instance of the WebServer
     * @return Reference to the singleton instance
     */
    static WebServer& instance();

    /**
     * Initialize the web server configuration
     * @return ESP_OK on success, error code otherwise
     */
    esp_err_t init();

    /**
     * Start the web server
     * @return ESP_OK on success, error code otherwise
     */
    esp_err_t start();

    /**
     * Stop the web server
     * @return ESP_OK on success, error code otherwise
     */
    esp_err_t stop();

    /**
     * Restart the web server
     * @return ESP_OK on success, error code otherwise
     */
    esp_err_t restart();

    /**
     * Check if the web server is running
     * @return true if running, false otherwise
     */
    bool is_running() const;

    /**
     * Get the HTTP server handle for WebSocket integration
     * @return HTTP server handle or nullptr if not initialized
     */
    httpd_handle_t get_server_handle() const;
    // HTTP request handlers
    static esp_err_t error_handler(httpd_req_t *req, httpd_err_code_t err);
    static esp_err_t root_get_handler(httpd_req_t *req);
    static esp_err_t config_get_handler(httpd_req_t *req);
    static esp_err_t status_get_handler(httpd_req_t *req);
    static esp_err_t config_post_handler(httpd_req_t *req);
    static esp_err_t toggle_auto_mode_handler(httpd_req_t *req);
    static esp_err_t reset_config_handler(httpd_req_t *req);
    static esp_err_t restart_handler(httpd_req_t *req);
    static esp_err_t reset_wifi_handler(httpd_req_t *req);
    static esp_err_t relay_status_handler(httpd_req_t *req);
    static esp_err_t relay_control_handler(httpd_req_t *req);
    static esp_err_t set_rx_antenna_handler(httpd_req_t *req);
    static esp_err_t antenna_switch_handler(httpd_req_t *req);
    static esp_err_t config_export_handler(httpd_req_t *req);
    static esp_err_t config_import_handler(httpd_req_t *req);
    static esp_err_t relay_names_handler(httpd_req_t *req);
    static esp_err_t config_basic_handler(httpd_req_t *req);

    /**
     * Register URI handlers for the web server.
     * This must be called after init() and before start().
     * @return ESP_OK on success, error code otherwise
     */
    esp_err_t register_uri_handlers() const;

private:
    // Private constructor for singleton pattern
    WebServer();

    // Delete copy constructor and assignment operator
    WebServer(const WebServer&);
    WebServer& operator=(const WebServer&);
    
    // Member variables
    httpd_handle_t m_server;
    httpd_config_t m_config;
    static constexpr size_t MAX_POST_SIZE = 4096;
};

#endif // WEBSERVER_H
