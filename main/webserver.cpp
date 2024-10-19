#include "webserver.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "antenna_switch.h"
#include "wifi_manager.h"
#include "cJSON.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "cat_parser.h"
#include "html_content.h"
#include <algorithm>
#include <arpa/inet.h>
#include <string>
#include <memory>
#include <vector>

static const char *TAG = "WEBSERVER";

static httpd_handle_t server = nullptr;
static httpd_config_t config;

constexpr size_t MAX_POST_SIZE = 1024;

static esp_err_t root_get_handler(httpd_req_t *req)
{
    antenna_switch_config_t config;
    esp_err_t ret = antenna_switch_get_config(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get configuration: %s", esp_err_to_name(ret));
    }

    char ip_addr[16];
    ret = wifi_manager_get_ip_info(ip_addr, sizeof(ip_addr));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get IP address: %s", esp_err_to_name(ret));
        strcpy(ip_addr, "Unknown");
    }

    std::string resp_str = generate_root_html(config, ip_addr);
    
    httpd_resp_send(req, resp_str.c_str(), resp_str.length());
    return ESP_OK;
}

static esp_err_t config_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Entering config_get_handler");
    
    antenna_switch_config_t config;
    esp_err_t ret = antenna_switch_get_config(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get configuration: %s", esp_err_to_name(ret));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to get configuration");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Configuration retrieved successfully");
    ESP_LOGI(TAG, "Number of bands: %d, Number of antenna ports: %d", config.num_bands, config.num_antenna_ports);

    if (config.num_bands <= 0 || config.num_bands > MAX_BANDS) {
        ESP_LOGE(TAG, "Invalid number of bands: %d (should be between 1 and %d)", config.num_bands, MAX_BANDS);
        
        // Reset to a valid number of bands (e.g., 1)
        config.num_bands = 1;
        ESP_LOGI(TAG, "Resetting number of bands to %d", config.num_bands);
        
        // Save the corrected configuration to NVS
        esp_err_t save_ret = antenna_switch_set_config(&config);
        if (save_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save corrected configuration: %s", esp_err_to_name(save_ret));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save corrected configuration");
            return ESP_FAIL;
        }
        
        ESP_LOGI(TAG, "Corrected configuration saved successfully");
    }

    if (config.num_antenna_ports <= 0 || config.num_antenna_ports > MAX_ANTENNA_PORTS) {
        ESP_LOGE(TAG, "Invalid number of antenna ports: %d (should be between 1 and %d)", config.num_antenna_ports, MAX_ANTENNA_PORTS);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Invalid configuration: number of antenna ports out of range");
        return ESP_FAIL;
    }

    std::string resp_str = generate_config_html(config);
    if (resp_str.empty()) {
        ESP_LOGE(TAG, "Failed to generate HTML");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to generate HTML");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "HTML generated successfully, length: %d", resp_str.length());

    ESP_LOGI(TAG, "Sending response");
    esp_err_t send_ret = httpd_resp_send(req, resp_str.c_str(), resp_str.length());
    if (send_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send response: %s", esp_err_to_name(send_ret));
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Response sent successfully");
    return ESP_OK;
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    // TODO: Implement actual status retrieval
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "frequency", 14200000);
    cJSON_AddNumberToObject(root, "antenna", 2);

    char *json_string = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_string);

    free(json_string);
    cJSON_Delete(root);
    return ESP_OK;
}

static const httpd_uri_t status = {
    .uri       = "/status",
    .method    = HTTP_GET,
    .handler   = status_get_handler,
    .user_ctx  = nullptr
};

static const httpd_uri_t root = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = root_get_handler,
    .user_ctx  = nullptr
};

static const httpd_uri_t config_get = {
    .uri       = "/config",
    .method    = HTTP_GET,
    .handler   = config_get_handler,
    .user_ctx  = nullptr
};

static esp_err_t config_post_handler(httpd_req_t *req)
{
    char content[MAX_POST_SIZE];
    int ret = httpd_req_recv(req, content, std::min(req->content_len, sizeof(content) - 1));
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    content[ret] = '\0';

    antenna_switch_config_t new_config;
    memset(&new_config, 0, sizeof(antenna_switch_config_t));  // Initialize to zero
    new_config.auto_mode = (strstr(content, "auto_mode=on") != NULL);

    // Parse num_bands with input validation
    char *num_bands_str = strstr(content, "num_bands=");
    if (num_bands_str) {
        int num_bands = atoi(num_bands_str + strlen("num_bands="));
        if (num_bands > 0 && num_bands <= MAX_BANDS) {
            new_config.num_bands = num_bands;
        } else {
            ESP_LOGE(TAG, "Invalid number of bands: %d", num_bands);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid number of bands");
            return ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "Number of bands not specified");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Number of bands not specified");
        return ESP_FAIL;
    }

    // Parse num_antenna_ports with input validation
    char *num_antenna_ports_str = strstr(content, "num_antenna_ports=");
    if (num_antenna_ports_str) {
        int num_antenna_ports = atoi(num_antenna_ports_str + strlen("num_antenna_ports="));
        if (num_antenna_ports > 0 && num_antenna_ports <= MAX_ANTENNA_PORTS) {
            new_config.num_antenna_ports = num_antenna_ports;
        } else {
            ESP_LOGE(TAG, "Invalid number of antenna ports: %d", num_antenna_ports);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid number of antenna ports");
            return ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "Number of antenna ports not specified");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Number of antenna ports not specified");
        return ESP_FAIL;
    }

    for (int i = 0; i < new_config.num_bands; i++) {
        char param[30];
        snprintf(param, sizeof(param), "band_%d=", i);
        char *value = strstr(content, param);
        if (value) {
            char *end = strchr(value + strlen(param), '&');
            if (end) {
                *end = '\0';
                std::string band_key(value + strlen(param));
                *end = '&';
                
                auto it = band_info.find(band_key);
                if (it != band_info.end()) {
                    strncpy(new_config.bands[i].description, it->second.name, sizeof(new_config.bands[i].description) - 1);
                    new_config.bands[i].description[sizeof(new_config.bands[i].description) - 1] = '\0';
                    new_config.bands[i].start_freq = it->second.start_freq;
                    new_config.bands[i].end_freq = it->second.end_freq;
                }
            }
        }

        // Process antenna port checkboxes
        for (int j = 0; j < new_config.num_antenna_ports; j++) {
            snprintf(param, sizeof(param), "antenna_%d_%d=on", i, j);
            new_config.bands[i].antenna_ports[j] = (strstr(content, param) != NULL);
        }
    }

    esp_err_t err = antenna_switch_set_config(&new_config);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to set configuration");
        return ESP_FAIL;
    }

    // Update CAT parser configuration
    err = cat_parser_update_config();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to update CAT parser configuration: %s", esp_err_to_name(err));
    }

    httpd_resp_sendstr(req, "Configuration updated successfully. <a href='/'>Back to Home</a>");
    return ESP_OK;
}

static esp_err_t toggle_auto_mode_handler(httpd_req_t *req)
{
    antenna_switch_config_t config;
    esp_err_t ret = antenna_switch_get_config(&config);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to get configuration");
        return ESP_FAIL;
    }

    config.auto_mode = !config.auto_mode;

    ret = antenna_switch_set_config(&config);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to set configuration");
        return ESP_FAIL;
    }

    // Redirect back to the home page
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t reset_config_handler(httpd_req_t *req)
{
    antenna_switch_config_t default_config = {
        .auto_mode = true,
        .num_bands = 1,
        .num_antenna_ports = 1,
        .bands = {{
            .description = "Default",
            .start_freq = 0,
            .end_freq = 30000000,
            .antenna_ports = {true}
        }}
    };

    esp_err_t ret = antenna_switch_set_config(&default_config);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to reset configuration");
        return ESP_FAIL;
    }

    // Redirect back to the config page
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/config");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static const httpd_uri_t toggle_auto_mode = {
    .uri       = "/toggle-auto-mode",
    .method    = HTTP_POST,
    .handler   = toggle_auto_mode_handler,
    .user_ctx  = nullptr
};

static const httpd_uri_t reset_config = {
    .uri       = "/reset-config",
    .method    = HTTP_POST,
    .handler   = reset_config_handler,
    .user_ctx  = nullptr
};

static const httpd_uri_t config_post = {
    .uri       = "/config",
    .method    = HTTP_POST,
    .handler   = config_post_handler,
    .user_ctx  = nullptr
};
esp_err_t webserver_init()
{
    config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 32768;  // Further increased stack size

    char ip_addr[16];
    esp_err_t ret = wifi_manager_get_ip_info(ip_addr, sizeof(ip_addr));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get IP address: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Starting server on IP: %s, port: '%d'", ip_addr, config.server_port);
    ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error starting server: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Registering URI handlers");
    ret = httpd_register_uri_handler(server, &root);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register root URI handler: %s", esp_err_to_name(ret));
        goto error_handler;
    }

    ret = httpd_register_uri_handler(server, &config_get);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register config GET URI handler: %s", esp_err_to_name(ret));
        goto error_handler;
    }

    ret = httpd_register_uri_handler(server, &config_post);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register config POST URI handler: %s", esp_err_to_name(ret));
        goto error_handler;
    }

    ret = httpd_register_uri_handler(server, &status);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register status URI handler: %s", esp_err_to_name(ret));
        goto error_handler;
    }

    ret = httpd_register_uri_handler(server, &toggle_auto_mode);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register toggle auto mode URI handler: %s", esp_err_to_name(ret));
        goto error_handler;
    }

    ret = httpd_register_uri_handler(server, &reset_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register reset config URI handler: %s", esp_err_to_name(ret));
        goto error_handler;
    }

    ESP_LOGI(TAG, "Server started successfully");
    return ESP_OK;

error_handler:
    ESP_LOGE(TAG, "Error occurred during server initialization. Stopping server.");
    webserver_stop();
    return ret;
}


esp_err_t webserver_start()
{
    if (server == nullptr) {
        ESP_LOGI(TAG, "Starting normal webserver");
        esp_err_t ret = webserver_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize webserver: %s", esp_err_to_name(ret));
            return ret;
        }
    }
    return ESP_OK;
}

esp_err_t webserver_stop()
{
    if (server) {
        ESP_LOGI(TAG, "Stopping webserver");
        httpd_stop(server);
        server = nullptr;
    }
    return ESP_OK;
}

esp_err_t webserver_restart()
{
    ESP_LOGI(TAG, "Restarting webserver");
    webserver_stop();
    return webserver_start();
}
