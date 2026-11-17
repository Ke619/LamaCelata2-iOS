// Brotherhood's languages: chosen outside the game (the launcher), and every
// installed one offered in its options.
//
// Brotherhood takes its first text language from the system's preferred
// languages (SelectInstalledLanguage; SCIMITAR_TEXT_LANGUAGE answers
// NSLocale.preferredLanguages, platform/src/objc.cpp), and then the text,
// subtitle and voice languages from its options (SAVES/OPTIONS; the ini's
// [Language] only records them), each through LocalizationUtils::ValidateAndSet*,
// which keeps a language the game supports and otherwise falls back.
// The replacements set SCIMITAR_TEXT_LANGUAGE (text and subtitles) and
// SCIMITAR_VOICE_LANGUAGE (voices) instead of the options' choice when set
// (short names: eng, fre, ger, ita, spa, rus...), and otherwise do what the
// originals do. A choice made in the options menu (OptionsMenu::SetVariable,
// wrapped) wins for the rest of the run.
//
// Which languages it supports comes from localization.lang
// (LocalizationUtils::ReadSupportedLanguages): "LANG", the default text
// language (a byte), the text and voice languages (big-endian bit masks) and
// the default voice language (a byte, optional). The original then narrows the
// text languages to the one in use, so the Subtitle Language option offers only
// that one; the replacement keeps the file's list and adds every voice
// language whose SoundData/Mac/sounds_<code>.pck is installed (the file leaves
// out Russian, which some copies have).
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

using namespace std::literals;

#include "game.h"
#include "scimitar/guest_heap.h"
#include "scimitar/image_loader.h"
#include "scimitar/imports.h"
#include "scimitar/runtime.h"

namespace scimitar::overrides {
namespace {

// The Language a switch names, 0 when unset or unknown.
uint32_t forced(Context& c, const char* env) {
    static const uint32_t to_language = guest_symbol("__ZN8scimitar26ShortLanguageName2LanguageEPKc");
    const char* v = std::getenv(env);
    if (!v || !*v || std::strlen(v) >= 0x100) return 0;
    const uint32_t buf = heap::alloc(*c.mem, 0x100);
    std::memcpy(c.mem->ptr(buf), v, std::strlen(v) + 1);
    const uint32_t l = call_guest(c, to_language, {buf});
    if (!l) game::log("unknown language %s in %s\n", v, env);
    return l;
}

bool g_from_menu;  // in OptionsMenu::SetVariable: the player's choice

uint32_t text_language(Context& c) {
    static const uint32_t l = forced(c, "SCIMITAR_TEXT_LANGUAGE");
    return g_from_menu ? 0 : l;
}
uint32_t voice_language(Context& c) {
    static const uint32_t l = forced(c, "SCIMITAR_VOICE_LANGUAGE");
    return g_from_menu ? 0 : l;
}

// What the originals choose for `lang`: itself if supported; English for a
// language that overrides English (and for Polish voices where the default is
// Russian), if supported; the default; English.
uint32_t validate(Context& c, uint32_t lang, bool sound) {
    static const uint32_t valid_text = guest_symbol("__ZN8scimitar17LocalizationUtils19IsValidTextLanguageENS_8LanguageE"),
                          valid_sound = guest_symbol("__ZN8scimitar17LocalizationUtils20IsValidSoundLanguageENS_8LanguageE"),
                          overrides_english = guest_symbol(
                              "__ZN8scimitar17LocalizationUtils30IsLanguageThatOverridesEnglishENS_8LanguageE"),
                          // The default language, the word after the supported text languages.
                          fallback = guest_symbol("__ZN8scimitar17LocalizationUtils25ms_SupportedTextLanguagesE") + 4;
    const uint32_t valid_fn = sound ? valid_sound : valid_text;
    const auto valid = [&](uint32_t l) { return (call_guest(c, valid_fn, {l}) & 0xff) != 0; };
    if (valid(lang)) return lang;
    const uint32_t d = c.mem->read<uint32_t>(fallback);
    const bool to_english = (sound && lang == 4 && d == 12) || (call_guest(c, overrides_english, {lang}) & 0xff);
    if (to_english && valid(1)) return 1;
    return valid(d) ? d : 1;
}

void set(Context& c, const char* setter, uint32_t lang) {
    call_guest(c, guest_symbol(setter), {lang});
    shim_return(c, 0);
}

// A forced text language the game has no text for: the game's own choice.
uint32_t supported_text(Context& c, uint32_t l) {
    static const uint32_t valid_text = guest_symbol("__ZN8scimitar17LocalizationUtils19IsValidTextLanguageENS_8LanguageE");
    static const uint32_t mask = guest_symbol("__ZN8scimitar17LocalizationUtils25ms_SupportedTextLanguagesE");
    if (!l || !c.mem->read<uint32_t>(mask) || (call_guest(c, valid_text, {l}) & 0xff)) return l;  // (not read yet)
    static bool told;
    if (!told) game::log("language: no text in language %u here, the game's choice instead\n", l);
    told = true;
    return 0;
}

constexpr uint32_t kRussian = 12;

// Whether DataMAC.forge's index (its first megabyte) names each of names.
bool forge_has(std::initializer_list<std::string_view> names) {
    std::string index(1u << 20, '\0');
    FILE* f = std::fopen(host_path("DataMAC.forge").c_str(), "rb");
    if (!f) return false;
    index.resize(std::fread(index.data(), 1, index.size(), f));
    std::fclose(f);
    for (std::string_view name : names)
        if (index.find(name) == std::string::npos) return false;
    return true;
}

GuestFn g_set_variable;  // the original OptionsMenu::SetVariable

void set_variable(Context& c) {
    g_from_menu = true;
    g_set_variable(c);
    g_from_menu = false;
}

}  // namespace

// void scimitar::LocalizationUtils::ReadSupportedLanguages()
void ov__ZN8scimitar17LocalizationUtils22ReadSupportedLanguagesEv(Context& c) {
    static const uint32_t text_mask = guest_symbol("__ZN8scimitar17LocalizationUtils25ms_SupportedTextLanguagesE"),
                          sound_mask = guest_symbol("__ZN8scimitar17LocalizationUtils26ms_SupportedSoundLanguagesE"),
                          text_default = text_mask + 4, sound_default = text_mask + 12,
                          short_name = guest_symbol("__ZN8scimitar20GetShortLanguageNameENS_8LanguageE");
    constexpr uint32_t kLast = 18;  // the languages the game names, 1 to 18
    uint8_t b[14] = {};
    size_t n = 0;
    if (FILE* f = std::fopen(host_path("localization.lang").c_str(), "rb")) {
        n = std::fread(b, 1, sizeof b, f);
        std::fclose(f);
    }
    const auto be32 = [&](int k) { return uint32_t(b[k]) << 24 | uint32_t(b[k + 1]) << 16 | uint32_t(b[k + 2]) << 8 | b[k + 3]; };
    uint32_t texts, sounds;
    if (n >= 13 && std::memcmp(b, "LANG", 4) == 0) {
        texts = be32(5);
        sounds = be32(9);
        c.mem->write<uint32_t>(text_default, b[4]);
        c.mem->write<uint32_t>(sound_default, n >= 14 ? b[13] : b[4]);
    } else {
        // As the original without the file: every language, English first.
        texts = sounds = ((1u << (kLast + 1)) - 1) & ~1u;
        c.mem->write<uint32_t>(text_default, 1);
        c.mem->write<uint32_t>(sound_default, 1);
        for (const char* s : {"__ZN8scimitar22SetCurrentTextLanguageENS_8LanguageE",
                              "__ZN8scimitar23SetCurrentSoundLanguageENS_8LanguageE",
                              "__ZN8scimitar27SetCurrentSubtitlesLanguageENS_8LanguageE"})
            call_guest(c, guest_symbol(s), {1});
    }
    // Russian text: localization.lang leaves it out, but DataMAC.forge carries
    // its packages (LocalizationPackage_Russian, _Russian_Subtitles; their
    // names are in the forge's index, near its start), and the Cyrillic font
    // (UbiGame_Text_TMP3_RussianCharacterSet).
    if (!(texts & 1u << kRussian) && forge_has({"LocalizationPackage_Russian_Subtitles"sv, "LocalizationPackage_Russian\0"sv}))
        texts |= 1u << kRussian;
    for (uint32_t l = 1; l <= kLast; l++) {
        const uint32_t name = call_guest(c, short_name, {l});
        if (!name) continue;
        const std::string pck = std::string("SoundData/Mac/sounds_") + reinterpret_cast<const char*>(c.mem->ptr(name)) + ".pck";
        if (::access(host_path(pck.c_str()).c_str(), R_OK) == 0) sounds |= 1u << l;
    }
    c.mem->write<uint32_t>(text_mask, texts);
    c.mem->write<uint32_t>(sound_mask, sounds);
    game::log("languages: text 0x%x, voices 0x%x\n", texts, sounds);

    const auto get = [&](const char* s) { return call_guest(c, guest_symbol(s), {}); };
    call_guest(c, guest_symbol("__ZN8scimitar17LocalizationUtils26ValidateAndSetTextLanguageENS_8LanguageE"),
               {get("__ZN8scimitar22GetCurrentTextLanguageEv")});
    call_guest(c, guest_symbol("__ZN8scimitar17LocalizationUtils27ValidateAndSetSoundLanguageENS_8LanguageE"),
               {get("__ZN8scimitar23GetCurrentSoundLanguageEv")});
    call_guest(c, guest_symbol("__ZN8scimitar17LocalizationUtils30ValidateAndSetSubtitleLanguageENS_8LanguageE"),
               {get("__ZN8scimitar27GetCurrentSubtitlesLanguageEv")});
    call_guest(c, guest_symbol("__ZN8scimitar17LocalizationUtils16FindFontLanguageEv"), {});
    shim_return(c, 0);
}

// void scimitar::LocalizationUtils::ValidateAndSetTextLanguage(Language)
void ov__ZN8scimitar17LocalizationUtils26ValidateAndSetTextLanguageENS_8LanguageE(Context& c) {
    const uint32_t l = supported_text(c, text_language(c));
    set(c, "__ZN8scimitar22SetCurrentTextLanguageENS_8LanguageE", l ? l : validate(c, arg32(c, 0), false));
}

// void scimitar::LocalizationUtils::ValidateAndSetSubtitleLanguage(Language)
// The options keep the subtitle language (PlayerOptionsSaveData +0x10), but
// loading them (PlayerOptionsSaveData::LoadData) sets the text's language
// instead, through PlayerOptions::SetSubtitlesLanguage_Callback, which calls
// this keeping a frame, with LoadData's `this` (in ebx) pushed below it: there
// the saved language is used, if the game has text in it.
void ov__ZN8scimitar17LocalizationUtils30ValidateAndSetSubtitleLanguageENS_8LanguageE(Context& c) {
    static const uint32_t from_callback =
                              guest_symbol("__ZN8scimitar3ac213PlayerOptions29SetSubtitlesLanguage_CallbackENS_8LanguageE") + 0x12,
                          from_load = guest_symbol("__ZN8scimitar3ac221PlayerOptionsSaveData8LoadDataEv") + 0x4c;
    const uint32_t l = supported_text(c, text_language(c));
    uint32_t lang = arg32(c, 0);
    if (!l && c.mem->read<uint32_t>(c.esp) == from_callback && c.mem->read<uint32_t>(c.ebp + 4) == from_load) {
        const uint32_t saved = c.mem->read<uint32_t>(c.mem->read<uint32_t>(c.ebp - 4) + 0x10);
        if (saved && saved < 32 && validate(c, saved, false) == saved) lang = saved;
    }
    set(c, "__ZN8scimitar27SetCurrentSubtitlesLanguageENS_8LanguageE", l ? l : validate(c, lang, false));
}

// void scimitar::LocalizationUtils::ValidateAndSetSoundLanguage(Language)
void ov__ZN8scimitar17LocalizationUtils27ValidateAndSetSoundLanguageENS_8LanguageE(Context& c) {
    const uint32_t l = voice_language(c);
    set(c, "__ZN8scimitar23SetCurrentSoundLanguageENS_8LanguageE", l ? l : validate(c, arg32(c, 0), true));
}

}  // namespace scimitar::overrides

namespace acb {

// Wraps the options menu's SetVariable (game.cpp's start): it sets the
// languages the player picks, which then win over the launcher's.
void install_language_menu() {
    const char* name = "__ZN8scimitar3ac211OptionsMenu11SetVariableEtPKPKwS3_RKN4fire18ActionScriptBridge9EventInfoE";
    auto* entry = const_cast<scimitar::FuncEntry*>(scimitar::find_function(scimitar::guest_symbol(name)));
    if (!entry) return game::log("language: %s is not translated\n", name);
    scimitar::overrides::g_set_variable = entry->fn;
    entry->fn = scimitar::overrides::set_variable;
}

}  // namespace acb
