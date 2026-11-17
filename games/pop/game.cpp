// Prince of Persia's hand-written replacements (docs/POP_WINDOWS.md): the
// language setters. The engine keeps the text language and the voice language
// in two globals, set at start from the registry or the system's locale and
// then checked against the languages the data supports (a settings object in
// DataPC.forge: a list and a default). The archives at hand are the Russian
// edition's, whose list is Russian alone, so every other choice becomes
// Russian. SCIMITAR_POP_TEXT and SCIMITAR_POP_VOICES (a language name:
// English, French, Spanish, German, Italian, Russian...) choose instead:
// voices need DataPC_StreamedSounds<first three letters>.forge, text needs the
// language in the data.
#include <cstdlib>
#include <cstring>
#include <strings.h>

#include "game.h"
#include "scimitar/runtime.h"

// What the port's entry point (main.cpp) and the app's launcher need to know
// about the game (games/common/game.h; the launcher's settings as AC1's,
// games/ac1/game.cpp). It is the Windows executable in the Mac release's
// Cider bundle, or in a Windows copy's folder (GOG's).
const game::Info game::kInfo = {
    .id = "pop",
    .title = "Prince of Persia",
    .app_env = "POP_EXE",
    .app_name = "Prince of Persia.app",
    .default_app = "../steam_depot/Prince of Persia.app",
    .saves = "pop",
    .pad_proxy_selected = 0,
    .game_file = "Contents/Resources/transgaming/c_drive/Program Files/Ubisoft/Prince of Persia/Prince of Persia.exe"
                 "|Prince of Persia.exe",
    .launcher_settings = "full-screen stats aniso",
};

namespace scimitar::overrides {
namespace {

constexpr uint32_t kTextLanguage = 0xe4746c, kVoiceLanguage = 0xe4758c;

// The engine's language numbers (its table at 0xd3d348).
uint32_t language_of(const char* env) {
    static const char* const names[] = {"None",     "English", "French", "Spanish", "Polish",
                                        "German",   "Chinese", "Hungarian", "Italian", "Japanese",
                                        "Czech",    "Korean",  "Russian", "Dutch"};
    const char* v = std::getenv(env);
    if (!v) return 0;
    for (uint32_t k = 1; k < sizeof names / sizeof *names; k++)
        if (strcasecmp(v, names[k]) == 0 || (std::strlen(v) == 3 && strncasecmp(v, names[k], 3) == 0)) return k;
    return 0;
}

void set(Context& c, uint32_t global, uint32_t chosen) {
    const uint32_t v = chosen ? chosen : arg32(c, 0);
    c.mem->write<uint32_t>(global, v);
    shim_return(c, v);
}

}  // namespace

// void SetTextLanguage(int) at 0x51f820, void SetVoiceLanguage(int) at 0x51f840:
// `mov 4(%esp), %eax; mov %eax, global; ret`.
void ov_sub_0051f820(Context& c) {
    static const uint32_t text = language_of("SCIMITAR_POP_TEXT");
    set(c, kTextLanguage, text);
}
void ov_sub_0051f840(Context& c) {
    static const uint32_t voices = language_of("SCIMITAR_POP_VOICES");
    set(c, kVoiceLanguage, voices);
}

}  // namespace scimitar::overrides
