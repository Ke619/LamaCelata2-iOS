// The HUD on screens that are not 16:9, natively (scimitar-recomp --override of
// scimitar::FireItem::Render). AC2 and Brotherhood share it; each says where
// its HUD's movies belong in hud::kLayout (games/<id>/hud.cpp).
//
// The UI is Flash: movies laid out on a 1280x720 stage, each played by a
// FireItem of its own (health, minimap, the icons over people, menus,
// messages). Money, weapon, button prompts, subtitles and the party's state
// are components that one movie (HUD_MasterScript) loads into clips of its
// own: HUD_Money, HUD_Equipment, HUD_Controls, HUD_Subtitles, HUD_Party.
// When the picture fills a screen of another shape, the stage is fitted to it
// (scale min(width / 1280, height / 720)), which leaves room at the sides of a
// wider screen or above and below a taller one; AC2's view.cpp puts the stage
// at the top left, Brotherhood's renderer centres it. Here each movie or
// component moves to the edges it belongs to, by the share of that room its
// anchor says, in up to three ways:
//
//  - the whole movie, by its player's viewport origin
//    (fire::Player::SetViewportOrigin);
//  - a clip inside it, by its _x and _y, set the way ActionScript's
//    setProperty sets them (the Flash player's CorePlayer::FindTargetThread
//    and SetProperty; fire's own SetVariable only sets root variables, and
//    finds only clips a movie registers);
//  - all the movie draws, on the screen (Metal: set_draw_shift): the minimap,
//    whose map picture the game draws itself from a texture placed on the
//    screen, and which follows neither of the others.
//
// Brotherhood's Flash player draws nothing that starts outside the 1280x720
// stage (counted before the viewport origin applies; AC2's draws it), so its
// clips stay where the movie put them and whole movies move instead; and
// pictures the game draws (the weapon in hand, button icons) follow the
// viewport origin but not their clips. A movie with components is therefore
// drawn in passes, each from its own origin with only its own clips shown; a
// pass with nothing shown is skipped.
//
// The icons over people stay: the game places them on the screen itself.
// SCIMITAR_TRACE_UI=1 prints each movie once, and every clip it moves.
#include "hud.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "scimitar/guest_heap.h"
#include "scimitar/image_loader.h"
#include "scimitar/metal_gl.h"
#include "shell_common.h"

namespace {

using scimitar::Context;
using scimitar::GuestMemory;
using namespace hud;

// The game's functions this calls, by symbol (the same in AC2 and Brotherhood).
struct Functions {
    uint32_t lock, unlock;                // Gear::AdaptiveLock::Lock(), Unlock()
    uint32_t render;                      // fire::Player::Render()
    uint32_t set_origin;                  // fire::Player::SetViewportOrigin(short, short)
    uint32_t set_size;                    // fire::Player::SetViewportSize(unsigned short, unsigned short)
    uint32_t get_controller;              // fire::Player::GetPlayerController() const
    uint32_t get_variable, get_bool;      // fire::PlayerController::GetVariable(const wchar_t*, double& / bool&) const
    uint32_t open_layer;                  // CorePlayer::OpenLayer(int, int, int): a layer's ScriptThread
    uint32_t find_target;                 // CorePlayer::FindTargetThread(ScriptThread*, const char*, SecurityAccess)
    uint32_t set_property;                // CorePlayer::SetProperty(ScriptThread*, int, const double&)
};
const Functions& fns() {
    static const Functions f = [] {
        using scimitar::guest_symbol;
        Functions r;
        r.lock = guest_symbol("__ZN4Gear12AdaptiveLock4LockEv");
        r.unlock = guest_symbol("__ZN4Gear12AdaptiveLock6UnlockEv");
        r.render = guest_symbol("__ZN4fire6Player6RenderEv");
        r.set_origin = guest_symbol("__ZN4fire6Player17SetViewportOriginEss");
        r.set_size = guest_symbol("__ZN4fire6Player15SetViewportSizeEtt");
        r.get_controller = guest_symbol("__ZNK4fire6Player19GetPlayerControllerEv");
        r.get_variable = guest_symbol("__ZNK4fire16PlayerController11GetVariableEPKwRd");
        r.get_bool = guest_symbol("__ZNK4fire16PlayerController11GetVariableEPKwRb");
        r.open_layer = guest_symbol("__ZN10CorePlayer9OpenLayerEiii");
        r.find_target = guest_symbol("__ZN10CorePlayer16FindTargetThreadEP12ScriptThreadPKc14SecurityAccess");
        r.set_property = guest_symbol("__ZN10CorePlayer11SetPropertyEP12ScriptThreadiRKd");
        return r;
    }();
    return f;
}

enum Prop : uint32_t { kPropX = 0, kPropY = 1, kPropVisible = 7 };  // ActionScript's property numbers

const bool g_trace = std::getenv("SCIMITAR_TRACE_UI") != nullptr;

const Movie kCentred = {"", {{kCentre}}};

// A FireItem's name: its player's description (scimitar::FirePlayer +0xc, a
// std::string, whose pointer is its characters).
std::string item_name(GuestMemory& m, uint32_t item) {
    const uint32_t player = m.read<uint32_t>(item + 0x7c);
    const uint32_t chars = player ? m.read<uint32_t>(player + 0xc) : 0;
    std::string s;
    for (uint32_t a = chars; a && s.size() < 128; a++) {
        const char ch = (char)m.read<uint8_t>(a);
        if (!ch) break;
        s += ch;
    }
    return s;
}

// Guest strings, made once and kept: wchar_t (4 bytes a character) and char.
uint32_t guest_wide(GuestMemory& m, const std::string& s) {
    const uint32_t a = scimitar::heap::alloc(m, 4 * ((uint32_t)s.size() + 1));
    for (size_t k = 0; k <= s.size(); k++) m.write<uint32_t>(a + 4 * (uint32_t)k, k < s.size() ? (uint8_t)s[k] : 0);
    return a;
}
uint32_t guest_chars(GuestMemory& m, const std::string& s) {
    const uint32_t a = scimitar::heap::alloc(m, (uint32_t)s.size() + 1);
    std::memcpy(m.ptr(a), s.c_str(), s.size() + 1);
    return a;
}

// A clip's _x or _y: its value as the movie set it, and the value held.
struct Property {
    uint32_t path = 0;  // guest: "_root.HUD_Money._x", for GetVariable
    double base = 0;
    bool have_base = false;
    double held = NAN;
};
struct ClipState {
    const Clip* clip = nullptr;
    size_t pass = 0;
    uint32_t name = 0, visible_path = 0;  // guest: "HUD_Money" (FindTargetThread), "_root.HUD_Money._visible"
    Property x, y;
    uint32_t target = 0;   // this frame's ScriptThread
    bool visible = false;  // as the movie has it this frame (the menus hide the HUD's components)
};
struct ItemState {
    const Movie* movie = nullptr;
    int16_t origin_x = 0, origin_y = 0;  // the viewport origin FireItem::Init set
    uint32_t origin_set = 0xffffffff;    // the origin last set here, packed
    uint32_t size = 0;                   // the viewport size set here, packed
    std::vector<ClipState> clips;
};
std::mutex g_lock;
std::map<uint32_t, ItemState> g_items;
uint32_t g_value = 0;  // guest: a double for GetVariable and SetProperty

// The Flash player (CorePlayer) of a fire::Player: its controller's
// PlayerPrivate, whose fire::Movie (+0x4c) holds the player's FI object (+4),
// which holds the CorePlayer (+4).
uint32_t core_player(GuestMemory& m, uint32_t controller) {
    const uint32_t priv = m.read<uint32_t>(controller);
    const uint32_t movie = priv ? m.read<uint32_t>(priv + 0x4c) : 0;
    const uint32_t fi = movie ? m.read<uint32_t>(movie + 4) : 0;
    return fi ? m.read<uint32_t>(fi + 4) : 0;
}

struct Flash {
    Context& c;
    uint32_t controller, core;
    // _visible is a boolean, which the double getter does not read.
    bool get_bool(uint32_t path, bool* out) {
        GuestMemory& m = *c.mem;
        m.write<uint32_t>(g_value, 0xff);
        if (call_guest(c, fns().get_bool, {controller, path, g_value}) != 0) return false;
        const uint8_t b = m.read<uint8_t>(g_value);
        if (b > 1) return false;
        *out = b != 0;
        return true;
    }
    bool get(uint32_t path, double* out) {
        GuestMemory& m = *c.mem;
        m.write<double>(g_value, NAN);
        if (call_guest(c, fns().get_variable, {controller, path, g_value}) != 0) return false;
        *out = m.read<double>(g_value);
        return !std::isnan(*out);
    }
    void set(uint32_t target, Prop prop, double v) {
        c.mem->write<double>(g_value, v);
        call_guest(c, fns().set_property, {core, target, (uint32_t)prop, g_value});
    }
};

// Holds a clip's _x or _y at the movie's value plus `offset`. One the movie
// moves itself (or made again) is taken from where it put it.
void hold(Flash& f, uint32_t target, Prop prop, Property& p, double offset) {
    double now;
    if (!f.get(p.path, &now)) return;
    // Positions are kept in twips (1/20 of a pixel): compare to the nearest.
    const auto same = [](double a, double b) { return std::fabs(a - b) < 0.05; };
    if (!p.have_base || (!same(now, p.held) && !same(now, p.base))) p.base = now, p.have_base = true;
    const double want = std::round((p.base + offset) * 20) / 20;
    p.held = want;
    if (!same(now, want)) f.set(target, prop, want);
}

void set_origin(Context& c, uint32_t player, ItemState& it, int16_t x, int16_t y) {
    const uint32_t want = (uint32_t)(uint16_t)x << 16 | (uint16_t)y;
    if (it.origin_set == want || (it.origin_set == 0xffffffff && x == it.origin_x && y == it.origin_y)) return;
    call_guest(c, fns().set_origin, {player, (uint32_t)(int32_t)x, (uint32_t)(int32_t)y});
    it.origin_set = want;
}

void render(Context& c, uint32_t player, int shift_x, int shift_y) {
    if (shift_x || shift_y) scimitar::metal::set_draw_shift(shift_x, shift_y);
    call_guest(c, fns().render, {player});
    if (shift_x || shift_y) scimitar::metal::set_draw_shift(0, 0);
}

ItemState& item(GuestMemory& m, uint32_t self) {
    ItemState& it = g_items[self];
    if (it.movie) return it;
    const std::string name = item_name(m, self);
    it.movie = &kCentred;
    for (const Movie& mv : kLayout.movies)
        if (name == mv.name) it.movie = &mv;
    for (size_t p = 0; p < it.movie->passes.size(); p++)
        for (const Clip& clip : it.movie->passes[p].clips) {
            ClipState s;
            s.clip = &clip, s.pass = p;
            s.name = guest_chars(m, clip.name);
            const std::string path = std::string("_root.") + clip.name;
            s.x.path = guest_wide(m, path + "._x"), s.y.path = guest_wide(m, path + "._y");
            s.visible_path = guest_wide(m, path + "._visible");
            it.clips.push_back(s);
        }
    // FireItem::Init: the settings' origin (+0x38, +0x3a), or (0, 0) with flag +0x3c.
    if (!m.read<uint8_t>(self + 0x3c))
        it.origin_x = (int16_t)m.read<uint16_t>(self + 0x38), it.origin_y = (int16_t)m.read<uint16_t>(self + 0x3a);
    if (g_trace)
        std::fprintf(stderr, "ui: movie %08x \"%s\" origin (%d, %d), %zu pass(es), %zu clips\n", self, name.c_str(),
                     it.origin_x, it.origin_y, it.movie->passes.size(), it.clips.size());
    return it;
}

}  // namespace

namespace scimitar::overrides {

// void scimitar::FireItem::Render(): the movie's frame, under the item's lock
// (+0x48), by its player (+0x7c); here drawn at the edges it belongs to.
// Brotherhood's draws nothing while a flag of the item is set (+0x82).
void ov__ZN8scimitar8FireItem6RenderEv(scimitar::Context& c) {
    GuestMemory& m = *c.mem;
    const uint32_t self = arg32(c, 0);
    if (kLayout.hidden_flag && m.read<uint8_t>(self + kLayout.hidden_flag)) return shim_return(c, 0);
    const uint32_t player = m.read<uint32_t>(self + 0x7c);
    call_guest(c, fns().lock, {self + 0x48});
    if (!player) {
        call_guest(c, fns().render, {player});
        call_guest(c, fns().unlock, {self + 0x48});
        return shim_return(c, 0);
    }
    std::lock_guard<std::mutex> l(g_lock);
    if (!g_value) g_value = scimitar::heap::alloc(m, 8);
    float room_w = 0, room_h = 0;
    kLayout.room(m, &room_w, &room_h);
    const float scale = (float)shell::g_mode.have_w / (1280.0f + room_w);  // the UI's, in pixels per stage unit
    const Anchor stage = kLayout.stage;
    ItemState& it = item(m, self);
    const Movie& mv = *it.movie;

    // Clips moving past the stage would need a viewport of the screen's size,
    // as the player draws nothing outside it; but fire's PlayerPrivate takes a
    // new size only while no movie is loaded (+0x4c), so this changes nothing
    // once the HUD plays (AC2's older Flash player draws past it anyway;
    // Brotherhood's culls there, so its layout never moves clips past the
    // stage).
    Flash f{c, 0, 0};
    uint32_t root = 0;
    if (!it.clips.empty()) {
        const uint32_t w = (uint32_t)std::ceil(1280.0f + room_w), h = (uint32_t)std::ceil(720.0f + room_h);
        if ((w << 16 | h) != it.size && (it.size || w != 1280 || h != 720)) {
            call_guest(c, fns().set_size, {player, w, h});
            it.size = w << 16 | h;
        }
        f.controller = call_guest(c, fns().get_controller, {player});
        f.core = f.controller ? core_player(m, f.controller) : 0;
        root = f.core ? call_guest(c, fns().open_layer, {f.core, 0, 1, 0}) : 0;
        for (ClipState& s : it.clips) {
            s.target = root ? call_guest(c, fns().find_target, {f.core, root, s.name, 0}) : 0;
            // A clip whose visibility cannot be read is left alone.
            if (s.target && !f.get_bool(s.visible_path, &s.visible)) s.visible = false;
        }
    }
    // A 16:9 screen has no room: one drawing as the movie has it.
    const bool flat = room_w == 0 && room_h == 0;
    const bool hide = !flat && mv.passes.size() > 1;
    for (size_t p = 0; p < (flat ? 1 : mv.passes.size()); p++) {
        const Pass& pass = mv.passes[p];
        if (hide && !pass.clips.empty()) {
            bool any = false;
            for (const ClipState& s : it.clips) any |= s.pass == p && s.target && s.visible;
            if (!any) continue;
        }
        // The origin moves the movie from where the game puts the stage.
        set_origin(c, player, it, (int16_t)(it.origin_x + (flat ? 0 : std::lround((pass.origin.x - stage.x) * room_w))),
                   (int16_t)(it.origin_y + (flat ? 0 : std::lround((pass.origin.y - stage.y) * room_h))));
        for (ClipState& s : it.clips) {
            if (!s.target) continue;
            if (hide && s.visible) f.set(s.target, kPropVisible, s.pass == p ? 1 : 0);
            if (s.pass != p && !flat) continue;
            hold(f, s.target, kPropX, s.x, flat ? 0 : (s.clip->anchor.x - pass.origin.x) * room_w);
            hold(f, s.target, kPropY, s.y, flat ? 0 : (s.clip->anchor.y - pass.origin.y) * room_h);
        }
        int dx = 0, dy = 0;
        if (pass.draw_at && !flat && scimitar::metal::requested()) {
            dx = (int)std::lround((pass.draw_at->x - pass.origin.x) * room_w * scale);
            dy = (int)std::lround((pass.draw_at->y - pass.origin.y) * room_h * scale);
        }
        render(c, player, dx, dy);
    }
    if (hide)
        for (ClipState& s : it.clips)
            if (s.target && s.visible) f.set(s.target, kPropVisible, 1);
    call_guest(c, fns().unlock, {self + 0x48});
    shim_return(c, 0);
}

}  // namespace scimitar::overrides
