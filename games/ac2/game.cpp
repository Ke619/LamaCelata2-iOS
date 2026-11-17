// Assassin's Creed II (Mac, 1.0.1): what the common port code needs to know
// about it (games/common/game.h).
#include "game.h"

namespace ac2 {
void install_pc_data_support();  // pc_data.cpp
void install_language_menu();    // language.cpp
void install_color_filter();     // color_filter.cpp

void start() {
    install_pc_data_support();
    install_language_menu();
    install_color_filter();
}
}

const game::Info game::kInfo = {
    .id = "ac2",
    .title = "Assassin's Creed II",
    .app_env = "AC2_APP",
    .app_name = "Assassin's Creed 2.app",
    .default_app = "../steam_depot/33362_5/Assassin's Creed 2.app",
    .saves = "saves",
    .pad_proxy_selected = 0x54c,
    .start = ac2::start,
};
