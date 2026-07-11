#pragma once

#include <atomic>
#include <mutex>

// Forward declarations
struct antenna_switch_config;
typedef struct antenna_switch_config antenna_switch_config_t;
class ConfigManager;

/**
 * @brief Base class providing cached configuration access to reduce ConfigManager calls
 *
 * This class should be inherited by components that frequently access configuration.
 * It caches the configuration locally and only refreshes when the config version changes.
 *
 * Thread-safety: Cache refresh is protected by a mutex to prevent data races during
 * concurrent access from multiple tasks.
 */
class ConfigCache {
protected:
    mutable antenna_switch_config_t* cached_config_;
    mutable std::atomic<uint32_t> cached_version_{0};
    mutable std::atomic<bool> cache_valid_{false};
    mutable std::mutex cache_mutex_;  // Protects cache refresh operations

    /**
     * @brief Update the cached configuration if needed
     * @return A coherent copy of the cached configuration
     */
    antenna_switch_config_t get_cached_config() const;

    /**
     * @brief Invalidate the local cache (force refresh on next access)
     */
    void invalidate_cache() const;

public:
    ConfigCache();
    virtual ~ConfigCache();
    
    // Disable copy/move to avoid cache synchronization issues
    ConfigCache(const ConfigCache&) = delete;
    ConfigCache& operator=(const ConfigCache&) = delete;
    ConfigCache(ConfigCache&&) = delete;
    ConfigCache& operator=(ConfigCache&&) = delete;
};
