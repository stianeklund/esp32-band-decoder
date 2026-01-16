#include "config_cache.h"
#include "config_manager.h"
#include "antenna_switch.h"

ConfigCache::ConfigCache() : cached_config_(new antenna_switch_config_t()) {
    // Pre-allocate to avoid allocation under lock
}

ConfigCache::~ConfigCache() {
    delete cached_config_;
}

const antenna_switch_config_t& ConfigCache::get_cached_config() const {
    const uint32_t current_version = ConfigManager::instance().get_config_version();

    // Fast path: check if cache is valid without acquiring lock
    if (cache_valid_.load(std::memory_order_acquire) &&
        cached_version_.load(std::memory_order_acquire) == current_version) {
        return *cached_config_;
    }

    // Slow path: acquire lock and refresh cache
    std::lock_guard<std::mutex> lock(cache_mutex_);

    // Double-check after acquiring lock (another thread may have refreshed)
    const uint32_t version_after_lock = ConfigManager::instance().get_config_version();
    if (cache_valid_.load(std::memory_order_relaxed) &&
        cached_version_.load(std::memory_order_relaxed) == version_after_lock) {
        return *cached_config_;
    }

    // Refresh the cache (cached_config_ is pre-allocated in constructor)
    *cached_config_ = ConfigManager::instance().get_config();
    cached_version_.store(version_after_lock, std::memory_order_relaxed);
    cache_valid_.store(true, std::memory_order_release);

    return *cached_config_;
}

void ConfigCache::invalidate_cache() const {
    cache_valid_.store(false, std::memory_order_release);
}