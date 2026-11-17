// The F4 graphics menu (graphics_menu.cpp): settings beyond the game's options,
// changed while playing and in effect from the next frame, and set from the
// start by switches (the launcher's Graphics section) over the game's options.
// The shell draws text() over the game picture and passes the menu its keys
// while it is open.
#pragma once
#include <string>

#include "scimitar/runtime.h"

namespace graphics_menu {

enum class Key { Up, Down, Left, Right };

// Keeps the chosen settings (their switches', then the menu's) in effect: at
// every event pump, from the first.
void enforce(scimitar::Context& c);
// Reads the current values from the game (when the menu opens).
void refresh(scimitar::Context& c);
// Up/Down choose a setting, Left/Right change it (applied at once).
void key(scimitar::Context& c, Key k);
// The menu as lines of text.
std::string text();

}  // namespace graphics_menu
