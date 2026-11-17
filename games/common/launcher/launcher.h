// The launcher (Launcher.swift): the game's settings before it starts. Its
// choices become the port's environment switches of this process.
#pragma once

extern "C" {

// The Mac: shows the launcher, unless its "show at start" is off and Option is
// not held, and applies its choices. 1: start the game, 2: start it (the window
// showed: main restarts the process first), 0: quit. default_app is
// where the build found the game (an absolute path); game_file and settings
// are game::Info's (settings "": all).
int scimitar_launcher_run(const char* id, const char* title, const char* app_env, const char* app_name,
                          const char* default_app, const char* game_file, const char* settings);

// iOS: the launcher's view controller, retained (take it with
// __bridge_transfer). on_play runs on the main thread once the choices are
// applied. default_app is the game's place in the app's Documents folder.
void* scimitar_launcher_make(const char* id, const char* title, const char* app_env, const char* app_name,
                             const char* default_app, void (*on_play)(void));
}
