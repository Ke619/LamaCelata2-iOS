// Assassin's Creed Revelations (Windows): what the port's entry point
// (main.cpp) and the app's launcher need to know about it
// (games/common/game.h; the launcher's settings as AC1's, games/ac1/game.cpp).
#include "game.h"

const game::Info game::kInfo = {
    .id = "acr",
    .title = "Assassin's Creed Revelations",
    .app_env = "ACR_EXE",
    .app_name = "Assassin's Creed Revelations",
    .default_app = "../windows-releases/Steam/Assassin's Creed Revelations",
    .saves = "acr/saves",
    .pad_proxy_selected = 0,
    .game_file = "ACRSP.exe",
    .launcher_settings = "full-screen stats aniso",
};
