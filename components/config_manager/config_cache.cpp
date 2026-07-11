#include "config_cache.h"
#include "config_manager.h"
#include "antenna_switch.h"

ConfigCache::ConfigCache() : cached_config_(new antenna_switch_config_t()) {
    // Pre-allocate to avoid allocation under lock
}

ConfigCache::~ConfigCache() {
    delete cached_config_;
}

antenna_switch_config_t ConfigCache::get_cached_config() const {
    const uint32_t current_version = ConfigManager::instance().get_config_version();
    std::lock_guard<std::mutex> lock(cache_mutex_);

    if (!cache_valid_.load(std::memory_order_relaxed) ||
        cached_version_.load(std::memory_order_relaxed) != current_version) {
        *cached_config_ = ConfigManager::instance().get_config();
        cached_version_.store(current_version, std::memory_order_relaxed);
        cache_valid_.store(true, std::memory_order_release);
    }

    return *cached_config_;
}

void ConfigCache::invalidate_cache() const {
    cache_valid_.store(false, std::memory_order_release);
}
