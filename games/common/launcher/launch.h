// Starting a port as an app: the launcher before the game (launch.cpp), for
// the Mac ports (games/common/main.cpp) and the Windows ones
// (games/{ac1,acr,pop}/main.cpp) alike.
#pragma once
#include <string>

#include "game.h"

namespace game {

// Started as an app (from its .app with no arguments; not with
// SCIMITAR_NO_LAUNCHER or SCIMITAR_HIDDEN), shows the launcher, whose choices
// go to this process's environment. When its window showed, the game starts
// in a fresh image of the process (execv; does not return). false: the user
// quit. default_app: where the build found the game (an absolute path).
bool run_launcher(int argc, char** argv, const char* default_app);

// The file kInfo.game_file names under `location` (the game's folder or
// .app), or `location` itself when it is a file; "" when there is none.
std::string find_game_file(const std::string& location);

}  // namespace game
