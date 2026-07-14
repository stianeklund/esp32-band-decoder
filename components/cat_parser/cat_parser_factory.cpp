#include "cat_parser.h"
#include "kenwood_cat.h"
#include "yaesu_cat.h"
#include "config_manager.h"
#include "antenna_switch.h"
#include "esp_log.h"
#include <memory>

// Protocol selection lives here so the base translation unit never needs to know
// about any concrete subclass.
//
// LOAD-BEARING INVARIANT: the parser object is created exactly once and lives for
// the whole session. RelayController (and potentially others) cache a CatParser&
// by identity, and that object owns the TX/PTT-interlock chokepoint - so it must
// never be destroyed or rebuilt at runtime. Changing the radio protocol therefore
// applies on reboot, not by swapping this object live (the web save handler triggers
// a restart when radio_protocol changes).
//
// The subclass is chosen from the persisted radio_protocol config the first time
// instance() is called (RelayController's ctor, after NVS/config load). We default
// to Kenwood on any unexpected value; ConfigManager value-initializes its config in
// its constructor, so get_config() is null-safe even if called before config load.
CatParser &CatParser::instance() {
    static std::unique_ptr<CatParser> parser = []() -> std::unique_ptr<CatParser> {
        radio_protocol_t proto = RADIO_PROTOCOL_KENWOOD;
        {
            const auto cfg = std::make_unique<antenna_switch_config_t>();
            ConfigManager::instance().get_config(*cfg);
            proto = cfg->radio_protocol;
        }

        switch (proto) {
            case RADIO_PROTOCOL_YAESU:
                ESP_LOGI("CAT_FACTORY", "CAT protocol: Yaesu");
                return std::make_unique<YaesuCat>();
            case RADIO_PROTOCOL_KENWOOD:
            default:
                ESP_LOGI("CAT_FACTORY", "CAT protocol: Kenwood");
                return std::make_unique<KenwoodCat>();
        }
    }();
    return *parser;
}
