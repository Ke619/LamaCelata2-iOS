// Assassin's Creed Brotherhood (Mac): what the common port code needs to know
// about it (games/common/game.h).
#include "game.h"

namespace acb {
void install_language_menu();  // language.cpp
}

const game::Info game::kInfo = {
    .id = "acb",
    .title = "Assassin's Creed Brotherhood",
    .app_env = "ACB_APP",
    .app_name = "Assassin's Creed Brotherhood.app",
    .default_app = "../steam_depot/48193_15/Assassin's Creed Brotherhood.app",
    .saves = "acb/saves",
    .pad_proxy_selected = 0x580,
    .start = acb::install_language_menu,
};
