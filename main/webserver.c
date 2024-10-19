#include "webserver.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "antenna_switch.h"

static const char *TAG = "WEBSERVER";

static httpd_handle_t server = NULL;

static esp_err_t root_get_handler(httpd_req_t *req)
{
    const char* resp_str = "<html><body><h1>Antenna Switch Controller</h1><p>Welcome to the Antenna Switch Controller web interface.</p></body></html>";
    httpd_resp_send(req, resp_str, strlen(resp_str));
    return ESP_OK;
}

static esp_err_t config_get_handler(httpd_req_t *req)
{
    antenna_switch_config_t config;
    esp_err_t ret = antenna_switch_get_config(&config);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to get configuration");
        return ESP_FAIL;
    }

    char *resp_str = malloc(1024);
    if (resp_str == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }

    int len = snprintf(resp_str, 1024, 
        "<html><body><h2>Antenna Switch Configuration</h2>"
        "<p>Auto mode: %s</p>"
        "<p>Number of bands: %d</p>"
        "<table border='1'><tr><th>Start Freq</th><th>End Freq</th><th>Antenna</th></tr>",
        config.auto_mode ? "ON" : "OFF", config.num_bands);

    for (int i = 0; i < config.num_bands; i++) {
        len += snprintf(resp_str + len, 1024 - len,
            "<tr><td>%u</td><td>%u</td><td>%u</td></tr>",
            config.bands[i].start_freq, config.bands[i].end_freq, config.bands[i].antenna);
    }

    len += snprintf(resp_str + len, 1024 - len, "</table></body></html>");

    httpd_resp_send(req, resp_str, len);
    free(resp_str);
    return ESP_OK;
}

static const httpd_uri_t root = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = root_get_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t config = {
    .uri       = "/config",
    .method    = HTTP_GET,
    .handler   = config_get_handler,
    .user_ctx  = NULL
};

esp_err_t webserver_init(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;

    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGI(TAG, "Error starting server!");
        return ret;
    }

    ESP_LOGI(TAG, "Registering URI handlers");
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &config);

    return ESP_OK;
}

esp_err_t webserver_start(void)
{
    if (server == NULL) {
        return webserver_init();
    }
    return ESP_OK;
}

esp_err_t webserver_stop(void)
{
    if (server) {
        httpd_stop(server);
        server = NULL;
    }
    return ESP_OK;
}
