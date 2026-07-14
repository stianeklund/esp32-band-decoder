#include "kenwood_cat.h"
#include "esp_log.h"
#include "esp_err.h"
#include "config_manager.h"
#include "antenna_switch.h"
#include <charconv>
#include <chrono>
#include <memory>
#include <string_view>

KenwoodCat::KenwoodCat() {
    // Kenwood 2-char command → handler map (owned by this subclass; the base
    // provides the shared dispatch_lookup() mechanism).
    command_handlers_ = {
        {'F' << 8 | 'A', &KenwoodCat::process_fa_command},
        {'F' << 8 | 'B', &KenwoodCat::process_fb_command},
        {'A' << 8 | 'I', &KenwoodCat::process_ai_command},
        {'A' << 8 | 'P', &KenwoodCat::process_ap_command},
        {'I' << 8 | 'F', &KenwoodCat::process_if_command},
        {'T' << 8 | 'X', &KenwoodCat::process_tx_command},
        {'R' << 8 | 'X', &KenwoodCat::process_rx_command},
        {'E' << 8 | 'X', &KenwoodCat::process_ex_command},
        {'X' << 8 | 'O', &KenwoodCat::process_xo_command}
    };
}

esp_err_t KenwoodCat::dispatch_one_command(std::string_view command_view) {
    return dispatch_lookup(command_handlers_, this, command_view);
}

void KenwoodCat::poll_for_updates() {
    // Check for async AI probe request (runs in UART task context, non-blocking for webserver)
    if (ai_probe_requested_.exchange(false)) {
        ESP_LOGI(TAG, "Processing async AI probe request");
        probe_and_configure_ai_mode();
    }

    // Periodic AI query: if ai_mode enabled, no recent valid commands, and enough time since last query
    bool auto_mode = false;
    bool ai_mode = false;
    ConfigManager::instance().get_cat_modes(auto_mode, ai_mode);
    if (auto_mode && ai_mode) {
        const auto now = std::chrono::steady_clock::now();
        auto time_since_valid_cmd = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_valid_command_time).count();
        auto time_since_ai_query = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_ai_query_time).count();

        // If no valid commands recently and enough time since last AI query, send AI;
        if (time_since_valid_cmd > SERIAL_DATA_TIMEOUT_S && time_since_ai_query >= AI_QUERY_INTERVAL_S) {
            ESP_LOGI(TAG, "No valid CAT commands for %lld sec, sending periodic AI query", time_since_valid_cmd);
            last_ai_query_time = now;
            send_to_radio("AI;");
        }
    }
}

esp_err_t KenwoodCat::process_ai_command(const std::string_view command_payload) {
    if (command_payload.empty()) {
        // This is a query command: AI;
        // A Kenwood radio would respond AI<P1>; e.g. AI0; if no auto information,or AI2; if auto information
        // For now, we just log that we received the query.
        ESP_LOGV(TAG, "AI Query (AI;) received. Radio Auto Information is currently %s. Polling %s.",
                 radio_provides_auto_updates_ ? "ON" : "OFF",
                 radio_provides_auto_updates_ ? "not required" : "required");
        // TODO: Implement response if this system needs to act as a device responding to AI;
        return ESP_OK;
    }

    if (command_payload.length() == 1) {
        // This is a set command: AI<P1>;
        const char p1_val = command_payload[0];
        ESP_LOGD(TAG, "Processing Kenwood AI Set command (AI%c;)", p1_val);

        switch (p1_val) {
        case '0': // Auto Information OFF
            radio_provides_auto_updates_ = false;
            ESP_LOGI(TAG, "Kenwood AI Set: Radio Auto Information OFF. CAT polling will be required for updates.");

            // Check if we should automatically enable AI2 mode
            {
                bool auto_mode = false;
                bool ai_mode = false;
                ConfigManager::instance().get_cat_modes(auto_mode, ai_mode);
                if (auto_mode && ai_mode) {
                    ESP_LOGI(TAG, "AI mode enabled in config, automatically sending AI2 to enable auto information");
                    esp_err_t send_err = send_to_radio("AI2;");
                    if (send_err == ESP_OK) {
                        ESP_LOGI(TAG, "Successfully sent AI2 command to radio");
                    } else {
                        ESP_LOGW(TAG, "Failed to send AI2 command: %s", esp_err_to_name(send_err));
                    }
                }
            }
            break;
        case '2': // Auto Information ON
        case '4': // Auto Information ON
            radio_provides_auto_updates_ = true;
            ESP_LOGI(TAG, "Kenwood AI Set: Radio Auto Information ON (P1=%c). CAT polling not required.", p1_val);
            break;
        default:
            ESP_LOGE(TAG, "Kenwood AI Set: Invalid parameter P1='%c'. Expected '0' through '6'.", p1_val);
            return ESP_ERR_INVALID_ARG;
        }
        return ESP_OK;
    }

    // Invalid payload length for AI command (e.g., AI12;)
    ESP_LOGE(TAG, "Kenwood AI command '%.*s' has invalid payload length. Expected empty or 1 char.",
             static_cast<int>(command_payload.length()), command_payload.data());
    return ESP_ERR_INVALID_ARG;
}

std::from_chars_result KenwoodCat::get_from_chars_result(const std::string_view command, unsigned long& ports_val){
    const char* const start_ptr = command.data();
    const auto end_ptr = command.data() + command.length();
    return std::from_chars(start_ptr, end_ptr, ports_val);
}

// ReSharper disable once CppMemberFunctionMayBeStatic
// CUSTOM CAT command
esp_err_t KenwoodCat::process_ap_command(const std::string_view command) {
    unsigned long ports_val;
    const auto end_ptr = command.data() + command.length();
    const auto result = get_from_chars_result(command, ports_val);

    if (result.ec == std::errc() && result.ptr == end_ptr) {
        ESP_LOGD(TAG, "Setting antenna ports (AP): %lu", ports_val);

        if (ports_val > UINT8_MAX) {
            ESP_LOGE(TAG, "Ports value %lu for AP command exceeds uint8_t max", ports_val);
            return ESP_ERR_INVALID_ARG;
        }

        const auto config_snapshot = std::make_unique<antenna_switch_config_t>();
        if (!config_snapshot) {
            ESP_LOGE(TAG, "Failed to allocate config snapshot for AP command");
            return ESP_ERR_NO_MEM;
        }
        ConfigManager::instance().get_config(*config_snapshot);

        // Modify the copy
        config_snapshot->num_antenna_ports = static_cast<uint8_t>(ports_val);

        // Set the modified config
        esp_err_t ret = AntennaSwitch::instance().set_config(config_snapshot.get());
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set config for AP command: %s", esp_err_to_name(ret));
            return ret;
        }
        // If set_config was successful, update CatParser's own current_config member
        // to reflect this change immediately.
        ConfigManager::instance().get_config(current_config);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Invalid ports format in AP command: %.*s",
             static_cast<int>(command.length()), command.data());
    return ESP_ERR_INVALID_ARG;
}

esp_err_t KenwoodCat::process_if_command(const std::string_view command) {
    if (command.length() < 35) {
        ESP_LOGW(TAG, "IF command too short: %.*s",
                 static_cast<int>(command.length()), command.data());
        return ESP_OK;
    }

    uint32_t frequency;
    // Parse frequency (first 11 chars of the command payload)
    const std::string_view freq_sv = command.substr(0, 11);
    const char* const freq_start_ptr = freq_sv.data();
    const char* const freq_end_ptr = freq_sv.data() + freq_sv.length();

    // ReSharper disable once CppUseStructuredBinding
    auto fc_result = std::from_chars(freq_start_ptr, freq_end_ptr, frequency);

    if (fc_result.ec != std::errc() || fc_result.ptr != freq_end_ptr) {
        ESP_LOGW(TAG, "Invalid frequency in IF command: %.*s", static_cast<int>(freq_sv.length()), freq_sv.data());
        return ESP_OK; // Keep original behavior
    }

    const auto new_tx_state = command[26] == '1';

    // Parse mode
    const char mode_char = command[27];
    std::string new_mode_str; // Renamed to avoid conflict with member `current_mode`

    switch (mode_char) {
        case '1': new_mode_str = "LSB"; break;
        case '2': new_mode_str = "USB"; break;
        case '3': new_mode_str = "CW-U"; break;
        case '4': new_mode_str = "FM"; break;
        case '5': new_mode_str = "AM"; break;
        case '6': new_mode_str = "DIG-L"; break;
        case '7': new_mode_str = "CW-L"; break;
        case '9': new_mode_str = "DIG-U"; break;
        default:  new_mode_str = "UNKNOWN";
    }

    // Parse VFO selection (P10 field at position 28)
    // P10: 0 = VFO A, 1 = VFO B, 2 = Memory
    const uint8_t vfo_selection = command[28] - '0';  // Convert ASCII to number

    const bool current_tx_state = transmitting.load();
    const bool tx_state_changed = new_tx_state != current_tx_state;
    const std::string old_mode = current_mode;                   // Capture current mode before update

    ESP_LOGD(TAG, "IF command parsing: freq=%lu, vfo_sel=%u, new_tx_state=%d, current_tx_state=%d, tx_changed=%d",
             frequency, vfo_selection, new_tx_state, current_tx_state, tx_state_changed);

    // Update VFO-specific frequency storage
    if (vfo_selection == 0) {
        vfo_a_frequency = frequency;
    } else if (vfo_selection == 1) {
        vfo_b_frequency = frequency;
    }

    // Track which VFO is active
    active_vfo = vfo_selection;

    // Update internal states
    current_mode = new_mode_str;

    if (tx_state_changed) {
        ESP_LOGD(TAG, "Radio %s (IF command)", new_tx_state ? "started transmitting" : "stopped transmitting");
        // The call to set_transmitting will handle updating transmitting member AND notifying AntennaSwitch
        set_transmitting(new_tx_state);
    }
    // If only frequency changed, but TX state did not, we still need to inform AntennaSwitch
    // about the frequency for potential antenna changes.
    // However, if TX state DID change, set_transmitting already called on_cat_tx_a_state_change,
    // which in turn calls on_radio_a_tx_start/stop, which handles frequency.
    // So, direct frequency handling here is mainly for when TX state *doesn't* change.

    if (current_mode != old_mode && !tx_state_changed) {
        ESP_LOGV(TAG, "Mode changed from %s to %s", old_mode.c_str(), current_mode.c_str());
    } else if (current_mode != old_mode && tx_state_changed) {
        ESP_LOGV(TAG, "Mode also changed from %s to %s", old_mode.c_str(), current_mode.c_str());
    }

    ESP_LOGV(TAG, "IF command: freq=%lu Hz, mode=%s, tx=%d, VFO=%c (A_freq=%lu, B_freq=%lu)",
             frequency, current_mode.c_str(), transmitting.load(), vfo_selection == 0 ? 'A' : (vfo_selection == 1 ? 'B' : 'M'),
             vfo_a_frequency, vfo_b_frequency);

    // IF reports the selected VFO and updates active_vfo above, so its frequency is active.
    return handle_frequency_change(frequency);
}

esp_err_t KenwoodCat::process_fa_command(const std::string_view command) {
    uint32_t frequency;

    const char* const start_ptr = command.data();
    const char* const end_ptr = command.data() + command.length();

    // ReSharper disable once CppLocalVariableMayBeConst
    // ReSharper disable once CppTooWideScopeInitStatement
    auto result = std::from_chars(start_ptr, end_ptr, frequency);

    if (result.ec == std::errc() && result.ptr == end_ptr) {
        ESP_LOGV(TAG, "FA command frequency: %lu Hz", frequency);
        // Update VFO A frequency
        vfo_a_frequency = frequency;

        // Only trigger antenna change if VFO A is active
        if (active_vfo == 0) {
            return handle_frequency_change(frequency);
        }
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Invalid frequency format in FA command: %.*s",
             static_cast<int>(command.length()), command.data());
    // We don't want to error here (I guess)
    return ESP_OK;
}

esp_err_t KenwoodCat::process_fb_command(const std::string_view command) {
    uint32_t frequency;

    const char* const start_ptr = command.data();
    const char* const end_ptr = command.data() + command.length();

    auto result = std::from_chars(start_ptr, end_ptr, frequency);

    if (result.ec == std::errc() && result.ptr == end_ptr) {
        ESP_LOGV(TAG, "FB command frequency: %lu Hz", frequency);
        // Update VFO B frequency
        vfo_b_frequency = frequency;

        // Only trigger antenna change if VFO B is active
        if (active_vfo == 1) {
            return handle_frequency_change(frequency);
        }
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Invalid frequency format in FB command: %.*s",
             static_cast<int>(command.length()), command.data());
    return ESP_OK;
}

esp_err_t KenwoodCat::process_ex_command(const std::string_view payload) {
    // EX menu format: [menu(3)][padding "0000"(4)][value(var)]. We only act on
    // menu 056 (Transverter Function); value 1 = ON, 0 = OFF. All other menus and
    // the read/query form (7 chars, no value) are ignored.
    if (payload.length() < 8) {
        return ESP_OK; // read form or malformed: nothing to act on
    }

    unsigned int menu = 0;
    const char* const menu_end = payload.data() + 3;
    if (const auto r = std::from_chars(payload.data(), menu_end, menu);
        r.ec != std::errc() || r.ptr != menu_end) {
        ESP_LOGV(TAG, "EX command: unparseable menu in '%.*s'",
                 static_cast<int>(payload.length()), payload.data());
        return ESP_OK;
    }

    if (menu != 56) {
        return ESP_OK; // not the transverter menu
    }

    const std::string_view value_str = payload.substr(7);
    unsigned int value = 0;
    const char* const val_end = value_str.data() + value_str.length();
    if (const auto r = std::from_chars(value_str.data(), val_end, value);
        r.ec != std::errc() || r.ptr != val_end) {
        ESP_LOGW(TAG, "EX056 (transverter): invalid value '%.*s'",
                 static_cast<int>(value_str.length()), value_str.data());
        return ESP_OK;
    }

    set_transverter_active(value != 0);
    return ESP_OK;
}

esp_err_t KenwoodCat::process_xo_command(const std::string_view payload) {
    // XO format: [direction(1)][offset in Hz(11)]. direction 0=plus, 1=minus.
    // The read query ("XO;") has an empty payload and is ignored.
    if (payload.length() < 2) {
        return ESP_OK;
    }

    const uint8_t dir = (payload[0] == '1') ? 1 : 0;

    const std::string_view offset_str = payload.substr(1);
    uint32_t offset = 0;
    const char* const off_end = offset_str.data() + offset_str.length();
    if (const auto r = std::from_chars(offset_str.data(), off_end, offset);
        r.ec != std::errc() || r.ptr != off_end) {
        ESP_LOGW(TAG, "XO (transverter offset): invalid offset '%.*s'",
                 static_cast<int>(offset_str.length()), offset_str.data());
        return ESP_OK;
    }

    transverter_offset_hz.store(static_cast<int32_t>(offset));
    transverter_minus_dir.store(dir);
    ESP_LOGI(TAG, "Transverter offset set: %s%lu Hz",
             dir ? "-" : "+", static_cast<unsigned long>(offset));

    // If already in transverter mode, the corrected frequency just changed.
    if (transverter_active.load()) {
        broadcast_transverter_state();
    }
    return ESP_OK;
}

esp_err_t KenwoodCat::process_tx_command(const std::string_view payload) {
    (void)payload; // ignore the payload, not needed here
    // Call the centralized set_transmitting method which now notifies AntennaSwitch
    set_transmitting(true);
    // Log message is now part of set_transmitting if state changes, or AntennaSwitch's handlers
    return ESP_OK;
}

esp_err_t KenwoodCat::process_rx_command(const std::string_view payload) {
    (void)payload;
    // Call the centralized set_transmitting method which now notifies AntennaSwitch
    set_transmitting(false);
    // Log message is now part of set_transmitting if state changes, or AntennaSwitch's handlers
    return ESP_OK;
}

esp_err_t KenwoodCat::probe_and_configure_ai_mode() {
    bool auto_mode = false;
    bool ai_mode = false;
    ConfigManager::instance().get_cat_modes(auto_mode, ai_mode);

    // Only proceed if both auto_mode and ai_mode are enabled
    if (!auto_mode || !ai_mode) {
        ESP_LOGI(TAG, "AI mode probing skipped: auto_mode=%s, ai_mode=%s",
                 auto_mode ? "enabled" : "disabled",
                 ai_mode ? "enabled" : "disabled");
        return ESP_OK;
    }

    // Check if we've successfully parsed valid CAT commands recently - if so, AI is likely already enabled
    auto now = std::chrono::steady_clock::now();
    auto time_since_last_valid_command = std::chrono::duration_cast<std::chrono::seconds>(
        now - last_valid_command_time).count();

    if (time_since_last_valid_command < SERIAL_DATA_TIMEOUT_S) {
        ESP_LOGI(TAG, "Already receiving valid CAT commands, AI mode active");
        radio_provides_auto_updates_ = true;
        return ESP_OK;
    }

    // Send initial AI query - periodic probing in uart_task will handle retries
    ESP_LOGI(TAG, "Sending initial AI query (periodic retries every %d sec if no response)", AI_QUERY_INTERVAL_S);
    last_ai_query_time = now;
    return send_to_radio("AI;");
}
