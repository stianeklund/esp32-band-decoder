#ifndef HTML_CONTENT_H
#define HTML_CONTENT_H

#include <map>
#include <string>
#include <string_view> // Ensure string_view is included
#include <vector>
#include "antenna_switch.h"
#include "esp_http_server.h" // Required for httpd_req_t

class HtmlContent {
public:
    // Deleted copy/move constructors and assignment operators
    HtmlContent(const HtmlContent&) = delete;
    HtmlContent& operator=(const HtmlContent&) = delete;
    HtmlContent(HtmlContent&&) = delete;
    HtmlContent& operator=(HtmlContent&&) = delete;

    static const char *HTML_HEADER;
    static const char *HTML_FOOTER;

    struct BandInfo {
        const char *name;
        uint32_t start_freq;
        uint32_t end_freq;
    };

    // Explicitly export band_info map
    static const std::map<std::string_view, BandInfo> band_info;

    static std::string generate_root_html(const antenna_switch_config_t &config, const char *ip_addr, const char *mac_addr);
    static esp_err_t generate_root_html_chunked(httpd_req_t *req, const antenna_switch_config_t &config, const char *ip_addr, const char *mac_addr);
    static esp_err_t generate_config_html_chunked(httpd_req_t *req, const antenna_switch_config_t &config);
    
    // Helper function to get bands sorted by frequency (high to low)
    static std::vector<std::pair<std::string_view, BandInfo>> get_bands_by_frequency();

private:
    HtmlContent() = default; // Private constructor to prevent instantiation
    static const char *TAG;
};

#endif // HTML_CONTENT_H
