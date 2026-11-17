// "Remove color filter" (SCIMITAR_NO_COLOR_FILTER=1): the game's color filter
// off in every world, the effect of the PC mod "AC2's True Colors", without
// touching the game's data (color_filter_patch.h says what it changes).
//
// The world's ambiance record is patched as the game reads it: WorldAmbiance's
// FastLoad is replaced in the runtime's function table (the engine reaches
// FastLoads through member-function pointers and vtables, as pc_data.cpp says),
// and before the original runs, the record waiting in the serializer's buffer is
// patched where it is.
//
// SCIMITAR_AMBIANCE_SYMBOL=<mangled name> names the function when the built-in
// guess is not the game's.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "color_filter_patch.h"
#include "game.h"
#include "scimitar/image_loader.h"
#include "scimitar/runtime.h"

namespace {

using scimitar::Context;

// FastLoadSerializer: +0x14 read position, +0x18 end of the buffered bytes (see pc_data.cpp).
constexpr uint32_t kSerCursor = 0x14, kSerEnd = 0x18;
// The record is looked for in the first bytes at the read position (slack, in case the
// stream puts a few bytes of its own in front).
constexpr uint32_t kSlack = 64;

const char* const kGuesses[] = {
    "__ZN8scimitar13WorldAmbiance8FastLoadERNS_18FastLoadSerializerE",
};

scimitar::GuestFn g_original;
uint32_t g_read_from_stream = 0;
std::atomic<int> g_patched{0}, g_misses{0};

// At least n bytes buffered from the read position (false at the stream's end).
bool want(Context& c, uint32_t ser, uint32_t n) {
    const auto cursor = [&] { return c.mem->read<uint32_t>(ser + kSerCursor); };
    const auto end = [&] { return c.mem->read<uint32_t>(ser + kSerEnd); };
    for (int tries = 0; end() - cursor() < n; tries++) {
        const uint32_t before = end() - cursor();
        scimitar::call_guest(c, g_read_from_stream, {ser});
        if (end() - cursor() == before && tries) return false;
    }
    return true;
}

void world_ambiance(Context& c) {
    const uint32_t ser = scimitar::arg32(c, 1);
    want(c, ser, (uint32_t)ac2::kAmbianceSize + kSlack);
    const uint32_t cursor = c.mem->read<uint32_t>(ser + kSerCursor), end = c.mem->read<uint32_t>(ser + kSerEnd);
    const size_t avail = end > cursor ? end - cursor : 0;
    const size_t size = std::min<size_t>(avail, ac2::kAmbianceSize + kSlack);
    uint8_t* const at = c.mem->ptr(cursor);
    // The read position can be a few bytes into the record already (the loader reads a header first),
    // or a few bytes before it: try the first, then look for the record in the bytes that follow.
    const int lead = ac2::patch_at_read_position(at, size);
    const size_t n = lead >= 0 ? 1 : ac2::patch_color_filter(at, size);
    if (n) {
        const int total = g_patched += (int)n;
        if (total <= 3) game::log("color filter: switched off in an ambiance record (%d so far; %d of its bytes were already read)\n", total, lead < 0 ? 0 : lead);
    } else if (g_misses++ < 3) {
        const ac2::Fit f = ac2::best_fit(at, size);
        char first[16 * 3 + 1] = "";
        for (size_t k = 0; k < 16 && k < size; k++) std::snprintf(first + k * 3, 4, "%02x ", at[k]);
        game::log("color filter: a WorldAmbiance record that is not recognised (%zu bytes buffered; closest fit: %zu of %zu anchors "
                  "if %zu bytes were already read; first bytes: %s); left as it is\n",
                  avail, f.hits, f.seen, f.lead, first);
    }
    g_original(c);
}

}  // namespace

namespace ac2 {

void install_color_filter() {
    const char* on = std::getenv("SCIMITAR_NO_COLOR_FILTER");
    if (!on || !*on || *on == '0') return;
    g_read_from_stream = scimitar::guest_symbol_or_0("__ZN8scimitar18FastLoadSerializer14ReadFromStreamEv");
    const char* given = std::getenv("SCIMITAR_AMBIANCE_SYMBOL");
    scimitar::FuncEntry* entry = nullptr;
    const char* used = nullptr;
    const auto look = [&](const char* name) {
        if (entry) return;
        if (const uint32_t addr = scimitar::guest_symbol_or_0(name))
            if ((entry = const_cast<scimitar::FuncEntry*>(scimitar::find_function(addr)))) used = name;
    };
    if (given && *given) look(given);
    for (const char* name : kGuesses) look(name);
    if (!g_read_from_stream || !entry) {
        game::log("color filter: cannot switch it off: %s is not in this build (set SCIMITAR_AMBIANCE_SYMBOL to WorldAmbiance's FastLoad)\n",
                  g_read_from_stream ? "WorldAmbiance::FastLoad" : "FastLoadSerializer::ReadFromStream");
        return;
    }
    g_original = entry->fn;
    entry->fn = world_ambiance;
    game::log("color filter: on, through %s\n", used);
}

}  // namespace ac2
