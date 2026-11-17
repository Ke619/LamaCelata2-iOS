// AC2's languages: text and voices apart.
//
// The game plays in its installed language, the Info.plist's "Language" key
// (SCIMITAR_GAME_LANGUAGE answers it, platform/src/cf.cpp):
// scimitar::SelectInstalledLanguage sets the text, subtitle and voice
// languages from it at startup, after the game options (/language,
// /soundlanguage) ran, so those have no effect (docs/AC2_DEV_OPTIONS.md).
// The replacement below does the same, except that SCIMITAR_TEXT_LANGUAGE
// sets the text and subtitles and SCIMITAR_VOICE_LANGUAGE the voices (short
// names: eng, fre, ger, ita, spa, rus...). A text language the game lacks
// falls back to its default (GameConfig::ValidateAndSetTextLanguage).
//
// The game's own Dialogue Language option (the text's language or Italian,
// kept with the player's options) and its settings, once loaded, set the
// voices again through GameConfig::ValidateAndSetSoundLanguage, which accepts
// only the languages of the game's list (the installed one and Italian). Its
// replacement accepts every voice language installed with the game
// (sounddata/Mac/sounds_<code>.pck), and at startup SCIMITAR_VOICE_LANGUAGE
// instead of the options' choice. Loading the player's options
// (PlayerOptionsSaveData::LoadData) resets a saved voice language other than
// the text's or Italian to the text's, through the same function: there the
// replacement keeps the saved one if it is installed.
//
// The options menu itself (OptionsMenu::GetVariable and SetVariable, wrapped
// through the function table: both are virtual, with no direct callers)
// offers every installed voice language in Dialogue Language, instead of the
// text's language and Italian.
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "game.h"
#include "scimitar/guest_heap.h"
#include "scimitar/image_loader.h"
#include "scimitar/imports.h"
#include "scimitar/runtime.h"

namespace ac2 {

bool g_from_menu = false;  // the options menu is setting the voice language

// scimitar::Language's short names, by number.
const char* const kShortNames[] = {"",    "eng", "fre", "spa", "pol", "ger", "chi", "hun", "ita",
                                   "jap", "cze", "kor", "rus", "dut", "dan", "nor", "swe"};
constexpr uint32_t kLanguages = sizeof kShortNames / sizeof *kShortNames;

// Whether the game has voices in `lang` (its working directory is the app's Resources).
bool voice_installed(uint32_t lang) {
    static const std::vector<bool> installed = [] {
        std::vector<bool> v(kLanguages);
        for (uint32_t l = 1; l < kLanguages; l++) {
            const std::string path = std::string("sounddata/Mac/sounds_") + kShortNames[l] + ".pck";
            v[l] = ::access(scimitar::host_path(path.c_str()).c_str(), R_OK) == 0;
        }
        return v;
    }();
    return lang < kLanguages && installed[lang];
}

namespace {

scimitar::GuestFn g_get_variable, g_set_variable;  // the originals

std::u32string guest_wide(scimitar::GuestMemory& m, uint32_t p) {
    std::u32string s;
    for (; p && m.read<uint32_t>(p); p += 4) s += (char32_t)m.read<uint32_t>(p);
    return s;
}
uint32_t guest_wide_copy(scimitar::GuestMemory& m, const std::u32string& s) {
    const uint32_t a = scimitar::heap::alloc(m, 4 * ((uint32_t)s.size() + 1));
    for (size_t k = 0; k <= s.size(); k++) m.write<uint32_t>(a + 4 * (uint32_t)k, k < s.size() ? s[k] : 0);
    return a;
}

// The installed voice languages, in Language order: the menu's rows.
const std::vector<uint32_t>& voices() {
    static const std::vector<uint32_t> v = [] {
        std::vector<uint32_t> r;
        for (uint32_t l = 1; l < kLanguages; l++)
            if (voice_installed(l)) r.push_back(l);
        return r;
    }();
    return v;
}

// fire::BasicStringInterface<wchar_t>& out: its virtual Set(const wchar_t*) at +8.
void answer(scimitar::Context& c, uint32_t out, uint32_t text) {
    const uint32_t set = c.mem->read<uint32_t>(c.mem->read<uint32_t>(out) + 8);
    scimitar::call_guest(c, set, {out, text});
}

// bool OptionsMenu::GetVariable(unsigned short count, const wchar_t* const* names,
//                               fire::BasicStringInterface<wchar_t>& out)
void get_variable(scimitar::Context& c) {
    const uint32_t self = scimitar::arg32(c, 0), count = scimitar::arg32(c, 1) & 0xffff,
                   names = scimitar::arg32(c, 2), out = scimitar::arg32(c, 3);
    const std::u32string name = count ? guest_wide(*c.mem, c.mem->read<uint32_t>(names)) : U"";
    if (voices().size() < 2 || (name != U"OptionsSupportedLanguages" && name != U"OptionsSpokenLanguage"))
        return g_get_variable(c);
    if (name == U"OptionsSpokenLanguage") {
        // The row of the language playing.
        static const uint32_t get_sound = scimitar::guest_symbol("__ZN8scimitar23GetCurrentSoundLanguageEv");
        const uint32_t current = scimitar::call_guest(c, get_sound, {});
        size_t row = 0;
        for (size_t k = 0; k < voices().size(); k++)
            if (voices()[k] == current) row = k;
        const std::string digits = std::to_string(row);
        if (std::getenv("SCIMITAR_TRACE_UI")) game::log("ui: OptionsSpokenLanguage = %s\n", digits.c_str());
        answer(c, out, guest_wide_copy(*c.mem, std::u32string(digits.begin(), digits.end())));
    } else {
        // "English;French;...", in the text's language (the text cannot change while the game runs).
        static uint32_t list = 0;
        if (!list) {
            static const uint32_t oasis_id = scimitar::guest_symbol("__ZN8scimitar3ac211OptionsMenu18GetLanguageOasisIDENS_8LanguageE"),
                                  instance = scimitar::guest_symbol("__ZN8scimitar19LocalizationManager11GetInstanceEv"),
                                  localized = scimitar::guest_symbol(
                                      "__ZN8scimitar19LocalizationManager18GetLocalizedStringElRNS_20SimpleStringTemplateIwLm4EEE");
            const uint32_t holder = scimitar::heap::alloc(*c.mem, 16);  // a SimpleStringTemplate<wchar_t>: its text first
            std::u32string joined;
            for (uint32_t l : voices()) {
                std::memset(c.mem->ptr(holder), 0, 16);
                const uint32_t id = scimitar::call_guest(c, oasis_id, {self, l});
                scimitar::call_guest(c, localized, {scimitar::call_guest(c, instance, {}), id, holder});
                std::u32string text = guest_wide(*c.mem, c.mem->read<uint32_t>(holder));
                if (text.empty()) text = std::u32string(kShortNames[l], kShortNames[l] + 3);
                joined += (joined.empty() ? U"" : U";") + text;
            }
            list = guest_wide_copy(*c.mem, joined);
            if (std::getenv("SCIMITAR_TRACE_UI")) {
                std::string utf8;  // the names in UTF-8
                for (char32_t ch : joined) {
                    if (ch < 0x80) utf8 += (char)ch;
                    else if (ch < 0x800) utf8 += (char)(0xc0 | ch >> 6), utf8 += (char)(0x80 | (ch & 0x3f));
                    else utf8 += (char)(0xe0 | ch >> 12), utf8 += (char)(0x80 | (ch >> 6 & 0x3f)), utf8 += (char)(0x80 | (ch & 0x3f));
                }
                game::log("ui: OptionsSupportedLanguages = %s\n", utf8.c_str());
            }
        }
        answer(c, out, list);
    }

    scimitar::shim_return(c, 0);  // as the original, which answers through `out`
}

// bool OptionsMenu::SetVariable(unsigned short count, const wchar_t* const* names,
//                               const wchar_t* value, const EventInfo&)
// The original sets the voices to the text's language (OptionsMenu +0x8c) for
// row 0 and to Italian otherwise; for a row of the full list it gets row 0
// with that row's language at +0x8c.
void set_variable(scimitar::Context& c) {
    const uint32_t self = scimitar::arg32(c, 0), count = scimitar::arg32(c, 1) & 0xffff,
                   names = scimitar::arg32(c, 2), value = scimitar::arg32(c, 3);
    if (voices().size() < 2 || !count || guest_wide(*c.mem, c.mem->read<uint32_t>(names)) != U"OptionsSpokenLanguage")
        return g_set_variable(c);
    const uint32_t value_slot = c.esp + 4 + 4 * 3;  // the value argument, rewritten for the original
    const std::u32string row_text = guest_wide(*c.mem, value);
    size_t row = 0;
    for (char32_t ch : row_text)
        if (ch >= U'0' && ch <= U'9') row = row * 10 + (ch - U'0');
    if (std::getenv("SCIMITAR_TRACE_UI")) {
        std::string raw;
        for (char32_t ch : row_text) raw += ch < 0x80 ? (char)ch : '?';
        game::log("ui: OptionsSpokenLanguage value \"%s\" (%u names)\n", raw.c_str(), count);
    }
    if (row >= voices().size()) return g_set_variable(c);
    if (std::getenv("SCIMITAR_TRACE_UI")) game::log("ui: OptionsSpokenLanguage set to row %zu (%s)\n", row, kShortNames[voices()[row]]);
    const uint32_t native = c.mem->read<uint32_t>(self + 0x8c);
    const uint32_t zero = guest_wide_copy(*c.mem, U"0");
    c.mem->write<uint32_t>(self + 0x8c, voices()[row]);
    c.mem->write<uint32_t>(value_slot, zero);
    g_from_menu = true;
    g_set_variable(c);
    g_from_menu = false;
    c.mem->write<uint32_t>(value_slot, value);
    c.mem->write<uint32_t>(self + 0x8c, native);
    scimitar::heap::release(*c.mem, zero);
}

}  // namespace

// Wraps the options menu's GetVariable and SetVariable (game.cpp's start).
void install_language_menu() {
    const char* const names[] = {
        "__ZN8scimitar3ac211OptionsMenu11GetVariableEtPKPKwRN4fire20BasicStringInterfaceIwEE",
        "__ZN8scimitar3ac211OptionsMenu11SetVariableEtPKPKwS3_RKN4fire18ActionScriptBridge9EventInfoE"};
    scimitar::GuestFn* originals[] = {&g_get_variable, &g_set_variable};
    const scimitar::GuestFn wrappers[] = {get_variable, set_variable};
    for (int k = 0; k < 2; k++) {
        auto* entry = const_cast<scimitar::FuncEntry*>(scimitar::find_function(scimitar::guest_symbol(names[k])));
        if (!entry) return game::log("language: %s is not translated\n", names[k]);
        *originals[k] = entry->fn;
        entry->fn = wrappers[k];
    }
}

}  // namespace ac2

namespace scimitar::overrides {

// void scimitar::SelectInstalledLanguage()
void ov__ZN8scimitar23SelectInstalledLanguageEv(scimitar::Context& c) {
    static const uint32_t get_name = guest_symbol("__Z18MacGetLanguageNamePci"),
                          to_language = guest_symbol("__ZN8scimitar26ShortLanguageName2LanguageEPKc"),
                          set_text = guest_symbol("__ZN8scimitar22SetCurrentTextLanguageENS_8LanguageE"),
                          set_subtitles = guest_symbol("__ZN8scimitar27SetCurrentSubtitlesLanguageENS_8LanguageE"),
                          set_sound = guest_symbol("__ZN8scimitar23SetCurrentSoundLanguageENS_8LanguageE");
    static const uint32_t buf = heap::alloc(*c.mem, 0x100);
    // The installed language, English (1) without one.
    uint32_t installed = 1;
    if (call_guest(c, get_name, {buf, 0x100}) & 0xff) installed = call_guest(c, to_language, {buf});
    const auto chosen = [&](const char* env) {
        const char* v = std::getenv(env);
        if (!v || !*v || std::strlen(v) >= 0x100) return installed;
        std::memcpy(c.mem->ptr(buf), v, std::strlen(v) + 1);
        const uint32_t l = call_guest(c, to_language, {buf});
        if (!l) game::log("unknown language %s in %s\n", v, env);
        return l ? l : installed;
    };
    const uint32_t text = chosen("SCIMITAR_TEXT_LANGUAGE"), voice = chosen("SCIMITAR_VOICE_LANGUAGE");
    call_guest(c, set_text, {text});
    call_guest(c, set_subtitles, {text});
    call_guest(c, set_sound, {voice});
    if (text != installed || voice != installed) game::log("text language %u, voices %u\n", text, voice);
    shim_return(c, 0);
}

// void scimitar::GameConfig::ValidateAndSetSoundLanguage(Language)
// GameConfig: +0x10 the valid voice languages (uint32_t array; its count in
// the low 14 bits of the uint16_t at +0x16), +0x18 the default one. Called
// when the settings and the player's options load (SCIMITAR_VOICE_LANGUAGE
// wins there) and when the options menu changes the language (which wins).
void ov__ZN8scimitar10GameConfig27ValidateAndSetSoundLanguageENS_8LanguageE(scimitar::Context& c) {
    static const uint32_t set_sound = guest_symbol("__ZN8scimitar23SetCurrentSoundLanguageENS_8LanguageE"),
                          to_language = guest_symbol("__ZN8scimitar26ShortLanguageName2LanguageEPKc");
    static const uint32_t forced = [&]() -> uint32_t {
        const char* v = std::getenv("SCIMITAR_VOICE_LANGUAGE");
        if (!v || !*v || std::strlen(v) >= 0x100) return 0;
        const uint32_t buf = heap::alloc(*c.mem, 0x100);
        std::memcpy(c.mem->ptr(buf), v, std::strlen(v) + 1);
        return call_guest(c, to_language, {buf});
    }();
    // The return address: SetSoundLanguage_Callback jumps here, so it is the
    // callback's caller's.
    static const uint32_t load_reset = guest_symbol("__ZN8scimitar3ac221PlayerOptionsSaveData8LoadDataEv") + 0x5d;
    const uint32_t self = arg32(c, 0);
    uint32_t lang = arg32(c, 1);
    if (forced && !ac2::g_from_menu) {
        call_guest(c, set_sound, {forced});
        return shim_return(c, 0);
    }
    // PlayerOptionsSaveData::LoadData keeps saved voices only in the text's
    // language or Italian and resets others to the text's language, here; its
    // `this` is still in esi, the saved language at +0xc. Keep any installed one.
    if (c.mem->read<uint32_t>(c.esp) == load_reset) {
        const uint32_t saved = c.mem->read<uint32_t>(c.esi + 0xc);
        if (saved > 0 && saved < ac2::kLanguages && ac2::voice_installed(saved)) lang = saved;
    }
    const uint32_t list = c.mem->read<uint32_t>(self + 0x10), n = c.mem->read<uint16_t>(self + 0x16) & 0x3fff,
                   fallback = c.mem->read<uint32_t>(self + 0x18);
    const auto valid = [&](uint32_t l) {
        for (uint32_t k = 0; k < n; k++)
            if (c.mem->read<uint32_t>(list + 4 * k) == l) return true;
        return ac2::voice_installed(l);
    };
    uint32_t chosen = 1;  // English
    if (lang && valid(lang)) chosen = lang;
    else if (lang == 4 && fallback == 12 && valid(1)) chosen = 1;  // Polish where the default is Russian
    else if (fallback && valid(fallback)) chosen = fallback;
    if (std::getenv("SCIMITAR_TRACE_UI")) game::log("ui: voices %u asked, %u set\n", lang, chosen);
    call_guest(c, set_sound, {chosen});
    shim_return(c, 0);
}

}  // namespace scimitar::overrides
