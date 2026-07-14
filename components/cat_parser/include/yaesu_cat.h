#ifndef YAESU_CAT_H
#define YAESU_CAT_H

#include "cat_parser.h"
#include <string_view>
#include <unordered_map>

// Modern Yaesu CAT (FT-891 / FT-991A / FTDX-10/101): same ';'-terminated ASCII,
// 2-char command framing as Kenwood, but with different field widths, a different
// mode enum, and - critically - a decoupled TX indicator. Unlike Kenwood, the Yaesu
// IF frame carries NO transmit flag; TX state arrives only via the TX command
// (TX0/1/2), which the radio auto-emits when Auto Information (AI1) is enabled.
//
// This subclass owns only the Yaesu wire parsing (its command map + per-command
// parsers) and the AI1 handshake. The UART engine, band/antenna handoff and the
// TX-safety chokepoint (set_transmitting) all live in the CatParser base.
class YaesuCat : public CatParser {
public:
    YaesuCat();

protected:
    esp_err_t dispatch_one_command(std::string_view command_view) override;
    void poll_for_updates() override;

private:
    using Handler = esp_err_t (YaesuCat::*)(std::string_view);

    esp_err_t process_fa_command(std::string_view payload); // VFO-A frequency (9-digit)
    esp_err_t process_fb_command(std::string_view payload); // VFO-B frequency (9-digit)
    esp_err_t process_if_command(std::string_view payload); // Status frame (freq + mode; NO TX flag)
    esp_err_t process_md_command(std::string_view payload); // Operating mode
    esp_err_t process_tx_command(std::string_view payload); // TX0/1/2 -> set_transmitting
    esp_err_t process_ai_command(std::string_view payload); // Auto-information state (note only)

    // Map a Yaesu mode character (IF P6 / MD P2) to a display-mode string.
    static const char* mode_string(char mode_char);

    std::unordered_map<uint16_t, Handler> command_handlers_;
    bool radio_provides_auto_updates_{false}; // True once the radio confirms AI1
};

#endif // YAESU_CAT_H
