// The F4 graphics menu: the settings behind the games' graphics options, and
// values past the options' highest, changed live.
//
// The options (GraphicsOptions::Set*Quality_Callback) only fill fields of a
// GraphicsOptions; GraphicsOptions::Apply copies a few to the view surface
// (g_ViewSurface) and resizes the shadow maps. The menu writes the same fields
// of the applied options (g_AppliedGraphicsOptions) and their view surface
// copies itself, as Apply would, without its mode, vsync and MSAA switches:
//   - Brotherhood's LOD distance multiplier (+0x40, 0.5 to 2.0 by environment
//     quality): World::UpdateCullingFlags and GetLODLevelFromDistance divide
//     distances by it every frame. AC2 has none; its LOD and culling scale is
//     tan(fov / 2) * 2.4142 (VisibilityContext::SetFrom, World::UpdateCullingFlags,
//     one constant in __const), so the menu scales that constant.
//   - The character LOD multiplier (character quality): Brotherhood's
//     CrowdHerder multiplies switch distances by it (1.0 to 1.6, higher is
//     better), AC2's LODSelectorInstance multiplies distances by it (1.6 to 1.0,
//     lower is better).
//   - Two distance pairs by environment and shadow quality, as integers here and
//     floats on the view surface: the environment pair (35/55 to 70/90) and the
//     shadow pair (9/20 to 20/40 in Brotherhood, 9/15 to 15/30 in AC2).
//   - The shadow map size (512 to 2048, AC2 1024): OGLGraphicDevice::
//     UpdateShadowMapSize marks the shadow targets for re-creation at that size.
// And the Metal renderer's anisotropic filtering, which no option sets.
//
// A setting's switch (SCIMITAR_DRAW_DISTANCE=6, SCIMITAR_SHADOW_DISTANCES=45/90,
// ...: the launcher's Graphics choices) and a choice in the menu are kept in
// effect until the game quits: enforce(), at every event pump, puts them back
// when the game applies its own options (at start, from its options menu).
#include "graphics_menu.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "game.h"
#include "scimitar/image_loader.h"
#include "scimitar/metal_gl.h"

namespace graphics_menu {
namespace {

using scimitar::Context;

struct Choice {
    std::string label;
    std::vector<double> v;  // the value (two for distance pairs)
};

struct Item {
    std::string name;
    const char* env;  // the switch that sets it from the start (the launcher's choice)
    std::vector<Choice> choices;
    std::function<std::vector<double>(Context&)> get;    // the game's current value
    std::function<void(Context&, const std::vector<double>&)> set;
    int current = -1;    // the choice matching get(), or -1
    std::string custom;  // get() when it matches none
    std::vector<double> target;  // kept in effect (from env, then the menu); empty for none
    int applied = 0;             // times target was (re)applied
};

// Where the settings are in each game (see the top).
struct Layout {
    const char* applied;                                // g_AppliedGraphicsOptions
    uint32_t lod;                                       // LOD distance multiplier (float), 0 for none
    uint32_t character;                                 // character LOD multiplier (float)
    uint32_t env_near, env_far, shadow_near, shadow_far;  // distances (int)
    uint32_t shadow_size;                               // shadow map size (int)
    uint32_t vs_env_near, vs_shadow_near, vs_env_far, vs_shadow_far;  // their floats on the view surface
};
constexpr Layout kBrotherhood{"__ZN8scimitarL24g_AppliedGraphicsOptionsE", 0x40, 0x2c, 0x30, 0x34, 0x38, 0x3c,
                              0x20, 0xb0, 0xb4, 0xb8, 0xbc};
constexpr Layout kAC2{"__ZN8scimitar24g_AppliedGraphicsOptionsE", 0, 0x24, 0x28, 0x2c, 0x30, 0x34,
                      0x18, 0xac, 0xb0, 0xb4, 0xb8};

struct Menu {
    bool built = false;
    std::vector<Item> items;
    int selected = 0;
};
Menu m;

uint32_t sym(const char* name) { return scimitar::guest_symbol_or_0(name); }

// AC2's LOD scale constant: VisibilityContext::SetFrom's mulss at +0x60
// (f3 0f 59 05 <address>), 1 / tan(22.5 degrees). 0 if it is not there.
uint32_t ac2_lod_constant(Context& c, float* original) {
    const uint32_t fn = sym("__ZN8scimitar17VisibilityContext7SetFromEPNS_4ViewE");
    if (!fn || c.mem->read<uint32_t>(fn + 0x60) != 0x05590ff3u) return 0;
    const uint32_t at = c.mem->read<uint32_t>(fn + 0x64);
    const float first = c.mem->read<float>(at);
    if (std::fabs(first - 2.4142135f) > 1e-3f) return 0;
    *original = first;
    return at;
}

std::string format_value(const std::vector<double>& v) {
    std::string s;
    for (double x : v) {
        char b[32];
        std::snprintf(b, sizeof b, x == std::floor(x) ? "%.0f" : "%.2f", x);
        if (!s.empty()) s += " / ";
        s += b;
    }
    return s;
}

void build(Context& c) {
    m.built = true;
    const bool acb = std::strcmp(game::kInfo.id, "acb") == 0;
    const Layout& L = acb ? kBrotherhood : kAC2;
    const uint32_t opts = sym(L.applied);
    const uint32_t view_surface = sym("__ZN8scimitar13g_ViewSurfaceE");
    const uint32_t device = sym("__ZN8scimitar16OGLGraphicDevice16m_MonitorsDeviceE");
    const uint32_t update_shadow_size = sym("__ZN8scimitar16OGLGraphicDevice19UpdateShadowMapSizeEm");
    const uint32_t shadow_size_now = sym("__ZN8scimitar13ShadowMapSizeE");  // what the device last made
    auto rf = [opts](Context& c, uint32_t off) { return (double)c.mem->read<float>(opts + off); };
    auto ri = [opts](Context& c, uint32_t off) { return (double)c.mem->read<uint32_t>(opts + off); };

    if (scimitar::metal::requested()) {
        m.items.push_back({"Anisotropic filtering", "SCIMITAR_ANISO",
                           {{"off (game)", {1}}, {"2x", {2}}, {"4x", {4}}, {"8x", {8}}, {"16x", {16}}},
                           [](Context&) { return std::vector<double>{(double)scimitar::metal::forced_anisotropy()}; },
                           [](Context&, const std::vector<double>& v) { scimitar::metal::set_forced_anisotropy((int)v[0]); }});
    }
    if (!opts) {
        game::log("graphics menu: no %s\n", L.applied);
        return;
    }

    // Draw distance.
    if (L.lod) {
        m.items.push_back({"Draw distance (LOD x)", "SCIMITAR_DRAW_DISTANCE",
                           {{"0.5 (game: lowest)", {0.5}}, {"0.8", {0.8}}, {"1.0", {1.0}}, {"1.5", {1.5}},
                            {"2.0 (game: highest)", {2.0}}, {"3.0", {3.0}}, {"4.0", {4.0}}, {"6.0", {6.0}}},
                           [=](Context& c) { return std::vector<double>{rf(c, L.lod)}; },
                           [=](Context& c, const std::vector<double>& v) { c.mem->write<float>(opts + L.lod, (float)v[0]); }});
    } else {
        float original = 0;
        if (const uint32_t at = ac2_lod_constant(c, &original)) {
            m.items.push_back({"Draw distance (LOD x)", "SCIMITAR_DRAW_DISTANCE",
                               {{"0.5", {0.5}}, {"1.0 (game)", {1.0}}, {"1.5", {1.5}}, {"2.0", {2.0}}, {"3.0", {3.0}},
                                {"4.0", {4.0}}, {"6.0", {6.0}}},
                               [=](Context& c) { return std::vector<double>{original / c.mem->read<float>(at)}; },
                               [=](Context& c, const std::vector<double>& v) {
                                   c.mem->write<float>(at, original / (float)v[0]);
                               }});
        } else {
            game::log("graphics menu: AC2's LOD scale constant is not where expected\n");
        }
    }

    // Character detail.
    if (acb) {
        m.items.push_back({"Character detail (LOD x)", "SCIMITAR_CHARACTER_DETAIL",
                           {{"1.0 (game: lowest)", {1.0}}, {"1.3", {1.3}}, {"1.6 (game: highest)", {1.6}}, {"2.0", {2.0}},
                            {"3.0", {3.0}}, {"4.0", {4.0}}},
                           [=](Context& c) { return std::vector<double>{rf(c, L.character)}; },
                           [=](Context& c, const std::vector<double>& v) { c.mem->write<float>(opts + L.character, (float)v[0]); }});
    } else {
        // AC2 multiplies the distance: lower keeps the detail farther.
        m.items.push_back({"Character detail (distance x)", "SCIMITAR_CHARACTER_DETAIL",
                           {{"1.6 (game: low)", {1.6}}, {"1.3", {1.3}}, {"1.0 (game: high)", {1.0}}, {"0.75", {0.75}},
                            {"0.5", {0.5}}, {"0.33", {0.33}}},
                           [=](Context& c) { return std::vector<double>{rf(c, L.character)}; },
                           [=](Context& c, const std::vector<double>& v) { c.mem->write<float>(opts + L.character, (float)v[0]); }});
    }

    // A distance pair: the options' integers and the view surface's floats.
    auto pair = [=](uint32_t near_off, uint32_t far_off, uint32_t vs_near, uint32_t vs_far) {
        return std::make_pair(
            std::function<std::vector<double>(Context&)>(
                [=](Context& c) {
                    // The view surface's copies when they differ (what the renderer uses).
                    const uint32_t vs = view_surface ? c.mem->read<uint32_t>(view_surface) : 0;
                    std::vector<double> v{ri(c, near_off), ri(c, far_off)};
                    if (vs && (c.mem->read<float>(vs + vs_near) != (float)v[0] || c.mem->read<float>(vs + vs_far) != (float)v[1]))
                        v = {c.mem->read<float>(vs + vs_near), c.mem->read<float>(vs + vs_far)};
                    return v;
                }),
            std::function<void(Context&, const std::vector<double>&)>([=](Context& c, const std::vector<double>& v) {
                c.mem->write<uint32_t>(opts + near_off, (uint32_t)v[0]);
                c.mem->write<uint32_t>(opts + far_off, (uint32_t)v[1]);
                const uint32_t vs = view_surface ? c.mem->read<uint32_t>(view_surface) : 0;
                if (vs) {
                    c.mem->write<float>(vs + vs_near, (float)v[0]);
                    c.mem->write<float>(vs + vs_far, (float)v[1]);
                }
            }));
    };
    {
        auto [get, set] = pair(L.env_near, L.env_far, L.vs_env_near, L.vs_env_far);
        std::vector<Choice> ch = acb ? std::vector<Choice>{{"35 / 55 (game: lowest)", {35, 55}}, {"40 / 60", {40, 60}},
                                                           {"45 / 65", {45, 65}}, {"60 / 80", {60, 80}},
                                                           {"70 / 90 (game: highest)", {70, 90}}, {"100 / 130", {100, 130}},
                                                           {"150 / 200", {150, 200}}, {"250 / 300", {250, 300}}}
                                     : std::vector<Choice>{{"35 / 55 (game: lowest)", {35, 55}}, {"40 / 60", {40, 60}},
                                                           {"45 / 65", {45, 65}}, {"50 / 70 (game: highest)", {50, 70}},
                                                           {"70 / 90", {70, 90}}, {"100 / 130", {100, 130}},
                                                           {"150 / 200", {150, 200}}, {"250 / 300", {250, 300}}};
        m.items.push_back({"Environment distances", "SCIMITAR_ENV_DISTANCES", ch, get, set});
    }
    {
        auto [get, set] = pair(L.shadow_near, L.shadow_far, L.vs_shadow_near, L.vs_shadow_far);
        std::vector<Choice> ch = acb ? std::vector<Choice>{{"9 / 20 (game: lowest)", {9, 20}}, {"12 / 25", {12, 25}},
                                                           {"15 / 30", {15, 30}}, {"20 / 40 (game: highest)", {20, 40}},
                                                           {"30 / 60", {30, 60}}, {"40 / 80", {40, 80}},
                                                           {"60 / 120", {60, 120}}}
                                     : std::vector<Choice>{{"9 / 15 (game: lowest)", {9, 15}}, {"9 / 20", {9, 20}},
                                                           {"12 / 25", {12, 25}}, {"15 / 30 (game: highest)", {15, 30}},
                                                           {"20 / 40", {20, 40}}, {"30 / 60", {30, 60}},
                                                           {"45 / 90", {45, 90}}};
        m.items.push_back({"Shadow distances", "SCIMITAR_SHADOW_DISTANCES", ch, get, set});
    }

    // Shadow map size.
    if (device && update_shadow_size) {
        std::vector<Choice> ch{{"512 (game: lowest)", {512}}, {"1024", {1024}}, {"2048", {2048}}, {"4096", {4096}}};
        (acb ? ch[2] : ch[1]).label += " (game: highest)";
        m.items.push_back({"Shadow map size", "SCIMITAR_SHADOW_MAP", ch, [=](Context& c) {
                               return std::vector<double>{shadow_size_now ? (double)c.mem->read<uint32_t>(shadow_size_now)
                                                                          : ri(c, L.shadow_size)};
                           },
                           [=](Context& c, const std::vector<double>& v) {
                               c.mem->write<uint32_t>(opts + L.shadow_size, (uint32_t)v[0]);
                               if (const uint32_t d = c.mem->read<uint32_t>(device))
                                   scimitar::call_guest(c, update_shadow_size, {d, (uint32_t)v[0]});
                           }});
    }
}

bool same(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.size() != b.size()) return false;
    for (size_t k = 0; k < a.size(); k++)
        if (std::fabs(a[k] - b[k]) >= 0.01 * std::max(1.0, std::fabs(a[k]))) return false;
    return true;
}

void read_current(Context& c, Item& it) {
    const std::vector<double> now = it.get(c);
    it.current = -1;
    for (size_t i = 0; i < it.choices.size(); i++) {
        if (same(it.choices[i].v, now)) {
            it.current = (int)i;
            return;
        }
    }
    it.custom = format_value(now);
}

// The item's switch: numbers separated by '/' ("6", "45/90"), as many as the
// item's values; anything else is ignored.
void read_env(Item& it) {
    const char* v = std::getenv(it.env);
    if (!v || !*v || it.choices.empty()) return;
    std::vector<double> t;
    for (const char* p = v;;) {
        char* end = nullptr;
        const double x = std::strtod(p, &end);
        if (end == p || !(x > 0) || !std::isfinite(x)) return game::log("graphics: %s=%s: not a value\n", it.env, v);
        t.push_back(x);
        if (*end == 0) break;
        if (*end != '/') return game::log("graphics: %s=%s: not a value\n", it.env, v);
        p = end + 1;
    }
    if (t.size() != it.choices[0].v.size()) return game::log("graphics: %s=%s: wants %zu values\n", it.env, v, it.choices[0].v.size());
    it.target = t;
}

}  // namespace

void enforce(Context& c) {
    if (!m.built) {
        build(c);
        for (Item& it : m.items) read_env(it);
    }
    for (Item& it : m.items) {
        if (it.target.empty() || same(it.get(c), it.target)) continue;
        it.set(c, it.target);
        // The first time, and a few more when the game's options put theirs back.
        if (it.applied++ < 4)
            game::log("graphics: %s %s%s\n", it.name.c_str(), format_value(it.target).c_str(),
                      it.applied == 1 ? "" : " (again, over the game's options)");
    }
}

void refresh(Context& c) {
    enforce(c);
    for (Item& it : m.items) read_current(c, it);
}

void key(Context& c, Key k) {
    if (m.items.empty()) return;
    const int n = (int)m.items.size();
    if (k == Key::Up) m.selected = (m.selected + n - 1) % n;
    if (k == Key::Down) m.selected = (m.selected + 1) % n;
    if (k != Key::Left && k != Key::Right) return;
    Item& it = m.items[m.selected];
    const int count = (int)it.choices.size();
    int next;
    if (it.current < 0) {
        // From a value between the choices: the next one up or down.
        const double now = it.get(c)[0];
        next = k == Key::Right ? count - 1 : 0;
        for (int i = 0; i < count; i++) {
            if (k == Key::Right && it.choices[i].v[0] > now) { next = i; break; }
            if (k == Key::Left && it.choices[i].v[0] < now) next = i;
        }
    } else {
        next = std::clamp(it.current + (k == Key::Right ? 1 : -1), 0, count - 1);
        if (next == it.current) return;
    }
    it.set(c, it.choices[next].v);
    it.target = it.choices[next].v;  // kept until the game quits, over its own options
    it.applied = 1;
    read_current(c, it);
    game::log("graphics menu: %s = %s\n", it.name.c_str(), it.choices[next].label.c_str());
}

std::string text() {
    std::string s = "GRAPHICS  F4 close  ↑↓ choose  ←→ change\n";
    if (m.items.empty()) return s + "\n  nothing to change";
    size_t width = 0;
    for (const Item& it : m.items) width = std::max(width, it.name.size());
    for (size_t i = 0; i < m.items.size(); i++) {
        const Item& it = m.items[i];
        const bool sel = (int)i == m.selected;
        const std::string value = it.current >= 0 ? it.choices[it.current].label : it.custom + " (custom)";
        s += "\n";
        s += sel ? "▸ " : "  ";
        s += it.name + std::string(width - it.name.size() + 2, ' ');
        s += sel ? "◂ " + value + " ▸" : "  " + value;
    }
    return s;
}

}  // namespace graphics_menu
