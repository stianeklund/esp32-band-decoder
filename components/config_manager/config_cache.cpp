#include "config_cache.h"
#include "config_manager.h"
#include "antenna_switch.h"

ConfigCache::ConfigCache() : cached_config_(nullptr) {
}

ConfigCache::~ConfigCache() {
    delete cached_config_;
}

const antenna_switch_config_t& ConfigCache::get_cached_config() const {
    const uint32_t current_version = ConfigManager::instance().get_config_version();
    
    if (!cache_valid_.load() || cached_version_.load() != current_version) {
        // Cache is invalid, refresh it
        if (!cached_config_) {
            cached_config_ = new antenna_switch_config_t();
        }
        *cached_config_ = ConfigManager::instance().get_config();
        cached_version_.store(current_version);
        cache_valid_.store(true);
    }
    
    return *cached_config_;
}

void ConfigCache::invalidate_cache() const {
    cache_valid_.store(false);
}