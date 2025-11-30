#include "antenna_switch.h"
#include "config_manager.h"
#include "esp_log.h"
#include "relay_controller.h"
#include "wifi_manager.hpp"
#include <memory>
#include "esp_wifi.h"
#include "esp_system.h"
#include "cat_parser.h"
#include "config_cache.h"
#include "esp_timer.h"
#include "websocket_server.h"

static auto TAG = "ANTENNA_SWITCH";

// Constructor for AntennaSwitch
AntennaSwitch::AntennaSwitch()
    : hw_ptt_a_active_(false),
      hw_ptt_b_active_(false),
      cat_tx_a_active_(false),
      relay_controller_(nullptr),
      pre_tx_active_relay_radio_a_(0),
      pre_tx_active_relay_radio_b_(0),
      auto_resolved_conflict_prev_a_relay_(0),
      auto_resolved_conflict_prev_b_relay_(0),
      radio_b_restore_delay_timer_(nullptr),
      radio_a_restore_delay_timer_(nullptr),
      interlock_mutex_(nullptr)
{

    //esp_log_level_set("ANTENNA_SWITCH", ESP_LOG_WARN);
    // esp_log_level_set("ANTENNA_SWITCH", ESP_LOG_DEBUG);

    interlock_mutex_ = xSemaphoreCreateMutex();
    if (interlock_mutex_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create interlock_mutex_");
    }
    
    // Get initial delay from config. ConfigManager should provide defaults if NVS not loaded/valid.
    // Note: ConfigManager::instance() might not be fully initialized here if AntennaSwitch is a static global
    // and its constructor runs before ConfigManager's. However, ConfigManager uses a Meyers' singleton,
    // so its instance() call should trigger its construction if not already done.
    // A safer approach might be to initialize/update timer periods in AntennaSwitch::init() after ConfigManager is confirmed ready.
    // For now, assuming ConfigManager is available or provides a usable default from its own constructor.
    const auto& initial_config = get_cached_config(); // Use cached config instead of direct ConfigManager access
    uint16_t initial_delay_ms = initial_config.radio_restore_delay_ms;

    // Validate and set a fallback if the configured value is unreasonable (e.g., 0 from a fresh NVS or before full init)
    if (initial_delay_ms < 1 || initial_delay_ms > 5000) { // Min 1ms, Max 5s
        ESP_LOGW(TAG, "Initial radio_restore_delay_ms from config (%u ms) is out of range [1, 5000]. Using 200 ms.", initial_delay_ms);
        initial_delay_ms = 200; // Default fallback
    }
    ESP_LOGI(TAG, "Initializing interlock restore timers with delay: %u ms", initial_delay_ms);


    // Create esp_timer for Radio B restore (one-shot, high precision)
    const esp_timer_create_args_t radio_b_timer_args = {
        .callback = radio_b_restore_timer_callback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "RadioBRestTimer",
        .skip_unhandled_events = false
    };
    if (esp_timer_create(&radio_b_timer_args, &radio_b_restore_delay_timer_) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create radio_b_restore_delay_timer_");
        radio_b_restore_delay_timer_ = nullptr;
    }

    // Create esp_timer for Radio A restore (one-shot, high precision)
    const esp_timer_create_args_t radio_a_timer_args = {
        .callback = radio_a_restore_timer_callback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "RadioARestTimer",
        .skip_unhandled_events = false
    };
    if (esp_timer_create(&radio_a_timer_args, &radio_a_restore_delay_timer_) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create radio_a_restore_delay_timer_");
        radio_a_restore_delay_timer_ = nullptr;
    }
}

// Destructor for AntennaSwitch
AntennaSwitch::~AntennaSwitch() {
    // Clean up esp_timers
    if (radio_a_restore_delay_timer_ != nullptr) {
        esp_timer_stop(radio_a_restore_delay_timer_);
        esp_timer_delete(radio_a_restore_delay_timer_);
        radio_a_restore_delay_timer_ = nullptr;
    }
    
    if (radio_b_restore_delay_timer_ != nullptr) {
        esp_timer_stop(radio_b_restore_delay_timer_);
        esp_timer_delete(radio_b_restore_delay_timer_);
        radio_b_restore_delay_timer_ = nullptr;
    }
    
    // Clean up mutex
    if (interlock_mutex_ != nullptr) {
        vSemaphoreDelete(interlock_mutex_);
        interlock_mutex_ = nullptr;
    }
}


[[maybe_unused]] static esp_err_t get_ip_address(char *ip_addr, const size_t max_len) {
    if (!ip_addr || max_len < 16) {
        // IPv4 address max length is 15 chars + null terminator
        return ESP_ERR_INVALID_ARG;
    }

    return WifiManager::instance().get_ip_info(ip_addr, max_len);
}

// Singleton implementation
AntennaSwitch& AntennaSwitch::instance() {
    static AntennaSwitch instance;
    return instance;
}

// Initialize the antenna switch
esp_err_t AntennaSwitch::init() {
    ESP_LOGI(TAG, "Initializing antenna switch");

    // Initialize configuration manager only
    if (const esp_err_t err = ConfigManager::instance().init(); err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize configuration manager: %s", esp_err_to_name(err));
        return err;
    }

    // Don't create or initialize the relay controller here
    // It will be initialized by SystemInitializer
    return ESP_OK;
}

void AntennaSwitch::set_relay_controller(RelayController* controller) {
    relay_controller_ = controller;
}

esp_err_t AntennaSwitch::set_config(const antenna_switch_config_t *config) {
    if (config == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Persist the new configuration using ConfigManager
    esp_err_t err = ConfigManager::instance().update_config(*config);
    if (err != ESP_OK) {
        return err; // Failed to save config
    }

    // ConfigManager now holds the new config.
    // Apply the new delay to the timers.
    const auto& current_config_ref = ConfigManager::instance().get_config_ref();
    uint16_t new_delay_ms = current_config_ref.radio_restore_delay_ms;

    // Validate the delay (e.g., 1ms to 5000ms)
    if (new_delay_ms < 1) new_delay_ms = 1;
    if (new_delay_ms > 5000) new_delay_ms = 5000;

    // Note: esp_timer doesn't need period configuration - timeout is specified when starting
    ESP_LOGI(TAG, "Timer delay configuration updated to %u ms (will be applied on next timer start)", new_delay_ms);

    return ESP_OK; // Return original error from ConfigManager::update_config if it failed, otherwise ESP_OK
}

esp_err_t AntennaSwitch::get_config(antenna_switch_config_t *config) {
    if (config == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *config = ConfigManager::instance().get_config();
    return ESP_OK;
}

const antenna_switch_config_t& AntennaSwitch::get_config_ref() const {
    return ConfigManager::instance().get_config();
}

// Callback from InputManager when hardware PTT A state changes
void AntennaSwitch::on_hw_ptt_a_state_change(const bool active) {
    int64_t ptt_a_start_time = esp_timer_get_time();
    ESP_LOGV(TAG, "on_hw_ptt_a_state_change: Input detected. Active: %s. Time: %lld", active ? "true" : "false", esp_timer_get_time());
    const bool old_hw_ptt_a_value = hw_ptt_a_active_.load(std::memory_order_relaxed);

    if (old_hw_ptt_a_value == active) {
        ESP_LOGV(TAG, "Radio A: HW PTT raw state received (%s) is same as current. No action.", active ? "ACTIVE" : "INACTIVE");
        return;
    }

    ESP_LOGV(TAG, "Radio A: HW PTT raw state changing from %s to %s",
             old_hw_ptt_a_value ? "ACTIVE" : "INACTIVE",
             active ? "ACTIVE" : "INACTIVE");

    // Determine effective TX state BEFORE applying the new HW PTT state.
    const bool old_cat_tx_a = cat_tx_a_active_.load(std::memory_order_relaxed);
    const bool was_effectively_transmitting = old_hw_ptt_a_value || old_cat_tx_a;

    // Store the new HW PTT state
    hw_ptt_a_active_.store(active, std::memory_order_relaxed);

    // When HW PTT goes inactive, clear any stale CAT TX state.
    // The hardware PTT line is the authoritative source for TX state when configured.
    // This prevents CAT TX state from getting "stuck" and blocking antenna changes.
    if (!active && old_cat_tx_a) {
        ESP_LOGI(TAG, "Radio A: HW PTT went inactive, clearing stale CAT TX state (was %s).",
                 old_cat_tx_a ? "ON" : "OFF");
        cat_tx_a_active_.store(false, std::memory_order_relaxed);
    }

    // Determine effective TX state AFTER applying changes.
    // Since we cleared CAT TX when HW PTT goes inactive, effective state now follows HW PTT.
    const bool is_now_effectively_transmitting = active;

    ESP_LOGV(TAG, "Radio A: Effective TX state check (HW PTT change): was_eff_tx=%d, is_now_eff_tx=%d (new_hw_ptt=%d, cat_tx_cleared=%d)",
             was_effectively_transmitting, is_now_effectively_transmitting, active, !active && old_cat_tx_a);

    if (was_effectively_transmitting && !is_now_effectively_transmitting) {
        // Effective TX state changed from ON to OFF
        ESP_LOGV(TAG, "Radio A: Effective TX state changed from ON to OFF (due to HW PTT).");
        on_radio_a_tx_stop();
    } else if (!was_effectively_transmitting && is_now_effectively_transmitting) {
        // Effective TX state changed from OFF to ON
        ESP_LOGV(TAG, "Radio A: Effective TX state changed from OFF to ON (due to HW PTT).");
        on_radio_a_tx_start();
    } else {
        // Effective TX state did not change (e.g., was ON, still ON, or was OFF, still OFF)
        ESP_LOGV(TAG, "Radio A: HW PTT raw state changed, but effective TX state remains %s.",
                 is_now_effectively_transmitting ? "ON" : "OFF");
    }
    ESP_LOGD(TAG, "PROF: on_hw_ptt_a_state_change took %lld us", esp_timer_get_time() - ptt_a_start_time);
}

// Callback from InputManager when hardware PTT B state changes
void AntennaSwitch::on_hw_ptt_b_state_change(const bool active) {
    ESP_LOGV(TAG, "on_hw_ptt_b_state_change: Input detected. Active: %s. Time: %lld", active ? "true" : "false", esp_timer_get_time());
    const bool old_hw_ptt_b_value = hw_ptt_b_active_.load(std::memory_order_relaxed);

    if (old_hw_ptt_b_value == active) {
        // ESP_LOGI(TAG, "Radio B: HW PTT raw state received (%s) is same as current. No action.", new_hw_ptt_b_value ? "ACTIVE" : "INACTIVE");
        return;
    }

    ESP_LOGV(TAG, "Radio B: HW PTT raw state changing from %s to %s",
             old_hw_ptt_b_value ? "ACTIVE" : "INACTIVE",
             active ? "ACTIVE" : "INACTIVE");

    // For Radio B, currently, its TX state is primarily driven by HW PTT.
    // If CatParser were to support independent TX state for Radio B, that check would be included here.
    const bool was_effectively_transmitting = old_hw_ptt_b_value; // Add || CatParser::instance().is_radio_b_transmitting() if applicable

    hw_ptt_b_active_.store(active, std::memory_order_relaxed);

    const bool is_now_effectively_transmitting = active; // Add || CatParser::instance().is_radio_b_transmitting() if applicable

    ESP_LOGV(TAG, "Radio B: Effective TX state check: was_eff_tx=%d, is_now_eff_tx=%d (new_hw_ptt=%d)",
             was_effectively_transmitting, is_now_effectively_transmitting, active);

    if (was_effectively_transmitting && !is_now_effectively_transmitting) {
        ESP_LOGV(TAG, "Radio B: Effective TX state changed from ON to OFF.");
        on_radio_b_tx_stop();
    } else if (!was_effectively_transmitting && is_now_effectively_transmitting) {
        ESP_LOGV(TAG, "Radio B: Effective TX state changed from OFF to ON.");
        on_radio_b_tx_start();
    } else {
        ESP_LOGV(TAG, "Radio B: HW PTT raw state changed, but effective TX state remains %s.",
                 is_now_effectively_transmitting ? "ON" : "OFF");
    }
}

// Callback from CatParser when CAT TX A state changes
void AntennaSwitch::on_cat_tx_a_state_change(const bool active) {
    const bool old_cat_tx_a_value = cat_tx_a_active_.load(std::memory_order_relaxed);

    if (old_cat_tx_a_value == active) {
        ESP_LOGV(TAG, "Radio A: CAT TX state received (%s) is same as current. No action.", active ? "ACTIVE" : "INACTIVE");
        return;
    }

    ESP_LOGV(TAG, "Radio A: CAT TX state changing from %s to %s",
             old_cat_tx_a_value ? "ACTIVE" : "INACTIVE",
             active ? "ACTIVE" : "INACTIVE");

    // Determine effective TX state BEFORE applying the new CAT TX state.
    const bool hw_ptt_is_active_a = hw_ptt_a_active_.load(std::memory_order_relaxed);
    const bool was_effectively_transmitting = hw_ptt_is_active_a || old_cat_tx_a_value;

    // Store the new CAT TX state
    cat_tx_a_active_.store(active, std::memory_order_relaxed);

    // Determine effective TX state AFTER applying the new CAT TX state.
    const bool is_now_effectively_transmitting = hw_ptt_is_active_a || active;

    ESP_LOGV(TAG, "Radio A: Effective TX state check (CAT change): was_eff_tx=%d, is_now_eff_tx=%d (hw_ptt=%d, new_cat_tx=%d)",
             was_effectively_transmitting, is_now_effectively_transmitting, hw_ptt_is_active_a, active);

    if (was_effectively_transmitting && !is_now_effectively_transmitting) {
        ESP_LOGV(TAG, "Radio A: Effective TX state changed from ON to OFF (due to CAT).");
        on_radio_a_tx_stop();
    } else if (!was_effectively_transmitting && is_now_effectively_transmitting) {
        ESP_LOGV(TAG, "Radio A: Effective TX state changed from OFF to ON (due to CAT).");
        on_radio_a_tx_start();
    } else {
        ESP_LOGV(TAG, "Radio A: CAT TX state changed, but effective TX state remains %s.",
                 is_now_effectively_transmitting ? "ON" : "OFF");
    }
}


bool AntennaSwitch::is_radio_a_transmitting_effective() const {
    const bool hw_ptt = hw_ptt_a_active_.load(std::memory_order_relaxed);
    const bool cat_tx = cat_tx_a_active_.load(std::memory_order_relaxed);
    const bool result = hw_ptt || cat_tx;
    
    // Add debugging for stuck TX state issues
    if (result) {
        ESP_LOGW(TAG, "Radio A transmitting effective: hw_ptt=%d, cat_tx=%d", hw_ptt, cat_tx);
    }
    
    return result;
}

bool AntennaSwitch::is_radio_b_transmitting_effective() const {
    // For now, Radio B's TX state is determined by its hardware PTT.
    // If CatParser supported Radio B TX state:
    // return hw_ptt_b_active_.load(std::memory_order_relaxed) || CatParser::instance().is_radio_b_transmitting();
     return hw_ptt_b_active_.load(std::memory_order_relaxed);
}

esp_err_t AntennaSwitch::set_frequency(const uint32_t frequency) {
    ESP_LOGV(TAG, "Setting antenna for frequency: %lu Hz", frequency);

    // we only need a reference to config here as we won't mutate it
    const auto &config = get_cached_config(); // Use cached config
    if (!config.auto_mode) {
        ESP_LOGW(TAG, "Automatic mode is disabled, not changing antenna");
        return ESP_OK;
    }

    // Assuming frequency updates are primarily for Radio A's context in auto mode
    constexpr auto radio_idx_numeric = static_cast<size_t>(RadioID::A);
    constexpr auto current_radio_context = RadioID::A;

    for (int i = 0; i < config.num_bands; i++) { // i is band_index
        // ReSharper disable once CppTooWideScopeInitStatement
        const auto &band_config_for_radio = config.bands[radio_idx_numeric][i];
        if (frequency >= band_config_for_radio.start_freq &&
            frequency <= band_config_for_radio.end_freq) {
            
            int target_relay_id = 0;
            const uint8_t preferred_antenna = config.last_used_antenna[radio_idx_numeric][i];

            // Check if preferred antenna is valid (1-based) and configured for this band
            if (preferred_antenna != 0 && 
                preferred_antenna <= config.num_antenna_ports &&
                band_config_for_radio.antenna_ports[preferred_antenna - 1]) {
                target_relay_id = preferred_antenna;
                ESP_LOGD(TAG, "Using preferred antenna %d for Radio %c, Band %d (Freq: %lu Hz)",
                         target_relay_id, (current_radio_context == RadioID::A ? 'A' : 'B'), i, frequency);
            } else {
                // Fallback: Find the first available antenna port for this band
                for (int j = 0; j < config.num_antenna_ports; j++) { // j is port_index (0-based)
                    if (band_config_for_radio.antenna_ports[j]) {
                        target_relay_id = j + 1; // relay_id is 1-based
                        ESP_LOGD(TAG, "Using first available antenna %d for Radio %c, Band %d (Freq: %lu Hz)",
                                 target_relay_id, (current_radio_context == RadioID::A ? 'A' : 'B'), i, frequency);
                        break;
                    }
                }
            }

            if (target_relay_id != 0) {
                ESP_LOGD(TAG, "Auto-selecting relay %d for band %d (Radio %c) due to frequency %lu Hz",
                         target_relay_id, i, (current_radio_context == RadioID::A ? 'A' : 'B'), frequency);
                // Use AntennaSwitch's own set_relay_for_antenna to ensure conflict anticipation and restoration logic is applied
                return set_relay_for_antenna(target_relay_id, i, current_radio_context, true);
            }

            ESP_LOGW(TAG, "No available/preferred antenna port found for Radio %c, Band %d (Freq: %lu Hz, Preferred: %d)",
                     (current_radio_context == RadioID::A ? 'A' : 'B'), i, frequency, preferred_antenna);
            return ESP_OK; // No suitable antenna, but not an error in processing
        }
    }

    ESP_LOGV(TAG, "Config does not support frequency: %lu Hz for auto-switching on Radio %c", frequency, (current_radio_context == RadioID::A ? 'A' : 'B'));
    return ESP_OK;
}

/**
 * Whether automatic band switching should be used
 * @param auto_mode
 * @return esp_err_t
 */
esp_err_t AntennaSwitch::set_auto_mode(const bool auto_mode) {
    ESP_LOGD(TAG, "Setting auto mode: %s", auto_mode ? "ON" : "OFF");

    const auto config_ptr = std::make_unique<antenna_switch_config_t>();
    if (!config_ptr) {
        ESP_LOGE(TAG, "Failed to allocate memory for config in set_auto_mode");
        return ESP_ERR_NO_MEM;
    }
    // Populate the heap-allocated config with current settings
    *config_ptr = ConfigManager::instance().get_config(); // Copy from ConfigManager's version

    config_ptr->auto_mode = auto_mode;

    return ConfigManager::instance().update_config(*config_ptr);
}

int AntennaSwitch::get_active_relay_for_radio(RadioID radio) const {
    if (!relay_controller_) {
        ESP_LOGE(TAG, "Relay controller not initialized in get_active_relay_for_radio");
        return 0; // 0 indicates no relay active or error
    }

    int start_relay_id, end_relay_id;
    if (radio == RadioID::A) {
        start_relay_id = 1;
        end_relay_id = RelayController::RELAYS_PER_RADIO;
    } else { // RadioID::B
        start_relay_id = RelayController::RELAYS_PER_RADIO + 1;
        end_relay_id = RelayController::NUM_RELAYS;
    }

    for (int relay_id_iter = start_relay_id; relay_id_iter <= end_relay_id; ++relay_id_iter) {
        bool current_state = false;
        // Use the public get_relay_state which queries the controller
        // Note: get_relay_state in AntennaSwitch calls relay_controller_->get_relay_state
        current_state = relay_controller_->get_relay_state(relay_id_iter);
        if (current_state) { // If true, relay is ON and active.
            return relay_id_iter;
        }
    }
    return 0; // No active relay found for this radio
}

void AntennaSwitch::on_radio_a_tx_start() {
    if (xSemaphoreTake(interlock_mutex_, pdMS_TO_TICKS(10)) != pdTRUE) {
        return;
    }

    const int active_relay_radio_a = get_active_relay_for_radio(RadioID::A);

    if (active_relay_radio_a == 0) {
        ESP_LOGW(TAG, "Radio A TX: PTT active, but no antenna selected for Radio A. Interlock for Radio B not applied.");
        ESP_LOGD(TAG, "Radio A TX state: HW PTT A: %s, CAT TX A: %s.",
                 hw_ptt_a_active_.load(std::memory_order_relaxed) ? "ACTIVE" : "INACTIVE",
                 cat_tx_a_active_.load(std::memory_order_relaxed) ? "ON" : "OFF");
        xSemaphoreGive(interlock_mutex_);
        return;
    }

    ESP_LOGV(TAG, "Radio A TX: HW PTT A: %s, CAT TX A: %s. Radio A using relay %d. Applying interlock for Radio B.",
             hw_ptt_a_active_.load(std::memory_order_relaxed) ? "ACTIVE" : "INACTIVE",
             cat_tx_a_active_.load(std::memory_order_relaxed) ? "ON" : "OFF",
             active_relay_radio_a);

    // Cancel any pending restoration for Radio B
    if (radio_b_restore_delay_timer_ != nullptr && esp_timer_is_active(radio_b_restore_delay_timer_)) {
        ESP_LOGD(TAG, "Radio A TX start: Cancelling pending Radio B relay restoration.");
        esp_timer_stop(radio_b_restore_delay_timer_);
        // pre_tx_active_relay_radio_b_ is not cleared here; it might be needed if this TX cycle also stops.
    }

    const auto& config = get_cached_config(); // Use cached config
    if (config.radio_operation_mode == RADIO_OP_MODE_SINGLE_A) {
        ESP_LOGV(TAG, "Single Radio A mode, no interlock action needed for Radio B.");
        xSemaphoreGive(interlock_mutex_);
        return;
    }

    if (!relay_controller_) {
        ESP_LOGE(TAG, "Relay controller not initialized, cannot apply TX interlock for Radio B.");
        xSemaphoreGive(interlock_mutex_);
        return;
    }

    // Check if Radio B is ALREADY considered interlocked (pre_tx_active_relay_radio_b_ is set)
    // AND if Radio B is currently not using any relay (meaning it was turned off and not restored yet).
    bool b_was_already_interlocked_and_off = false;
    if (pre_tx_active_relay_radio_b_ != 0) {
        if (get_active_relay_for_radio(RadioID::B) == 0) { // Check if B is actually off
            b_was_already_interlocked_and_off = true;
            ESP_LOGD(TAG, "Radio A TX start: Radio B interlock for relay %d already in effect and B is off. No new deactivation needed.", pre_tx_active_relay_radio_b_);
        }
    }

    if (!b_was_already_interlocked_and_off) {
        const int active_b_relay = get_active_relay_for_radio(RadioID::B);

        if (active_b_relay != 0) {
            if (is_radio_b_transmitting_effective() && config.radio_operation_mode == RADIO_OP_MODE_CONCURRENT_AB && !config.interlock_auto_resolves_conflict) {
                ESP_LOGW(TAG, "Radio A TX start: Conflict! Radio B (relay %d) is also transmitting. Interlock resolution is manual. Radio B relay NOT changed.", active_b_relay);
            } else {
                pre_tx_active_relay_radio_b_ = active_b_relay; // Store it before turning off
                ESP_LOGV(TAG, "Radio B was using relay %d. Turning it OFF due to Radio A TX.", pre_tx_active_relay_radio_b_);

                if (const esp_err_t err = relay_controller_->set_relay(pre_tx_active_relay_radio_b_, false); err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to turn OFF Radio B relay %d: %s", pre_tx_active_relay_radio_b_, esp_err_to_name(err));
                    pre_tx_active_relay_radio_b_ = 0; 
                }
            }
        } else {
            ESP_LOGV(TAG, "Radio B was not using any relay. No interlock action needed for Radio A TX.");
            // If B was not using any relay, but pre_tx_active_relay_radio_b_ was set (e.g. from a previous cycle where timer was cancelled)
            // it should remain set. So, don't clear pre_tx_active_relay_radio_b_ here.
        }
    }
    xSemaphoreGive(interlock_mutex_);
}

void AntennaSwitch::on_radio_a_tx_stop() {
    if (xSemaphoreTake(interlock_mutex_, pdMS_TO_TICKS(10)) != pdTRUE) {
        ESP_LOGE(TAG, "on_radio_a_tx_stop: Could not take mutex");
        return;
    }

    ESP_LOGD(TAG, "Radio A TX: HW PTT A: %s, CAT TX A: %s. Scheduling Radio B state restoration if applicable.",
             hw_ptt_a_active_.load(std::memory_order_relaxed) ? "ACTIVE" : "INACTIVE",
             cat_tx_a_active_.load(std::memory_order_relaxed) ? "ON" : "OFF");
    
    const auto& config = get_cached_config(); // Use cached config
    if (config.radio_operation_mode == RADIO_OP_MODE_SINGLE_A) {
        ESP_LOGD(TAG, "Single Radio A mode, no interlock restoration needed for Radio B.");
        xSemaphoreGive(interlock_mutex_);
        return;
    }

    if (!relay_controller_) {
        ESP_LOGE(TAG, "Relay controller not initialized, cannot schedule Radio B state restoration.");
        xSemaphoreGive(interlock_mutex_);
        return;
    }

    if (pre_tx_active_relay_radio_b_ != 0) {
        if (is_radio_b_transmitting_effective()) {
            ESP_LOGW(TAG, "Radio B is currently transmitting (effective), not scheduling restoration of its relay %d.", pre_tx_active_relay_radio_b_);
            pre_tx_active_relay_radio_b_ = 0; // Abort restoration for this stored relay
        } else {
            if (radio_b_restore_delay_timer_ != nullptr) {
                uint16_t current_delay_ms = ConfigManager::instance().get_config_ref().radio_restore_delay_ms;
                const auto& cached_cfg_for_delay = get_cached_config();
                current_delay_ms = cached_cfg_for_delay.radio_restore_delay_ms;
                if (current_delay_ms < 1 || current_delay_ms > 5000) current_delay_ms = 200; // Fallback for logging safety
                ESP_LOGD(TAG, "Radio A TX stop: Scheduling Radio B relay %d restoration in %u ms.", pre_tx_active_relay_radio_b_, current_delay_ms);
                if (esp_timer_start_once(radio_b_restore_delay_timer_, current_delay_ms * 1000) != ESP_OK) { // Convert ms to microseconds
                    ESP_LOGE(TAG, "Failed to start radio_b_restore_delay_timer_. Relay %d will not be restored by timer.", pre_tx_active_relay_radio_b_);
                }
                // Do NOT clear pre_tx_active_relay_radio_b_ here. The timer callback will handle it.
            } else {
                ESP_LOGE(TAG, "Radio B restore delay timer not initialized! Relay %d will not be restored.", pre_tx_active_relay_radio_b_);
            }
        }
    } else {
        ESP_LOGD(TAG, "No Radio B relay was stored (pre_tx_active_relay_radio_b_ is 0). No restoration action for Radio B.");
    }
    xSemaphoreGive(interlock_mutex_);
}

// Timer callback for Radio B restoration
void AntennaSwitch::radio_b_restore_timer_callback(void* arg) {
    auto* self = static_cast<AntennaSwitch*>(arg);

    if (!self) {
        ESP_LOGE(TAG, "Radio B restore timer callback: self is null");
        return;
    }

    if (xSemaphoreTake(self->interlock_mutex_, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGE(TAG, "Radio B restore timer: Could not take interlock_mutex_. Rescheduling.");
        if (self->radio_b_restore_delay_timer_ != nullptr && self->pre_tx_active_relay_radio_b_ != 0) {
            const auto& cached_cfg = self->get_cached_config();
            uint16_t current_delay_ms = cached_cfg.radio_restore_delay_ms;
            if (current_delay_ms < 1 || current_delay_ms > 5000) current_delay_ms = 200; // Fallback

            uint16_t reschedule_delay_ms = current_delay_ms / 2;
            if (reschedule_delay_ms < 50) reschedule_delay_ms = 50; // Minimum reschedule delay 50ms

            ESP_LOGD(TAG, "Radio B restore timer: Rescheduling for relay %d in %u ms (interlock_mutex_ busy)", self->pre_tx_active_relay_radio_b_, reschedule_delay_ms);
            if (esp_timer_start_once(self->radio_b_restore_delay_timer_, reschedule_delay_ms * 1000) != ESP_OK) {
                 ESP_LOGE(TAG, "Radio B restore timer: Failed to reschedule after interlock_mutex_ busy. Aborting for relay %d.", self->pre_tx_active_relay_radio_b_);
                 self->pre_tx_active_relay_radio_b_ = 0; // Cannot reschedule, abort this attempt
            }
        } else if (self->pre_tx_active_relay_radio_b_ == 0) {
             ESP_LOGD(TAG, "Radio B restore timer: interlock_mutex_ busy, but no relay to restore.");
        } else {
             ESP_LOGE(TAG, "Radio B restore timer: interlock_mutex_ busy, timer invalid, cannot reschedule relay %d.", self->pre_tx_active_relay_radio_b_);
             self->pre_tx_active_relay_radio_b_ = 0; // Cannot reschedule, abort
        }
        return;
    }

    ESP_LOGD(TAG, "Radio B restore timer expired, interlock_mutex_ acquired.");

    if (self->pre_tx_active_relay_radio_b_ == 0) {
        ESP_LOGD(TAG, "Radio B restore timer: No pre_tx_active_relay_radio_b_ to restore (cleared or already restored).");
        xSemaphoreGive(self->interlock_mutex_);
        return;
    }

    // Safety check: Is Radio A transmitting again?
    if (self->is_radio_a_transmitting_effective()) {
        ESP_LOGW(TAG, "Radio B restore timer: Radio A started transmitting again. Aborting restoration of relay %d.", self->pre_tx_active_relay_radio_b_);
        // pre_tx_active_relay_radio_b_ is NOT cleared here. on_radio_a_tx_start would have handled it or will handle it.
        xSemaphoreGive(self->interlock_mutex_);
        return;
    }

    // Check if Radio A is using the same port that Radio B wants to restore to
    if (const int active_a_relay = self->get_active_relay_for_radio(RadioID::A); active_a_relay != 0) {
        const int radio_b_port_idx = (self->pre_tx_active_relay_radio_b_ - 1) % RelayController::RELAYS_PER_RADIO;

        if (const int radio_a_port_idx = (active_a_relay - 1) % RelayController::RELAYS_PER_RADIO; radio_a_port_idx == radio_b_port_idx) {
            ESP_LOGW(TAG, "Radio B restore timer: Radio A is using port %d (relay %d), cannot restore Radio B relay %d (same port). Will retry later.", 
                     radio_a_port_idx + 1, active_a_relay, self->pre_tx_active_relay_radio_b_);

            const auto& cached_cfg = self->get_cached_config();
            uint16_t current_delay_ms = cached_cfg.radio_restore_delay_ms;
            if (current_delay_ms < 1 || current_delay_ms > 5000) current_delay_ms = 200;
            
            uint16_t reschedule_delay_ms = current_delay_ms; // Retry with the original configured delay
            if (reschedule_delay_ms < 50) reschedule_delay_ms = 50;


            ESP_LOGD(TAG, "Radio B restore timer: Rescheduling for relay %d in %u ms (port conflict with Radio A)", self->pre_tx_active_relay_radio_b_, reschedule_delay_ms);
            if (esp_timer_start_once(self->radio_b_restore_delay_timer_, reschedule_delay_ms * 1000) != ESP_OK) {
                ESP_LOGE(TAG, "Radio B restore timer: Failed to reschedule on port conflict. Aborting for relay %d.", self->pre_tx_active_relay_radio_b_);
                self->pre_tx_active_relay_radio_b_ = 0; // Cannot reschedule, abort
            }
            xSemaphoreGive(self->interlock_mutex_);
            return;
        } else {
            ESP_LOGD(TAG, "Radio B restore timer: Radio A is using port %d (relay %d), Radio B can safely restore to port %d (relay %d).",
                     radio_a_port_idx + 1, active_a_relay, radio_b_port_idx + 1, self->pre_tx_active_relay_radio_b_);
        }
    } else {
        ESP_LOGD(TAG, "Radio B restore timer: Radio A is not using any relay, Radio B can safely restore to relay %d.", self->pre_tx_active_relay_radio_b_);
    }

    if (self->is_radio_b_transmitting_effective()) {
        ESP_LOGW(TAG, "Radio B restore timer: Radio B is now transmitting on its own. Aborting restoration of its previous relay %d.", self->pre_tx_active_relay_radio_b_);
        self->pre_tx_active_relay_radio_b_ = 0; // Abort and clear
        xSemaphoreGive(self->interlock_mutex_);
        return;
    }

    if (!self->relay_controller_) {
        ESP_LOGE(TAG, "Radio B restore timer: Relay controller not initialized.");
        self->pre_tx_active_relay_radio_b_ = 0; // Cannot restore, clear
        xSemaphoreGive(self->interlock_mutex_);
        return;
    }
    
    ESP_LOGD(TAG, "Radio B restore timer: Attempting to restore Radio B relay %d to ON.", self->pre_tx_active_relay_radio_b_);

    if (const esp_err_t err = self->relay_controller_->set_relay(self->pre_tx_active_relay_radio_b_, true); err != ESP_OK) {
        ESP_LOGE(TAG, "Radio B restore timer: Failed to restore Radio B relay %d: %s", self->pre_tx_active_relay_radio_b_, esp_err_to_name(err));

        if (err == ESP_ERR_TIMEOUT && self->radio_b_restore_delay_timer_ != nullptr && self->pre_tx_active_relay_radio_b_ != 0) {
            // If it was a timeout (potentially mutex or I2C hardware), reschedule
            ESP_LOGW(TAG, "Radio B restore timer: Rescheduling restoration for relay %d due to timeout error from set_relay.", self->pre_tx_active_relay_radio_b_);
            const auto& cached_cfg = self->get_cached_config();
            uint16_t current_delay_ms = cached_cfg.radio_restore_delay_ms;
            if (current_delay_ms < 1 || current_delay_ms > 5000) current_delay_ms = 200; // Fallback
            
            uint16_t reschedule_delay_ms = current_delay_ms / 2; 
            if (reschedule_delay_ms < 50) reschedule_delay_ms = 50; // Minimum 50ms

            ESP_LOGD(TAG, "Radio B restore timer: Rescheduling for relay %d in %u ms (set_relay timeout)", self->pre_tx_active_relay_radio_b_, reschedule_delay_ms);
            if (esp_timer_start_once(self->radio_b_restore_delay_timer_, reschedule_delay_ms * 1000) == ESP_OK) {
                // Successfully rescheduled, so don't clear pre_tx_active_relay_radio_b_ yet
                xSemaphoreGive(self->interlock_mutex_);
                return; // Exit without clearing the relay to be restored
            }

            ESP_LOGE(TAG, "Radio B restore timer: Failed to reschedule after set_relay error. Aborting restoration for relay %d.", self->pre_tx_active_relay_radio_b_);
            // Fall through to clear pre_tx_active_relay_radio_b_
        }
        // If not rescheduled, or error was not timeout, or other issue, clear the relay.
        self->pre_tx_active_relay_radio_b_ = 0; 
    } else { // Success
        ESP_LOGD(TAG, "Radio B restore timer: Successfully restored Radio B relay %d.", self->pre_tx_active_relay_radio_b_);
        self->pre_tx_active_relay_radio_b_ = 0; // Clear after successful restoration
    }
    xSemaphoreGive(self->interlock_mutex_);
}


void AntennaSwitch::on_radio_b_tx_start() {
    if (xSemaphoreTake(interlock_mutex_, pdMS_TO_TICKS(10)) != pdTRUE) {
        ESP_LOGE(TAG, "on_radio_b_tx_start: Could not take mutex");
        return;
    }

    const int active_relay_radio_b = get_active_relay_for_radio(RadioID::B);

    if (active_relay_radio_b == 0) {
        ESP_LOGW(TAG, "Radio B TX: PTT active, but no antenna selected for Radio B. Interlock for Radio A not applied.");
        ESP_LOGD(TAG, "Radio B TX state: HW PTT B: %s.",
                 hw_ptt_b_active_.load(std::memory_order_relaxed) ? "ACTIVE" : "INACTIVE");
        xSemaphoreGive(interlock_mutex_);
        return;
    }

    ESP_LOGI(TAG, "Radio B TX: HW PTT B: %s. Radio B using relay %d. Applying interlock for Radio A.",
             hw_ptt_b_active_.load(std::memory_order_relaxed) ? "ACTIVE" : "INACTIVE",
             active_relay_radio_b);

    if (radio_a_restore_delay_timer_ != nullptr && esp_timer_is_active(radio_a_restore_delay_timer_)) {
        ESP_LOGI(TAG, "Radio B TX start: Cancelling pending Radio A relay restoration.");
        esp_timer_stop(radio_a_restore_delay_timer_);
    }
    
    const auto& config = get_cached_config(); // Use cached config
   
    if (config.radio_operation_mode == RADIO_OP_MODE_SINGLE_A) {
        ESP_LOGW(TAG, "Radio B TX reported in SINGLE_A mode. This is unexpected.");
        xSemaphoreGive(interlock_mutex_);
        return;
    }
    if (!relay_controller_) {
        ESP_LOGE(TAG, "Relay controller not initialized, cannot apply TX interlock for Radio A.");
        xSemaphoreGive(interlock_mutex_);
        return;
    }

    bool a_was_already_interlocked_and_off = false;
    if (pre_tx_active_relay_radio_a_ != 0) {
        if (get_active_relay_for_radio(RadioID::A) == 0) {
            a_was_already_interlocked_and_off = true;
            ESP_LOGD(TAG, "Radio B TX start: Radio A interlock for relay %d already in effect and A is off. No new deactivation needed.", pre_tx_active_relay_radio_a_);
        }
    }

    if (!a_was_already_interlocked_and_off) {
        if (const int active_a_relay = get_active_relay_for_radio(RadioID::A); active_a_relay != 0) {
            if (is_radio_a_transmitting_effective() && config.radio_operation_mode == RADIO_OP_MODE_CONCURRENT_AB && !config.interlock_auto_resolves_conflict) {
                ESP_LOGW(TAG, "Radio B TX start: Conflict! Radio A (relay %d) is also transmitting. Interlock resolution is manual. Radio A relay NOT changed.", active_a_relay);
            } else {
                pre_tx_active_relay_radio_a_ = active_a_relay;
                ESP_LOGI(TAG, "Radio A was using relay %d. Turning it OFF due to Radio B TX.", pre_tx_active_relay_radio_a_);

                if (const esp_err_t err = relay_controller_->set_relay(pre_tx_active_relay_radio_a_, false); err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to turn OFF Radio A relay %d: %s", pre_tx_active_relay_radio_a_, esp_err_to_name(err));
                    pre_tx_active_relay_radio_a_ = 0; 
                }
            }
        } else {
            ESP_LOGD(TAG, "Radio A was not using any relay. No interlock action needed for Radio B TX.");
        }
    }
    xSemaphoreGive(interlock_mutex_);
}

void AntennaSwitch::on_radio_b_tx_stop() {
    if (xSemaphoreTake(interlock_mutex_, pdMS_TO_TICKS(10)) != pdTRUE) {
        ESP_LOGE(TAG, "on_radio_b_tx_stop: Could not take mutex");
        return;
    }

    ESP_LOGD(TAG, "Radio B TX: HW PTT B: %s. Scheduling Radio A state restoration if applicable.",
             hw_ptt_b_active_.load(std::memory_order_relaxed) ? "ACTIVE" : "INACTIVE");

    if (const auto& config = get_cached_config(); config.radio_operation_mode == RADIO_OP_MODE_SINGLE_A) {
        xSemaphoreGive(interlock_mutex_);
        return;
    }
    if (!relay_controller_) {
        ESP_LOGE(TAG, "Relay controller not initialized, cannot schedule Radio A state restoration.");
        xSemaphoreGive(interlock_mutex_);
        return;
    }

    if (pre_tx_active_relay_radio_a_ != 0) {
        if (is_radio_a_transmitting_effective()) {
             ESP_LOGW(TAG, "Radio A is currently transmitting (effective), cannot schedule restoration of its previous relay %d.", pre_tx_active_relay_radio_a_);
             pre_tx_active_relay_radio_a_ = 0;
        } else {
            if (radio_a_restore_delay_timer_ != nullptr) {
                const auto& cached_cfg_for_delay = get_cached_config();
                uint16_t current_delay_ms = cached_cfg_for_delay.radio_restore_delay_ms;

                if (current_delay_ms < 1 || current_delay_ms > 5000) current_delay_ms = 200;
                ESP_LOGD(TAG, "Radio B TX stop: Scheduling Radio A relay %d restoration in %u ms.", pre_tx_active_relay_radio_a_, current_delay_ms);

                if (esp_timer_start_once(radio_a_restore_delay_timer_, current_delay_ms * 1000) != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to start radio_a_restore_delay_timer_. Relay %d will not be restored by timer.", pre_tx_active_relay_radio_a_);
                }
            } else {
                ESP_LOGE(TAG, "Radio A restore delay timer not initialized! Relay %d will not be restored.", pre_tx_active_relay_radio_a_);
            }
        }
    } else {
         ESP_LOGD(TAG, "No Radio A relay was stored. No restoration action for Radio A.");
    }
    xSemaphoreGive(interlock_mutex_);
}

// Timer callback for Radio A restoration
void AntennaSwitch::radio_a_restore_timer_callback(void* arg) {
    auto* self = static_cast<AntennaSwitch*>(arg);
    if (!self) {
        ESP_LOGE(TAG, "Radio A restore timer callback: self is null");
        return;
    }

    if (xSemaphoreTake(self->interlock_mutex_, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGE(TAG, "Radio A restore timer: Could not take interlock_mutex_. Rescheduling.");
        if (self->radio_a_restore_delay_timer_ != nullptr && self->pre_tx_active_relay_radio_a_ != 0) {
            const auto& cached_cfg = self->get_cached_config();
            uint16_t current_delay_ms = cached_cfg.radio_restore_delay_ms;
            if (current_delay_ms < 1 || current_delay_ms > 5000) current_delay_ms = 200; // Fallback

            uint16_t reschedule_delay_ms = current_delay_ms / 2;
            if (reschedule_delay_ms < 50) reschedule_delay_ms = 50; // Minimum reschedule delay 50ms
            
            ESP_LOGD(TAG, "Radio A restore timer: Rescheduling for relay %d in %u ms (interlock_mutex_ busy)", self->pre_tx_active_relay_radio_a_, reschedule_delay_ms);
            if (esp_timer_start_once(self->radio_a_restore_delay_timer_, reschedule_delay_ms * 1000) != ESP_OK) {
                ESP_LOGE(TAG, "Radio A restore timer: Failed to reschedule after interlock_mutex_ busy. Aborting for relay %d.", self->pre_tx_active_relay_radio_a_);
                self->pre_tx_active_relay_radio_a_ = 0; // Cannot reschedule, abort
            }
        } else if (self->pre_tx_active_relay_radio_a_ == 0) {
            ESP_LOGD(TAG, "Radio A restore timer: interlock_mutex_ busy, but no relay to restore.");
        } else {
            ESP_LOGE(TAG, "Radio A restore timer: interlock_mutex_ busy, timer invalid, cannot reschedule relay %d.", self->pre_tx_active_relay_radio_a_);
            self->pre_tx_active_relay_radio_a_ = 0; // Cannot reschedule, abort
        }
        return;
    }

    ESP_LOGD(TAG, "Radio A restore timer expired, interlock_mutex_ acquired.");

    if (self->pre_tx_active_relay_radio_a_ == 0) {
        ESP_LOGD(TAG, "Radio A restore timer: No pre_tx_active_relay_radio_a_ to restore (cleared or already restored).");
        xSemaphoreGive(self->interlock_mutex_);
        return;
    }

    if (self->is_radio_b_transmitting_effective()) {
        ESP_LOGW(TAG, "Radio A restore timer: Radio B started transmitting again. Aborting restoration of relay %d.", self->pre_tx_active_relay_radio_a_);
        // pre_tx_active_relay_radio_a_ is NOT cleared here. on_radio_b_tx_start would have handled it.
        xSemaphoreGive(self->interlock_mutex_);
        return;
    }

    // Check if Radio B is using the same port that Radio A wants to restore to
    if (const int active_b_relay = self->get_active_relay_for_radio(RadioID::B); active_b_relay != 0) {
        const int radio_a_port_idx = (self->pre_tx_active_relay_radio_a_ - 1) % RelayController::RELAYS_PER_RADIO;

        if (const int radio_b_port_idx = (active_b_relay - 1) % RelayController::RELAYS_PER_RADIO; radio_b_port_idx == radio_a_port_idx) {
            ESP_LOGW(TAG, "Radio A restore timer: Radio B is using port %d (relay %d), cannot restore Radio A relay %d (same port). Will retry later.", 
                     radio_b_port_idx + 1, active_b_relay, self->pre_tx_active_relay_radio_a_);
            const auto& cached_cfg = self->get_cached_config();
            uint16_t current_delay_ms = cached_cfg.radio_restore_delay_ms;
            if (current_delay_ms < 1 || current_delay_ms > 5000) current_delay_ms = 200;

            uint16_t reschedule_delay_ms = current_delay_ms; // Retry with the original configured delay
            if (reschedule_delay_ms < 50) reschedule_delay_ms = 50;

            ESP_LOGD(TAG, "Radio A restore timer: Rescheduling for relay %d in %u ms (port conflict with Radio B)", self->pre_tx_active_relay_radio_a_, reschedule_delay_ms);
            if (esp_timer_start_once(self->radio_a_restore_delay_timer_, reschedule_delay_ms * 1000) != ESP_OK) {
                ESP_LOGE(TAG, "Radio A restore timer: Failed to reschedule on port conflict. Aborting for relay %d.", self->pre_tx_active_relay_radio_a_);
                self->pre_tx_active_relay_radio_a_ = 0; // Cannot reschedule, abort
            }
            xSemaphoreGive(self->interlock_mutex_);
            return;
        } else {
            ESP_LOGV(TAG, "Radio A restore timer: Radio B is using port %d (relay %d), Radio A can safely restore to port %d (relay %d).",
                     radio_b_port_idx + 1, active_b_relay, radio_a_port_idx + 1, self->pre_tx_active_relay_radio_a_);
        }
    } else {
        ESP_LOGV(TAG, "Radio A restore timer: Radio B is not using any relay, Radio A can safely restore to relay %d.", self->pre_tx_active_relay_radio_a_);
    }

    if (self->is_radio_a_transmitting_effective()) {
        ESP_LOGW(TAG, "Radio A restore timer: Radio A is now transmitting on its own. Aborting restoration of its previous relay %d.", self->pre_tx_active_relay_radio_a_);
        self->pre_tx_active_relay_radio_a_ = 0; 
        xSemaphoreGive(self->interlock_mutex_);
        return;
    }

    if (!self->relay_controller_) {
        ESP_LOGE(TAG, "Radio A restore timer: Relay controller not initialized.");
        self->pre_tx_active_relay_radio_a_ = 0; 
        xSemaphoreGive(self->interlock_mutex_);
        return;
    }
    
    ESP_LOGD(TAG, "Radio A restore timer: Attempting to restore Radio A relay %d to ON.", self->pre_tx_active_relay_radio_a_);
    esp_err_t err = self->relay_controller_->set_relay(self->pre_tx_active_relay_radio_a_, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Radio A restore timer: Failed to restore Radio A relay %d: %s", self->pre_tx_active_relay_radio_a_, esp_err_to_name(err));
        if (err == ESP_ERR_TIMEOUT && self->radio_a_restore_delay_timer_ != nullptr && self->pre_tx_active_relay_radio_a_ != 0) {
            ESP_LOGW(TAG, "Radio A restore timer: Rescheduling restoration for relay %d due to timeout error from set_relay.", self->pre_tx_active_relay_radio_a_);
            const auto& cached_cfg = self->get_cached_config();
            uint16_t current_delay_ms = cached_cfg.radio_restore_delay_ms;
            if (current_delay_ms < 1 || current_delay_ms > 5000) current_delay_ms = 200; // Fallback
            
            uint16_t reschedule_delay_ms = current_delay_ms / 2;
            if (reschedule_delay_ms < 50) reschedule_delay_ms = 50; // Minimum 50ms

            ESP_LOGD(TAG, "Radio A restore timer: Rescheduling for relay %d in %u ms (set_relay timeout)", self->pre_tx_active_relay_radio_a_, reschedule_delay_ms);
            if (esp_timer_start_once(self->radio_a_restore_delay_timer_, reschedule_delay_ms * 1000) == ESP_OK) {
                xSemaphoreGive(self->interlock_mutex_);
                return; 
            } else {
                ESP_LOGE(TAG, "Radio A restore timer: Failed to reschedule after set_relay error. Aborting restoration for relay %d.", self->pre_tx_active_relay_radio_a_);
            }
        }
        self->pre_tx_active_relay_radio_a_ = 0; 
    } else {
        ESP_LOGD(TAG, "Radio A restore timer: Successfully restored Radio A relay %d.", self->pre_tx_active_relay_radio_a_);
        self->pre_tx_active_relay_radio_a_ = 0; 
    }
    xSemaphoreGive(self->interlock_mutex_);
}


esp_err_t AntennaSwitch::update_last_used_antenna_preference(const int activated_relay_id, RadioID radio_of_activated_relay, int band_idx_for_preference) {
    const auto current_cfg_ptr = std::make_unique<antenna_switch_config_t>();
    if (!current_cfg_ptr) {
        ESP_LOGE(TAG, "Failed to allocate memory for config in update_last_used_antenna_preference; skipping preference update.");
        return ESP_ERR_NO_MEM;
    }

    if (get_config(current_cfg_ptr.get()) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get_config in update_last_used_antenna_preference; skipping preference update.");
        return ESP_FAIL;
    }

    // Use the passed band_idx_for_preference directly
    int band_idx = band_idx_for_preference;
    const auto radio_numeric_idx = static_cast<size_t>(radio_of_activated_relay);

    // For logging purposes, get the current CAT frequency if it's Radio A
    uint32_t current_cat_freq_for_logging = 0;
    if (radio_of_activated_relay == RadioID::A) {
        current_cat_freq_for_logging = CatParser::instance().get_frequency();
    }

    if (band_idx == -1) { 
        ESP_LOGD(TAG, "No band context (band_idx: -1) provided for Radio %c preference update (Relay: %d, current CAT Freq: %lu Hz). Not updating preference.",
                 (radio_of_activated_relay == RadioID::A ? 'A' : 'B'), activated_relay_id, current_cat_freq_for_logging);
        return ESP_OK; 
    }

    // Ensure band_idx is valid for the current config
    if (band_idx < 0 || band_idx >= current_cfg_ptr->num_bands) {
        ESP_LOGW(TAG, "Invalid band_idx %d provided for preference update (Num bands: %d, Radio: %c, Relay: %d, current CAT Freq: %lu Hz). Not updating preference.",
                 band_idx, current_cfg_ptr->num_bands, (radio_of_activated_relay == RadioID::A ? 'A' : 'B'), activated_relay_id, current_cat_freq_for_logging);
        return ESP_OK; 
    }

    // Check if the selected relay is actually configured for this band for this radio
    // relay_id is 1-based, antenna_ports is 0-based.
    // The port_index for the band_config_t.antenna_ports array is 0-based and relative to the radio.
    int port_index_in_band_config = -1;
    if (radio_of_activated_relay == RadioID::A && activated_relay_id >= 1 && activated_relay_id <=
        RelayController::RELAYS_PER_RADIO)
    {
        port_index_in_band_config = activated_relay_id - 1; // e.g. relay 1 is port 0 for Radio A
    }
    else if (radio_of_activated_relay == RadioID::B && activated_relay_id > RelayController::RELAYS_PER_RADIO &&
        activated_relay_id <= RelayController::NUM_RELAYS)
    {
        // e.g. relay 9 is port 0 for Radio B ( (9-1) % 8 = 0 if RELAYS_PER_RADIO is 8)
        port_index_in_band_config = (activated_relay_id - 1) % RelayController::RELAYS_PER_RADIO;
    }

    if (port_index_in_band_config == -1 || port_index_in_band_config >= current_cfg_ptr->num_antenna_ports ||
        !current_cfg_ptr->bands[radio_numeric_idx][band_idx].antenna_ports[port_index_in_band_config]) {
        ESP_LOGW(TAG, "Relay %d (port_idx %d) is not configured for Radio %c, Band %d (current CAT Freq: %lu Hz). Not updating preference.",
                 activated_relay_id, 
                 (port_index_in_band_config != -1 ? port_index_in_band_config + 1 : -1), // Display 1-based or -1 if invalid
                 (radio_of_activated_relay == RadioID::A ? 'A' : 'B'), band_idx, current_cat_freq_for_logging);
        return ESP_OK; 
    }

    if (current_cfg_ptr->last_used_antenna[radio_numeric_idx][band_idx] != static_cast<uint8_t>(activated_relay_id)) {
        ESP_LOGD(TAG, "Updating last used antenna for Radio %c, Band %d to Relay %d (current CAT Freq: %lu Hz)",
                 (radio_of_activated_relay == RadioID::A ? 'A' : 'B'), band_idx, activated_relay_id, current_cat_freq_for_logging);
        current_cfg_ptr->last_used_antenna[radio_numeric_idx][band_idx] = static_cast<uint8_t>(activated_relay_id);

        if (const esp_err_t save_err = ConfigManager::instance().update_config(*current_cfg_ptr); save_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save updated last_used_antenna: %s", esp_err_to_name(save_err));
            return save_err; // Propagate the save error
        }
    }
    return ESP_OK;
}

// Helper to attempt restoration of relays deselected by auto-resolved port conflicts
void AntennaSwitch::attempt_restore_auto_resolved_radio_a_relay() {
    // Check if conditions for auto-restoration are met
    if (const auto& config = get_cached_config(); config.radio_operation_mode != RADIO_OP_MODE_CONCURRENT_AB ||
        !config.interlock_auto_resolves_conflict ||
        !config.auto_restore_on_conflict_resolution) {
        // If conditions are not met, and a relay was stored, log and clear it.
        if (auto_resolved_conflict_prev_a_relay_ != 0) {
            ESP_LOGD(TAG, "Auto-restore conditions not met (mode/interlock/auto_restore_setting). Clearing stored Radio A relay %d.", auto_resolved_conflict_prev_a_relay_);
            auto_resolved_conflict_prev_a_relay_ = 0;
        }
        return;
    }

    if (auto_resolved_conflict_prev_a_relay_ == 0) {
        // ESP_LOGD(TAG, "No Radio A relay stored from auto-resolved conflict to restore.");
        return;
    }

    // Check if Radio A is currently transmitting (should not restore if it is)
    if (is_radio_a_transmitting_effective()) {
        ESP_LOGW(TAG, "Radio A is currently transmitting. Cannot restore auto-resolved relay %d.", auto_resolved_conflict_prev_a_relay_);
        // Do not clear auto_resolved_conflict_prev_a_relay_ here, as TX state might change.
        return;
    }

    // Check if the port is now free (i.e., Radio B is not using it or is not transmitting on it)
    const int active_b_relay = get_active_relay_for_radio(RadioID::B);
    const int port_idx_of_stored_a_relay = (auto_resolved_conflict_prev_a_relay_ - 1) % RelayController::RELAYS_PER_RADIO;
    
    if (active_b_relay != 0) {
        if (const int port_idx_of_active_b_relay = (active_b_relay - 1) % RelayController::RELAYS_PER_RADIO;
            port_idx_of_active_b_relay == port_idx_of_stored_a_relay)
        {
            // Port is still in use by Radio B. Check if Radio B is transmitting.
            if (is_radio_b_transmitting_effective()) {
                 ESP_LOGW(TAG, "Cannot restore Radio A relay %d. Port %d is in use by Radio B (relay %d) AND Radio B is transmitting.",
                         auto_resolved_conflict_prev_a_relay_, port_idx_of_stored_a_relay + 1, active_b_relay);
                return;
            }
            ESP_LOGD(TAG, "Port %d for Radio A relay %d is currently used by Radio B (relay %d), but B is not TXing. Attempting restore for A.",
                     port_idx_of_stored_a_relay + 1, auto_resolved_conflict_prev_a_relay_, active_b_relay);
        }
    }

    ESP_LOGV(TAG, "Attempting to restore Radio A relay %d (deselected by auto-resolved conflict).", auto_resolved_conflict_prev_a_relay_);
    // Use set_relay_for_antenna to ensure proper mode handling. Band_number can be -1 if not relevant for direct restore.
    if (const esp_err_t err = set_relay_for_antenna(auto_resolved_conflict_prev_a_relay_, -1, RadioID::A, true); err == ESP_OK) {
        ESP_LOGV(TAG, "Successfully restored Radio A relay %d.", auto_resolved_conflict_prev_a_relay_);
        auto_resolved_conflict_prev_a_relay_ = 0; // Clear after successful restoration
    } else {
        ESP_LOGE(TAG, "Failed to restore Radio A relay %d: %s. It might be restored later if conditions change.", auto_resolved_conflict_prev_a_relay_, esp_err_to_name(err));
    }
}

void AntennaSwitch::attempt_restore_auto_resolved_radio_b_relay() {
    if (const auto& config = get_cached_config(); config.radio_operation_mode != RADIO_OP_MODE_CONCURRENT_AB ||
        !config.interlock_auto_resolves_conflict ||
        !config.auto_restore_on_conflict_resolution) {
        if (auto_resolved_conflict_prev_b_relay_ != 0) {
            ESP_LOGD(TAG, "Auto-restore conditions not met (mode/interlock/auto_restore_setting). Clearing stored Radio B relay %d.", auto_resolved_conflict_prev_b_relay_);
            auto_resolved_conflict_prev_b_relay_ = 0;
        }
        return;
    }

    if (auto_resolved_conflict_prev_b_relay_ == 0) {
        // ESP_LOGD(TAG, "No Radio B relay stored from auto-resolved conflict to restore.");
        return;
    }

    // Don't restore if Radio B is transmitting
    if (is_radio_b_transmitting_effective()) {
        ESP_LOGW(TAG, "Radio B is currently transmitting. Cannot restore auto-resolved relay %d.", auto_resolved_conflict_prev_b_relay_);
        return;
    }

    // Get the port index of the stored Radio B relay we want to restore
    const int port_idx_of_stored_b_relay = (auto_resolved_conflict_prev_b_relay_ - 1) % RelayController::RELAYS_PER_RADIO;

    // Check if Radio A is using the same port
    if (const int active_a_relay = get_active_relay_for_radio(RadioID::A); active_a_relay != 0) {
        // If Radio A is using the same port, we cannot restore Radio B's relay
        if (const int port_idx_of_active_a_relay = (active_a_relay - 1) % RelayController::RELAYS_PER_RADIO;
            port_idx_of_active_a_relay == port_idx_of_stored_b_relay)
        {
            ESP_LOGW(TAG, "Cannot restore Radio B relay %d. Port %d is still in use by Radio A (relay %d).",
                     auto_resolved_conflict_prev_b_relay_, port_idx_of_stored_b_relay + 1, active_a_relay);
                     
            // If Radio A is transmitting, just return (we'll try again later)
            if (is_radio_a_transmitting_effective()) {
                ESP_LOGW(TAG, "Additionally, Radio A is currently transmitting on this port.");
                return;
            }
            
            // Even if Radio A is not transmitting, we still can't restore B to the same port
            ESP_LOGD(TAG, "Radio A is not currently transmitting, but still using the port. Cannot restore Radio B relay.");
            return;
        }
    }

    // Radio A is not using the same port, so we can safely attempt to restore Radio B's relay
    ESP_LOGD(TAG, "Attempting to restore Radio B relay %d (deselected by auto-resolved conflict).", auto_resolved_conflict_prev_b_relay_);
    if (const esp_err_t err = set_relay_for_antenna(auto_resolved_conflict_prev_b_relay_, -1, RadioID::B, true); err ==
        ESP_OK)
    {
        ESP_LOGD(TAG, "Successfully restored Radio B relay %d.", auto_resolved_conflict_prev_b_relay_);
        auto_resolved_conflict_prev_b_relay_ = 0; 
    } else {
        ESP_LOGE(TAG, "Failed to restore Radio B relay %d: %s. It might be restored later if conditions change.", auto_resolved_conflict_prev_b_relay_, esp_err_to_name(err));
    }
}


esp_err_t AntennaSwitch::set_relay(const int relay_id, const bool state) {
    ESP_LOGD(TAG, "AntennaSwitch: Setting relay %d to %s", relay_id, state ? "ON" : "OFF");

    if (relay_id < 1 || relay_id > RelayController::NUM_RELAYS) {
        ESP_LOGE(TAG, "Invalid relay ID: %d", relay_id);
        return ESP_ERR_INVALID_ARG;
    }

    if (!relay_controller_) {
        ESP_LOGE(TAG, "Relay controller not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // Determine which radio this relay belongs to
    const bool is_this_radio_a_relay = (relay_id >= 1 && relay_id <= RelayController::RELAYS_PER_RADIO);
    const RadioID radio_context = is_this_radio_a_relay ? RadioID::A : RadioID::B;

    // Try to determine the current band from CAT frequency for preference tracking
    // Only save preference if the selected antenna is configured for the current band
    int band_number_for_preference = -1;  // Default: no band context

    if (state && radio_context == RadioID::A) {
        // For Radio A, we can get the current frequency from CAT parser
        const uint32_t current_frequency = CatParser::instance().get_frequency();

        if (current_frequency > 0) {
            // Look up which band this frequency belongs to
            const auto& config = get_cached_config();
            const auto radio_idx = static_cast<size_t>(RadioID::A);

            for (int i = 0; i < config.num_bands; i++) {
                const auto& band = config.bands[radio_idx][i];
                if (current_frequency >= band.start_freq && current_frequency <= band.end_freq) {
                    // Found the band for current frequency
                    // Now check if this relay is configured for this band
                    int port_idx = relay_id - 1; // Convert 1-based relay to 0-based port

                    if (port_idx >= 0 && port_idx < config.num_antenna_ports &&
                        band.antenna_ports[port_idx]) {
                        // This antenna IS configured for this band - save preference
                        band_number_for_preference = i;
                        ESP_LOGD(TAG, "Manual relay %d selection on band %d (%s, %lu Hz): antenna IS configured for band, will save preference",
                                 relay_id, i, band.description, current_frequency);
                    } else {
                        // This antenna is NOT configured for this band - temporary override
                        ESP_LOGD(TAG, "Manual relay %d selection on band %d (%s, %lu Hz): antenna NOT configured for band, temporary override (no preference save)",
                                 relay_id, i, band.description, current_frequency);
                    }
                    break;
                }
            }
        }
    }

    // All logic for TX interlock, conflict anticipation, preference update, and restoration
    // is handled by set_relay_for_antenna.
    // band_number_for_preference will be valid only if the antenna is configured for the current band
    return set_relay_for_antenna(relay_id, band_number_for_preference, radio_context, state);
}

esp_err_t AntennaSwitch::set_relay_radio_b(const int relay_id, const bool state) {
    // Ensure relay_id is within Radio B's range (e.g., 9-16 if RELAYS_PER_RADIO is 8)
    // This check is also implicitly handled by set_relay_for_antenna's call to relay_controller,
    // but an early exit here can be useful.
    if (relay_id <= RelayController::RELAYS_PER_RADIO || relay_id > RelayController::NUM_RELAYS) {
        ESP_LOGE(TAG, "Invalid relay_id %d for Radio B operation in set_relay_radio_b.", relay_id);
        return ESP_ERR_INVALID_ARG;
    }
    // The band_number is -1 as this is a direct relay set.
    // All logic for TX interlock, conflict anticipation, preference update, and restoration
    // is handled by set_relay_for_antenna.
    // Note: set_relay_for_antenna will also check RADIO_OP_MODE_SINGLE_A.
    return set_relay_for_antenna(relay_id, /*band_number=*/-1, RadioID::B, state);
}

esp_err_t AntennaSwitch::get_relay_state(const int relay_id, bool *state) {
    if (state == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!relay_controller_) {
        ESP_LOGE(TAG, "Relay controller not initialized for get_relay_state");
        return ESP_ERR_INVALID_STATE;
    }
    *state = relay_controller_->get_relay_state(relay_id);
    return ESP_OK;
}

esp_err_t AntennaSwitch::restart() {
    ESP_LOGI(TAG, "Restarting device...");

    // Attempt to flush any pending configuration changes to NVS
    ESP_LOGI(TAG, "Flushing pending configuration to NVS before restart...");
    if (const esp_err_t flush_err = ConfigManager::instance().flush_pending_save(pdMS_TO_TICKS(1000)); flush_err == ESP_OK) {
        ESP_LOGI(TAG, "Pending configuration successfully flushed to NVS.");
    } else if (flush_err == ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "Timeout flushing configuration to NVS before restart. Changes might not be saved.");
    } else {
        ESP_LOGE(TAG, "Error flushing configuration to NVS: %s. Changes might not be saved.", esp_err_to_name(flush_err));
    }

    // Small delay to allow logging to complete and any other brief OS tasks
    vTaskDelay(pdMS_TO_TICKS(200));

    esp_restart();
    return ESP_OK; // esp_restart() does not return
}

esp_err_t AntennaSwitch::set_relay_for_antenna(const int relay_id, const int band_number, const RadioID radio, const bool state) {
    if (!relay_controller_) {
        ESP_LOGE(TAG, "Relay controller not initialized for set_relay_for_antenna");
        return ESP_ERR_INVALID_STATE;
    }
    const auto& config = get_cached_config(); // Use cached config, get early

    // For conflict restoration logic tracking, get the port index we're trying to activate
    int activating_radio_port_idx = -1;
    if (state) {
        activating_radio_port_idx = (relay_id - 1) % RelayController::RELAYS_PER_RADIO;
    }

    // Expect and store other radio's state if a port conflict will be auto-resolved
    if (state && config.radio_operation_mode == RADIO_OP_MODE_CONCURRENT_AB && config.interlock_auto_resolves_conflict) {
        const RadioID other_radio_id = (radio == RadioID::A) ? RadioID::B : RadioID::A;
        const int active_other_relay = get_active_relay_for_radio(other_radio_id);

        ESP_LOGD(TAG, "Port conflict anticipation: Radio %c activating port %d, checking Radio %c (active relay: %d)",
                 (radio == RadioID::A ? 'A' : 'B'), activating_radio_port_idx + 1,
                 (other_radio_id == RadioID::A ? 'A' : 'B'), active_other_relay);

        if (active_other_relay != 0) {
            // Port indices are 0-based from the perspective of a single radio's available ports

            if (const int other_radio_active_port_idx = (active_other_relay - 1) % RelayController::RELAYS_PER_RADIO; activating_radio_port_idx == other_radio_active_port_idx) {
                // Conflict will be auto-resolved by RelayController, deactivating other_radio's relay.
                // Store this relay to allow restoration later when the conflict is resolved
                if (other_radio_id == RadioID::B) {
                    if (auto_resolved_conflict_prev_b_relay_ == 0) { // Only if not already set
                        ESP_LOGD(TAG, "Anticipating Radio B relay %d (port %d) deactivation by Radio A (activating relay %d, port %d). Storing for auto-restoration.",
                                 active_other_relay, other_radio_active_port_idx + 1, relay_id, activating_radio_port_idx + 1);
                        auto_resolved_conflict_prev_b_relay_ = active_other_relay;
                    }
                    // ALSO store in TX interlock variable to enable timer-based restoration
                    if (pre_tx_active_relay_radio_b_ == 0) {
                        ESP_LOGD(TAG, "Storing Radio B relay %d in TX interlock variable for timer-based restoration.", active_other_relay);
                        pre_tx_active_relay_radio_b_ = active_other_relay;
                    }
                } else { // other_radio_id == RadioID::A
                    if (auto_resolved_conflict_prev_a_relay_ == 0) {
                        ESP_LOGD(TAG, "Anticipating Radio A relay %d (port %d) deactivation by Radio B (activating relay %d, port %d). Storing for auto-restoration.",
                                 active_other_relay, other_radio_active_port_idx + 1, relay_id, activating_radio_port_idx + 1);
                        auto_resolved_conflict_prev_a_relay_ = active_other_relay;
                    }
                    // ALSO store in TX interlock variable to enable timer-based restoration
                    if (pre_tx_active_relay_radio_a_ == 0) {
                        ESP_LOGD(TAG, "Storing Radio A relay %d in TX interlock variable for timer-based restoration.", active_other_relay);
                        pre_tx_active_relay_radio_a_ = active_other_relay;
                    }
                }
            }
        }
    }

    // Add TX interlock checks here too, if setting a relay ON.
    if (state == true) { // Trying to activate an antenna
        // const auto& config = get_cached_config(); // Already fetched
        if (radio == RadioID::A) {
            // Hot-switching protection for Radio A itself:
            if (is_radio_a_transmitting_effective()) {
                ESP_LOGW(TAG, "Cannot set antenna for Radio A; Radio A is currently transmitting (effective).");
                return ESP_ERR_INVALID_STATE;
            }
            // Interlock: Check if Radio B is transmitting
            if (config.radio_operation_mode != RADIO_OP_MODE_SINGLE_A && is_radio_b_transmitting_effective()) {
               ESP_LOGW(TAG, "Cannot set antenna for Radio A; Radio B is transmitting (effective).");
               return ESP_ERR_INVALID_STATE;
            }
        } else { // RadioID::B
            if (config.radio_operation_mode == RADIO_OP_MODE_SINGLE_A) {
                 ESP_LOGW(TAG, "Cannot set antenna for Radio B; Single A mode active.");
                 if (state) return ESP_OK; // Acknowledge but don't act on ON request in single A mode
            }
            // Hot-switching protection for Radio B itself:
            if (is_radio_b_transmitting_effective()) {
               ESP_LOGW(TAG, "Cannot set antenna for Radio B; Radio B is currently transmitting (effective).");
               return ESP_ERR_INVALID_STATE;
            }
            // Interlock: Check if Radio A is transmitting
            if (is_radio_a_transmitting_effective()) {
                ESP_LOGW(TAG, "Cannot set antenna for Radio B; Radio A is transmitting (effective).");
                return ESP_ERR_INVALID_STATE;
            }
        }
    }
    
    // Execute the relay change
    const esp_err_t ret = relay_controller_->set_relay_for_antenna(relay_id, band_number, radio, state);

    if (ret == ESP_OK && state) {
        // Update last used antenna preference if a relay was successfully turned ON
        // Pass band_number which was determined by set_frequency or passed directly.
        // If band_number is -1 (e.g. direct set_relay call), preference update will skip if band_idx is -1.
        if (const esp_err_t pref_err = update_last_used_antenna_preference(relay_id, radio, band_number); pref_err != ESP_OK &&
            pref_err != ESP_ERR_NO_MEM && pref_err != ESP_FAIL)
        {
            ESP_LOGE(TAG, "Error updating antenna preference for relay %d, radio %c, band %d: %s. Relay operation itself was successful.",
                     relay_id, (radio == RadioID::A ? 'A' : 'B'), band_number, esp_err_to_name(pref_err));
        }

        // Broadcast status update for band/frequency changes (not manual relay control)
        // band_number >= 0 indicates this was triggered by a frequency/band change
        if (band_number >= 0 && websocket_server_is_running()) {
            WebSocketServer::instance().broadcast_status_update();
            ESP_LOGD(TAG, "Broadcasted status update for band change: relay %d, radio %c, band %d",
                     relay_id, (radio == RadioID::A ? 'A' : 'B'), band_number);
        }

        // Logic for immediate restoration if the current activation resolves a conflict for the *other* radio.
        // This considers both TX-interlock-stored relays and auto-conflict-resolved relays.
        const RadioID other_radio = (radio == RadioID::A) ? RadioID::B : RadioID::A;
        int* other_radio_pre_tx_relay_ptr = (other_radio == RadioID::A) ? &pre_tx_active_relay_radio_a_ : &pre_tx_active_relay_radio_b_;
        int* other_radio_auto_conflict_relay_ptr = (other_radio == RadioID::A) ? &auto_resolved_conflict_prev_a_relay_ : &auto_resolved_conflict_prev_b_relay_;
        const esp_timer_handle_t* other_radio_restore_timer_ptr = (other_radio == RadioID::A) ? &radio_a_restore_delay_timer_ : &radio_b_restore_delay_timer_;

        int relay_to_restore_for_other_radio = 0;
        if (*other_radio_pre_tx_relay_ptr != 0) {
            relay_to_restore_for_other_radio = *other_radio_pre_tx_relay_ptr;
        } else if (*other_radio_auto_conflict_relay_ptr != 0) {
            relay_to_restore_for_other_radio = *other_radio_auto_conflict_relay_ptr;
        }

        if (relay_to_restore_for_other_radio != 0) {
            // activating_radio_port_idx was determined earlier in this function if state is true
            if (const int stored_other_radio_port_idx = (relay_to_restore_for_other_radio - 1) %
                RelayController::RELAYS_PER_RADIO; activating_radio_port_idx != stored_other_radio_port_idx)
            {
                ESP_LOGI(TAG, "Radio %c switched to port %d, other Radio %c can now be restored to port %d (relay %d). Triggering immediate restoration.",
                         (radio == RadioID::A ? 'A' : 'B'), activating_radio_port_idx + 1,
                         (other_radio == RadioID::A ? 'A' : 'B'), stored_other_radio_port_idx + 1, relay_to_restore_for_other_radio);

                if (const esp_err_t restore_err = set_relay_for_antenna(relay_to_restore_for_other_radio, -1,
                                                                        other_radio, true); restore_err == ESP_OK)
                {
                    ESP_LOGD(TAG, "Successfully restored Radio %c relay %d immediately.", (other_radio == RadioID::A ? 'A' : 'B'), relay_to_restore_for_other_radio);
                    *other_radio_pre_tx_relay_ptr = 0;
                    *other_radio_auto_conflict_relay_ptr = 0;
                } else {
                    ESP_LOGW(TAG, "Failed to immediately restore Radio %c relay %d: %s. Will retry via timer if applicable.",
                             (other_radio == RadioID::A ? 'A' : 'B'), relay_to_restore_for_other_radio, esp_err_to_name(restore_err));
                    // If pre_tx_active_relay was the source, timer will handle it.
                    // If auto_resolved_conflict_prev was the source, and pre_tx is 0, we might want to set pre_tx to allow timer retry.
                    if (*other_radio_pre_tx_relay_ptr == 0 && *other_radio_auto_conflict_relay_ptr != 0) {
                         *other_radio_pre_tx_relay_ptr = *other_radio_auto_conflict_relay_ptr; // Allow timer to pick it up
                    }
                    if (other_radio_restore_timer_ptr != nullptr && *other_radio_restore_timer_ptr != nullptr && *other_radio_pre_tx_relay_ptr != 0) {
                        uint16_t configured_delay_ms = config.radio_restore_delay_ms;
                        // Validate the delay again here for safety, though it should be valid from config.
                        if (configured_delay_ms < 1) configured_delay_ms = 1;
                        if (configured_delay_ms > 5000) configured_delay_ms = 200; // Fallback to a reasonable default if somehow out of typical range for a retry

                        if (configured_delay_ms < 1) configured_delay_ms = 1; // Ensure at least 1ms

                        if (esp_timer_start_once(*other_radio_restore_timer_ptr, configured_delay_ms * 1000) == ESP_OK) {
                            ESP_LOGD(TAG, "Scheduled Radio %c restoration retry via timer with %u ms delay.", (other_radio == RadioID::A ? 'A' : 'B'), configured_delay_ms);
                        } else {
                            ESP_LOGE(TAG, "Failed to start Radio %c restore timer for retry.", (other_radio == RadioID::A ? 'A' : 'B'));
                        }
                    }
                }
            }
        }
    }
    return ret;
}
