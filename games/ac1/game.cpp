// Assassin's Creed (2007, Windows): what the port's entry point (main.cpp) and
// the app's launcher need to know about it (games/common/game.h).
#include "game.h"

// The Windows ports' settings in the launcher: Mode (they start in a window:
// SCIMITAR_FULL_SCREEN), the frame-rate overlay, and anisotropic filtering
// (the Direct3D layer's). Not the Mac ports' aspect, intro, language, button
// prompt, game option or F4 graphics switches, nor the Metal shader
// precompile (Direct3D 9 compiles its shaders as they come).
const game::Info game::kInfo = {
    .id = "ac1",
    .title = "Assassin's Creed",
    .app_env = "AC1_EXE",
    .app_name = "Assassin's Creed",
    .default_app = "../windows-releases/Ubisoft Launcher/Assassin's Creed 1",
    .saves = "ac1",
    .pad_proxy_selected = 0,
    .game_file = "AssassinsCreed_Dx9.exe",
    .launcher_settings = "full-screen stats aniso",
};
