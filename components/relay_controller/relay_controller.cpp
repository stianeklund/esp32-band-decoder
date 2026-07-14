#include "relay_controller.h"
#include "antenna_switch.h"
#include "websocket_server.h"
#include "config_manager.h"
#include "my_mqtt_client.h"
#include "cat_parser.h"
#include "esp_log.h"
#include <memory>
#include "config_cache.h"

static constexpr const char* TAG = "RELAY_CONTROLLER";

RelayController::RelayController()
    : currently_selected_relay_(0),
      cat_parser_(CatParser::instance()) {

    esp_log_level_set(TAG, ESP_LOG_WARN);
}

RelayController::~RelayController() = default;

esp_err_t RelayController::init() {
    ESP_LOGI(TAG, "Initializing relay controller");

    esp_err_t ret = kc868_hw_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize KC868 hardware: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = turn_off_all_relays();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize relay states: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Relay controller initialized successfully");
    return ESP_OK;
}

esp_err_t RelayController::turn_off_all_relays() {
    ESP_LOGD(TAG, "Turning off all relays");

    std::lock_guard lock(relay_mutex_);

    // Block bulk changes while any radio transmits (CAT or MQTT).
    if (is_transmitting()) {
        ESP_LOGW(TAG, "Cannot change relays while transmitting");
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t ret = kc868_hw_set_all_outputs(0);
    if (ret == ESP_OK) {
        currently_selected_relay_ = 0;
    }
    return ret;
}

esp_err_t RelayController::set_relay(const int relay_id, const bool state) {
    if (relay_id < 1 || relay_id > NUM_RELAYS) {
        ESP_LOGE(TAG, "Invalid relay ID: %d", relay_id);
        return ESP_ERR_INVALID_ARG;
    }
    std::lock_guard lock(relay_mutex_);

    // Selective transmission check: only block changes to the transmitting radio's own relays.
    if (is_transmitting()) {
        const bool is_radio_a_relay = (relay_id >= 1 && relay_id <= RELAYS_PER_RADIO);
        const bool is_radio_b_relay = (relay_id > RELAYS_PER_RADIO && relay_id <= NUM_RELAYS);

        const bool is_cat_tx = cat_parser_.is_transmitting(); // Assume this is Radio A
        const bool is_mqtt_tx = MQTTClient::instance().is_transmitting(); // Could be either radio

        if ((is_cat_tx && is_radio_a_relay) || (is_mqtt_tx && (is_radio_a_relay || is_radio_b_relay))) {
            ESP_LOGW(TAG, "Cannot change relay %d while that radio is transmitting", relay_id);
            return ESP_ERR_INVALID_STATE;
        }
    }

    // Single-bit change derived from the committed physical state (the HW cache is
    // the single source of truth). Compute the desired 16-bit mask, write it, and
    // only update software state after a confirmed successful write.
    const uint8_t hw_relay = relay_id - 1; // 0-based
    const uint16_t committed = kc868_hw_get_all_outputs(); // logical (1 = ON)
    const uint16_t desired = state
        ? static_cast<uint16_t>(committed | (1u << hw_relay))
        : static_cast<uint16_t>(committed & ~(1u << hw_relay));

    const esp_err_t ret = kc868_hw_set_all_outputs(desired);
    if (ret == ESP_OK) {
        if (state) {
            currently_selected_relay_ = relay_id;
        } else if (currently_selected_relay_ == relay_id) {
            // If the relay being turned OFF was the currently_selected_relay_, clear it.
            currently_selected_relay_ = 0;
        }

        // Broadcast relay state change to WebSocket clients
        if (websocket_server_is_running()) {
            WebSocketServer::instance().broadcast_relay_state_change(relay_id, state);
        }
    }
    // On failure the HW cache still reflects physical reality (per-chip, see
    // kc868_hw_set_all_outputs); leave software state alone and let the caller retry.
    return ret;
}

esp_err_t RelayController::swap_antenna_relays(const int off_relay, const int on_relay) {
    // The relay to switch ON is required: this call always leaves an antenna live.
    if (on_relay < 1 || on_relay > NUM_RELAYS) {
        ESP_LOGE(TAG, "swap_antenna_relays: invalid on_relay %d", on_relay);
        return ESP_ERR_INVALID_ARG;
    }
    // off_relay may be 0, meaning "no relay is currently on, just switch one on".
    if (off_relay != 0 && (off_relay < 1 || off_relay > NUM_RELAYS)) {
        ESP_LOGE(TAG, "swap_antenna_relays: invalid off_relay %d", off_relay);
        return ESP_ERR_INVALID_ARG;
    }

    std::lock_guard lock(relay_mutex_);

    // Deliberately no transmit interlock here: see the header. This is the key-up /
    // key-down antenna move itself, so blocking it during transmit is exactly the
    // bug we are fixing.

    // Start from what is physically on right now, clear the old relay's bit, set
    // the new relay's bit, and write the whole thing at once. Because it is one
    // write, the old and new relays change together -- the antenna is never left
    // on nothing in between.
    const uint16_t current_outputs = kc868_hw_get_all_outputs(); // 1 = relay ON
    uint16_t new_outputs = current_outputs;
    if (off_relay != 0) {
        new_outputs = static_cast<uint16_t>(new_outputs & ~(1u << (off_relay - 1)));
    }
    new_outputs = static_cast<uint16_t>(new_outputs | (1u << (on_relay - 1)));

    if (new_outputs == current_outputs) {
        // Already exactly where we want to be (new relay on, old relay off).
        currently_selected_relay_ = on_relay;
        return ESP_OK;
    }

    if (const esp_err_t ret = kc868_hw_set_all_outputs(new_outputs); ret != ESP_OK) {
        // The write failed. Put the relays back the way they were so a failed
        // switch never leaves the antenna disconnected. The hardware cache still
        // reflects what is physically set, so re-writing the old value restores it.
        ESP_LOGE(TAG, "swap_antenna_relays: hardware write failed (%s); restoring relay %d",
                 esp_err_to_name(ret), off_relay);
        if (const esp_err_t rb = kc868_hw_set_all_outputs(current_outputs); rb != ESP_OK) {
            ESP_LOGE(TAG, "swap_antenna_relays: restore ALSO failed (%s)", esp_err_to_name(rb));
        }
        return ret;
    }

    currently_selected_relay_ = on_relay;

    // Tell WebSocket clients about both relays that changed.
    if (websocket_server_is_running()) {
        if (off_relay != 0) {
            WebSocketServer::instance().broadcast_relay_state_change(off_relay, false);
        }
        WebSocketServer::instance().broadcast_relay_state_change(on_relay, true);
    }
    return ESP_OK;
}

bool RelayController::get_relay_state(const int relay_id) const {
    if (relay_id < 1 || relay_id > NUM_RELAYS) {
        ESP_LOGE(TAG, "Invalid relay ID: %d", relay_id);
        return false;
    }

    bool state;
    if (kc868_hw_get_output_state(relay_id - 1, &state) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get relay state");
        return false;
    }
    return state;
}

uint16_t RelayController::get_relay_states() const {
    // kc868_hw_get_all_outputs() returns logical state (1 = ON, 0 = OFF).
    // However, UI layer expects active-low format (0 = ON, 1 = OFF) for consistency
    // with the active-low checking pattern: if (!((states >> i) & 1))
    // So we intentionally invert to provide active-low format to callers.
    const uint16_t logical_states = kc868_hw_get_all_outputs();
    const uint16_t active_low_states = ~logical_states & 0xFFFF;

    if (active_low_states != 0xFFFF) {
        ESP_LOGD(TAG, "Relay states (active-low): 0x%04X", active_low_states);
    }
    return active_low_states;
}

int RelayController::get_last_selected_relay_for_band(const int band_number) const {
    const auto &mapA = last_selected_relay_for_band_[0];
    if (const auto it = mapA.find(band_number); it != mapA.end()) {
        return it->second;
    }
    return 0;
}

bool RelayController::is_correct_relay_set(int band_number) const {
    int last_selected_relay = get_last_selected_relay_for_band(band_number);
    return currently_selected_relay_ == last_selected_relay && last_selected_relay != 0;
}

bool RelayController::is_transmitting() const {
    // "Are we transmitting right now?" for the relay interlock. We check all three
    // TX sources: CAT (radio told us over the serial link), MQTT (a networked
    // radio), and the hardware PTT line. Including the PTT line matters because it
    // is the fastest and most reliable signal, and on some radios (e.g. Yaesu) CAT
    // may not report TX at all -- without it the interlock would miss a live key-up.
    // The key-up antenna move itself uses swap_antenna_relays(), which skips this
    // check, so this only ever blocks OTHER callers (web UI, MQTT, band changes).
    return cat_parser_.is_transmitting() || MQTTClient::instance().is_transmitting() ||
           AntennaSwitch::instance().hw_ptt_tx_active();
}

esp_err_t RelayController::execute_relay_change(const int relay_id, const int band_number, const RadioID radio, const bool state) {
    std::lock_guard lock(relay_mutex_);
    const auto config_snapshot = std::make_unique<antenna_switch_config_t>();
    if (!config_snapshot) {
        ESP_LOGE(TAG, "Failed to allocate config snapshot in execute_relay_change");
        return ESP_ERR_NO_MEM;
    }
    get_cached_config(*config_snapshot);
    const auto &cfg = *config_snapshot;

    // More selective transmission check: only block changes to the transmitting radio's own relays
    if (is_transmitting()) {
        // Determine which radio this relay belongs to
        const bool is_radio_a_relay = (relay_id >= 1 && relay_id <= RELAYS_PER_RADIO);
        const bool is_radio_b_relay = (relay_id > RELAYS_PER_RADIO && relay_id <= NUM_RELAYS);

        // Check if we're trying to change a relay for the transmitting radio
        const bool is_cat_tx = cat_parser_.is_transmitting(); // Assume this is Radio A
        const bool is_mqtt_tx = MQTTClient::instance().is_transmitting(); // Could be either radio

        if ((is_cat_tx && is_radio_a_relay) || (is_mqtt_tx && (is_radio_a_relay || is_radio_b_relay))) {
            ESP_LOGW(TAG, "Cannot change relay %d while that radio is transmitting", relay_id);
            return ESP_ERR_INVALID_STATE;
        }

        // Allow interlock operations on non-transmitting radio relays
        ESP_LOGD(TAG, "Allowing relay %d change for interlock while other radio transmits", relay_id);
    }

    const int current_radio_idx = static_cast<int>(radio);
    const int other_radio_idx = (current_radio_idx == 0) ? 1 : 0;
    const int port_index = (relay_id - 1) % RELAYS_PER_RADIO;

    // Start from the committed physical state (the HW cache is the single source of
    // truth). Splitting it into per-radio nibbles means a control output dropped by
    // the interlock (via set_relay, which doesn't go through this path) is never
    // re-asserted here: bits 0-7 = Radio A, bits 8-15 = Radio B.
    const uint16_t committed = kc868_hw_get_all_outputs();
    uint8_t radio_mask[2] = {
        static_cast<uint8_t>(committed & 0xFF),
        static_cast<uint8_t>((committed >> RELAYS_PER_RADIO) & 0xFF),
    };

    if (state) { // Turning a relay ON
        switch (cfg.radio_operation_mode) {
            case RADIO_OP_MODE_SINGLE_A:
                if (radio == RadioID::B) {
                    ESP_LOGW(TAG, "Radio B operations disabled (Single A mode). Cannot turn ON relay %d for Radio B.", relay_id);
                    // Ignore the request for B; the mask update below keeps B off.
                } else { // Radio A
                    radio_mask[0] = (1u << port_index); // Intra-radio exclusivity: only this port on
                }
                radio_mask[1] = 0; // Radio B is always off in Single A mode
                break;

            case RADIO_OP_MODE_ALTERNATING_AB:
                // Clear the other radio's mask first, then set the current radio's relay
                radio_mask[other_radio_idx] = 0;
                radio_mask[current_radio_idx] = (1u << port_index);
                break;

            case RADIO_OP_MODE_CONCURRENT_AB:
                // Check for same antenna port conflict with the other radio
                if ((radio_mask[other_radio_idx] & (1u << port_index)) != 0) { // Other radio is using the same port
                    if (cfg.interlock_auto_resolves_conflict) {
                        ESP_LOGW(TAG, "Interlock (Concurrent Mode, Auto-Resolve): Port %d conflict. Clearing other radio's relay.", port_index + 1);
                        radio_mask[other_radio_idx] &= ~(1u << port_index); // Clear the conflicting bit for the other radio
                    } else {
                        ESP_LOGE(TAG, "CRITICAL (Concurrent Mode, Block): Attempt to use same antenna port %d. Operation blocked.", port_index + 1);
                        return ESP_ERR_INVALID_STATE; // Block operation
                    }
                }
                // Set the current radio's relay (intra-radio exclusivity)
                radio_mask[current_radio_idx] = (1u << port_index);
                break;
        }
    } else { // Turning a relay OFF
        radio_mask[current_radio_idx] &= ~(1u << port_index);
        // If in SINGLE_A mode and Radio A is turned off, Radio B mask should remain 0.
        if (cfg.radio_operation_mode == RADIO_OP_MODE_SINGLE_A) {
            radio_mask[1] = 0; // Explicitly ensure Radio B mask is zero
        }
    }

    // Combine masks and push to hardware.
    // radio_mask[0] is for relays 1-8 (physical bits 0-7)
    // radio_mask[1] is for relays 9-16 (physical bits 8-15)
    const uint16_t desired = radio_mask[0] | (static_cast<uint16_t>(radio_mask[1]) << RELAYS_PER_RADIO);

    const esp_err_t ret = kc868_hw_set_all_outputs(desired);
    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "Successfully set desired mask: 0x%04X (Radio A: 0x%02X, Radio B: 0x%02X)", desired, radio_mask[0], radio_mask[1]);
        if (state) { // Only update last selected if turning ON
            last_selected_relay_for_band_[current_radio_idx][band_number] = relay_id;
        }
        // Update the generic currently_selected_relay_ if Radio A is involved, primarily for single-radio context or backward compatibility views
        if (radio == RadioID::A) {
            currently_selected_relay_ = state ? relay_id : 0; // if turning off, and it was this relay, set to 0
        }

        // Broadcast relay state change to WebSocket clients
        if (websocket_server_is_running()) {
            WebSocketServer::instance().broadcast_relay_state_change(relay_id, state);
        }
    } else {
        // Committed the desired mask failed; the HW cache still matches physical
        // reality (per-chip), so the next selection re-derives from it. Leave
        // software state untouched and let the caller retry.
        ESP_LOGE(TAG, "Failed to set all outputs with mask 0x%04X. Error: %s", desired, esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t RelayController::set_relay_for_antenna(int relay_id, int band_number, const RadioID radio, const bool state) {
    if (relay_id < 1 || relay_id > NUM_RELAYS) {
        ESP_LOGE(TAG, "Invalid relay ID: %d", relay_id);
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGD(TAG, "Setting relay %d for Radio %d to state %s (band %d)", relay_id, static_cast<int>(radio), state ? "ON" : "OFF", band_number);

    return execute_relay_change(relay_id, band_number, radio, state);
}
