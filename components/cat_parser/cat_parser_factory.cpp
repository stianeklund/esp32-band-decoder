#include "cat_parser.h"
#include "kenwood_cat.h"
#include <memory>

// Protocol selection lives here so the base translation unit never needs to know
// about any concrete subclass.
//
// LOAD-BEARING INVARIANT: the parser object is created exactly once and lives for
// the whole session. RelayController (and potentially others) cache a CatParser&
// by identity, and that object owns the TX/PTT-interlock chokepoint — so it must
// never be destroyed or rebuilt at runtime. Changing the radio protocol therefore
// applies on reboot, not by swapping this object live.
//
// Step 1: only the Kenwood protocol exists, so the factory always builds KenwoodCat.
// Step 2 will read the persisted radio_protocol config here and pick the subclass
// (defaulting to Kenwood on unset/out-of-range).
CatParser &CatParser::instance() {
    static std::unique_ptr<CatParser> parser = std::make_unique<KenwoodCat>();
    return *parser;
}
