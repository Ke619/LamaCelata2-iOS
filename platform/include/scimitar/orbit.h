#pragma once
// Local replacement for Ubisoft's launcher library (ubiorbitapi_r2.dylib).
//
// The game reaches Orbit only through the 22 C functions its loader resolves
// with dlsym (mg::orbitclient::LibraryLoader). This implements them natively:
// an offline session that is always "logged in", and savegames stored as local
// files. Nothing from the original library is used. Results go back to the
// game through its listener objects (one virtual method, slot 0), called
// synchronously inside each request, which the game's wrappers allow (they
// register a listener before calling the library).
//
// Saves: one file per (product id, savegame id) under save_directory(),
// holding the save's name (UTF-32, as the game's wchar_t) and its bytes;
// written to a temporary file and renamed when the game closes the writer.
#include <cstdint>
#include <string>

namespace scimitar::orbit {

constexpr int kFunctionCount = 22;

// The functions in the order the game's loader resolves them.
extern const char* const kNames[kFunctionCount];

// Guest addresses of the 22 functions (host functions, registered on first use).
const uint32_t* functions();

// The session handle GetSession returns.
constexpr uint32_t kSession = 0x05e55100;

// Saves go here: $SCIMITAR_SAVE_DIR if set, else
// ~/Library/Application Support/scimitar-recomp/saves. Created when needed.
void set_save_directory(const std::string& dir);
std::string save_directory();

}  // namespace scimitar::orbit
