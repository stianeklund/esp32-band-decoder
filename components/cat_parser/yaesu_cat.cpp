#include "yaesu_cat.h"
#include "yaesu_frames.h"
#include "esp_log.h"
#include "esp_err.h"
#include "config_manager.h"
#include <chrono>
#include <string>
#include <string_view>

YaesuCat::YaesuCat() {
    // Yaesu 2-char command -> handler map. NOTE: there is deliberately no 'RX'
    // entry - unlike Kenwood (separate TX;/RX;), Yaesu signals both transmit and
    // receive through a single 'TX' command whose digit carries the state.
    command_handlers_ = {
        {'F' << 8 | 'A', &YaesuCat::process_fa_command},
        {'F' << 8 | 'B', &YaesuCat::process_fb_command},
        {'I' << 8 | 'F', &YaesuCat::process_if_command},
        {'M' << 8 | 'D', &YaesuCat::process_md_command},
        {'T' << 8 | 'X', &YaesuCat::process_tx_command},
        {'A' << 8 | 'I', &YaesuCat::process_ai_command},
    };
}

esp_err_t YaesuCat::dispatch_one_command(std::string_view command_view) {
    return dispatch_lookup(command_handlers_, this, command_view);
}

void YaesuCat::poll_for_updates() {
    // Yaesu requires Auto Information (AI1) ON for CAT-side TX detection: the IF
    // frame has no TX flag, so transmit state arrives ONLY as pushed TX0/1/2 frames.
    // Honor an async probe request by (re-)enabling AI1.
    if (ai_probe_requested_.exchange(false)) {
        ESP_LOGI(TAG, "Yaesu: enabling Auto Information (AI1) on probe request");
        last_ai_query_time = std::chrono::steady_clock::now();
        send_to_radio("AI1;");
    }

    bool auto_mode = false;
    bool ai_mode = false;
    ConfigManager::instance().get_cat_modes(auto_mode, ai_mode);
    if (auto_mode && ai_mode) {
        const auto now = std::chrono::steady_clock::now();
        const auto time_since_valid_cmd = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_valid_command_time).count();
        const auto time_since_ai_query = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_ai_query_time).count();

        // Idle: re-assert AI1 (in case the radio was power-cycled - AI resets to 0
        // when the transceiver turns off) and poll IF for a fresh freq/mode snapshot.
        if (time_since_valid_cmd > SERIAL_DATA_TIMEOUT_S && time_since_ai_query >= AI_QUERY_INTERVAL_S) {
            ESP_LOGI(TAG, "Yaesu: no CAT data for %lld s, re-asserting AI1 and polling IF",
                     static_cast<long long>(time_since_valid_cmd));
            last_ai_query_time = now;
            send_to_radio("AI1;");
            send_to_radio("IF;");
        }
    }
}

const char* YaesuCat::mode_string(const char mode_char) {
    return yaesu::mode_string(mode_char);
}

esp_err_t YaesuCat::process_fa_command(const std::string_view payload) {
    const auto frequency = yaesu::parse_frequency(payload);
    if (!frequency) {
        ESP_LOGW(TAG, "Invalid frequency in Yaesu FA: %.*s",
                 static_cast<int>(payload.length()), payload.data());
        return ESP_OK;
    }

    ESP_LOGV(TAG, "Yaesu FA frequency: %lu Hz", static_cast<unsigned long>(*frequency));
    vfo_a_frequency = *frequency;
    // Only drive an antenna change if VFO A is the active VFO.
    if (active_vfo == 0) {
        return handle_frequency_change(*frequency);
    }
    return ESP_OK;
}

esp_err_t YaesuCat::process_fb_command(const std::string_view payload) {
    const auto frequency = yaesu::parse_frequency(payload);
    if (!frequency) {
        ESP_LOGW(TAG, "Invalid frequency in Yaesu FB: %.*s",
                 static_cast<int>(payload.length()), payload.data());
        return ESP_OK;
    }

    ESP_LOGV(TAG, "Yaesu FB frequency: %lu Hz", static_cast<unsigned long>(*frequency));
    vfo_b_frequency = *frequency;
    // Only drive an antenna change if VFO B is the active VFO.
    if (active_vfo == 1) {
        return handle_frequency_change(*frequency);
    }
    return ESP_OK;
}

esp_err_t YaesuCat::process_if_command(const std::string_view payload) {
    // See yaesu_frames.h for the full IF byte layout. Key point: unlike Kenwood,
    // there is NO TX flag in this frame - transmit state is delivered solely via the
    // 'TX' command, so IF never touches TX here.
    const auto frame = yaesu::parse_if(payload);
    if (!frame) {
        ESP_LOGW(TAG, "Yaesu IF invalid/too short (%d): %.*s",
                 static_cast<int>(payload.length()),
                 static_cast<int>(payload.length()), payload.data());
        return ESP_OK;
    }

    const std::string old_mode = current_mode;
    current_mode = yaesu::mode_string(frame->mode_char);
    if (current_mode != old_mode) {
        ESP_LOGV(TAG, "Yaesu mode (IF): %s -> %s", old_mode.c_str(), current_mode.c_str());
    }

    // IF reports the operating (VFO-A) frequency; store it and drive the antenna
    // decode. TX state is deliberately left untouched (no TX field in the frame).
    vfo_a_frequency = frame->frequency;
    ESP_LOGV(TAG, "Yaesu IF: freq=%lu Hz mode=%s",
             static_cast<unsigned long>(frame->frequency), current_mode.c_str());
    return handle_frequency_change(frame->frequency);
}

esp_err_t YaesuCat::process_md_command(const std::string_view payload) {
    // MD answer: P1 (const '0' = MAIN RX) + P2 (mode char). payload = "0" + mode.
    const auto mode_char = yaesu::parse_md_mode(payload);
    if (!mode_char) {
        ESP_LOGV(TAG, "Yaesu MD too short: %.*s",
                 static_cast<int>(payload.length()), payload.data());
        return ESP_OK;
    }

    const std::string old_mode = current_mode;
    current_mode = yaesu::mode_string(*mode_char);
    if (current_mode != old_mode) {
        ESP_LOGV(TAG, "Yaesu mode (MD): %s -> %s", old_mode.c_str(), current_mode.c_str());
    }
    return ESP_OK;
}

esp_err_t YaesuCat::process_tx_command(const std::string_view payload) {
    // Yaesu 'TX' answer digit: '0' = not transmitting; '1' = CAT-initiated TX;
    // '2' = radio/PTT-initiated TX. FAIL-SAFE mapping (see yaesu::tx_intent): only an
    // explicit '0' releases the interlock; '1'/'2' assert it; empty/unknown payloads
    // cause NO transition, so garbage can never silently drop an active TX (the
    // dangerous direction). The base set_transmitting() owns the actual safety path.
    switch (yaesu::tx_intent(payload)) {
        case yaesu::TxIntent::Rx:
            set_transmitting(false);
            break;
        case yaesu::TxIntent::Tx:
            set_transmitting(true);
            break;
        case yaesu::TxIntent::NoChange:
            ESP_LOGW(TAG, "Yaesu TX unparseable ('%.*s'); leaving TX state unchanged",
                     static_cast<int>(payload.length()), payload.data());
            break;
    }
    return ESP_OK;
}

esp_err_t YaesuCat::process_ai_command(const std::string_view payload) {
    if (payload.empty()) {
        ESP_LOGV(TAG, "Yaesu AI query received");
        return ESP_OK;
    }

    switch (payload[0]) {
        case '0':
            radio_provides_auto_updates_ = false;
            ESP_LOGI(TAG, "Yaesu AI OFF: relying on IF polling for freq/mode (TX detection needs AI1)");
            break;
        case '1':
            radio_provides_auto_updates_ = true;
            ESP_LOGI(TAG, "Yaesu AI ON: auto information active");
            break;
        default:
            ESP_LOGW(TAG, "Yaesu AI unexpected parameter '%c'", payload[0]);
            break;
    }
    return ESP_OK;
}
