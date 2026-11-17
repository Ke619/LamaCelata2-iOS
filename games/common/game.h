// What differs between the games the common port code (games/common) serves:
// each game's directory defines its Info (games/<id>/game.cpp). Game code the
// port calls or replaces is found by symbol name in the loaded image
// (scimitar::guest_symbol), so the shells and overrides are the same for every
// game; what cannot be found by name (structure offsets) is here.
#pragma once
#include <cstdint>

namespace game {

struct Info {
    const char* id;           // "ac2": the executable's name, and the prefix of log lines
    const char* title;        // "Assassin's Creed II": window title and menus
    const char* app_env;      // environment variable naming the game's .app ("AC2_APP")
    const char* app_name;     // the .app's name ("Assassin's Creed 2.app"; on iOS in Documents)
    const char* default_app;  // where a Mac build finds it, relative to the working directory
    const char* saves;        // save folder under ~/Library/Application Support/scimitar-recomp
    // PadProxyMac: the selected slot's index; slots of 0x420 bytes follow
    // with each slot's Pad* at +8 and its type (2 for pads) at +12.
    uint32_t pad_proxy_selected;
    // Run once the image is loaded, before its static initializers (may be null).
    void (*start)() = nullptr;
    // The file the port runs, relative to the game's location (app_env,
    // default_app, the launcher's choice); alternatives separated by '|'. The
    // Windows ports run an .exe in the game's folder (or in a Mac release's
    // .app, Prince of Persia's Cider bundle).
    const char* game_file = "Contents/MacOS/scimitar_sf";
    // The launcher's settings the port supports, separated by spaces
    // (Launcher.swift, LauncherGame.supports); null: all of them.
    const char* launcher_settings = nullptr;
};

// The game this executable is built for (games/<id>/game.cpp).
extern const Info kInfo;

// A line on stderr prefixed with the game's id ("ac2: ...").
[[gnu::format(printf, 1, 2)]] void log(const char* fmt, ...);

// Keeps the videos 16:9 on screens of other shapes (intro.cpp).
void install_video_fit();

// The game's main, run on the guest main thread: loads the image, runs its
// static initializers and main (games/common/main.cpp). On the Mac this is
// main(); the iOS shell runs it on a thread of its own.
int game_main(int argc, char** argv);

}  // namespace game
