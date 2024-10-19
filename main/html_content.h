#ifndef HTML_CONTENT_H
#define HTML_CONTENT_H

#include <string>
#include <map>
#include "antenna_switch.h"

extern const char *HTML_HEADER;
extern const char *HTML_FOOTER;

struct BandInfo {
    const char *name;
    uint32_t start_freq;
    uint32_t end_freq;
};

extern const std::map<std::string, BandInfo> band_info;

std::string generate_root_html(const antenna_switch_config_t &config, const char *ip_addr, const char *mac_addr);

std::string generate_config_html(const antenna_switch_config_t &config);

#endif // HTML_CONTENT_H
