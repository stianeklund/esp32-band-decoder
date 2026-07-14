#ifndef KENWOOD_CAT_H
#define KENWOOD_CAT_H

#include "cat_parser.h"
#include <charconv>
#include <string_view>
#include <unordered_map>

// Kenwood TS-590SG CAT protocol: ';'-terminated ASCII, 2-char command codes.
// Owns only the wire-format parsing (its command map + per-command parsers) and
// the Kenwood Auto-Information (AI) handshake; everything else — the UART engine,
// band/antenna handoff and the TX-safety chokepoint — lives in the CatParser base.
class KenwoodCat : public CatParser {
public:
    KenwoodCat();

protected:
    esp_err_t dispatch_one_command(std::string_view command_view) override;
    void poll_for_updates() override;

private:
    using Handler = esp_err_t (KenwoodCat::*)(std::string_view);

    esp_err_t process_fa_command(std::string_view command);
    esp_err_t process_fb_command(std::string_view command);
    esp_err_t process_if_command(std::string_view command);
    esp_err_t process_ap_command(std::string_view command);
    esp_err_t process_ai_command(std::string_view command_payload);
    esp_err_t process_tx_command(std::string_view payload);
    esp_err_t process_rx_command(std::string_view payload);
    esp_err_t process_ex_command(std::string_view payload); // Extended menu (transverter = menu 056)
    esp_err_t process_xo_command(std::string_view payload); // Transverter offset/direction

    static std::from_chars_result get_from_chars_result(std::string_view command, unsigned long& ports_val);

    // Kenwood AI (Auto Information) probe: ask the radio to push updates so we don't poll.
    esp_err_t probe_and_configure_ai_mode();

    std::unordered_map<uint16_t, Handler> command_handlers_;
    bool radio_provides_auto_updates_{false}; // True if Kenwood AI from the radio is ON
};

#endif // KENWOOD_CAT_H
