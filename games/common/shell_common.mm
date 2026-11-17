// What the port's Mac and iOS shells share (shell_common.h).
//
// Game pads: the first connected controller the GameController framework
// offers as an extended gamepad (Xbox, PlayStation, MFi, ...) drives the
// virtual HID pad (platform/src/iokit.cpp, scimitar/gamepad.h), polled once per
// frame. The game looks for pads once, at startup, so a controller must be
// connected before launch; the game then lists it as an input device in its
// options. SCIMITAR_PAD scripts pad input for unattended runs (below).
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <mach/mach.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>
#endif
#include <pthread.h>
#include <sys/resource.h>

#include "game.h"
#include "shell_common.h"
#include "scimitar/image_loader.h"
#include "scimitar/gamepad.h"
#include "scimitar/guest_heap.h"
#include "scimitar/metal_gl.h"
#include "scimitar/runtime.h"

namespace scimitar {
void gl_frame_presented(uint64_t frame);  // platform/src/opengl.cpp
void gl_trace_frame();
void hid_keyboard_event(uint32_t usage, bool down);  // platform/src/iokit.cpp
}

namespace shell {

std::atomic<uint64_t> g_frames{0};
std::atomic<bool> g_keys_used{false};
std::atomic<uint32_t> g_virtual_buttons{0};
std::atomic<float> g_virtual_axis[6] = {};
std::atomic<bool> g_virtual_pad_present{false};
const bool g_hidden = std::getenv("SCIMITAR_HIDDEN") != nullptr;
const bool g_metal = scimitar::metal::requested();
Context* g_pump_context = nullptr;
Mode g_mode;

namespace {

const uint8_t kHidUsage[128] = {
    0x04, 0x16, 0x07, 0x09, 0x0b, 0x0a, 0x1d, 0x1b, 0x06, 0x19, 0x64, 0x05, 0x14, 0x1a, 0x08, 0x15,  // 0x00
    0x1c, 0x17, 0x1e, 0x1f, 0x20, 0x21, 0x23, 0x22, 0x2e, 0x26, 0x24, 0x2d, 0x25, 0x27, 0x30, 0x12,  // 0x10
    0x18, 0x2f, 0x0c, 0x13, 0x28, 0x0f, 0x0d, 0x34, 0x0e, 0x33, 0x31, 0x36, 0x38, 0x11, 0x10, 0x37,  // 0x20
    0x2b, 0x2c, 0x35, 0x2a, 0x00, 0x29, 0xe7, 0xe3, 0xe1, 0x39, 0xe2, 0xe0, 0xe5, 0xe6, 0xe4, 0x00,  // 0x30
    0x6c, 0x63, 0x00, 0x55, 0x00, 0x57, 0x00, 0x53, 0x00, 0x00, 0x00, 0x54, 0x58, 0x00, 0x56, 0x6d,  // 0x40
    0x6e, 0x67, 0x62, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f, 0x6f, 0x60, 0x61, 0x00, 0x00, 0x00,  // 0x50
    0x3e, 0x3f, 0x40, 0x3c, 0x41, 0x42, 0x00, 0x44, 0x00, 0x68, 0x00, 0x69, 0x00, 0x43, 0x00, 0x45,  // 0x60
    0x00, 0x6a, 0x49, 0x4a, 0x4b, 0x4c, 0x3d, 0x4d, 0x3b, 0x4e, 0x3a, 0x50, 0x4f, 0x51, 0x52, 0x00,  // 0x70
};

// The mouse: written by the event handling, read by MacGetMouseState.
struct Mouse {
    std::mutex lock;
    float dx = 0, dy = 0, dz = 0;
    struct Button {
        uint32_t number;
        bool down;
    };
    std::vector<Button> buttons;
    bool touched = false;  // a touch moved since the last MacGetMouseState, to tx, ty
    float tx = 0, ty = 0;
};
Mouse g_mouse;

// The UI's cursor (scimitar::g_FireMouseCursorPositionX and Y): what the game
// sends its Flash movies as the mouse's place, on their 1280x720 stage. Mouse
// motion moves it (MouseKeyboardPadEmulator::UpdateFireMouse); a touch sets
// it. The menus' stage is centred on the screen at the scale that fits it
// (games/ac2/view.cpp, hud.cpp; with SCIMITAR_ASPECT=16:9 the picture is that
// rectangle), and a movie's viewport origin moves only its drawing.
void place_ui_cursor(Context& c, float fx, float fy) {
    static const uint32_t x_at = scimitar::guest_symbol_or_0("__ZN8scimitar26g_FireMouseCursorPositionXE");
    static const uint32_t y_at = scimitar::guest_symbol_or_0("__ZN8scimitar26g_FireMouseCursorPositionYE");
    if (!x_at || !y_at) return;
    const float w = (float)g_mode.have_w, h = (float)g_mode.have_h;
    const float s = std::min(w / 1280.0f, h / 720.0f);
    const long x = std::lround((fx * w - (w - 1280.0f * s) * 0.5f) / s);
    const long y = std::lround((fy * h - (h - 720.0f * s) * 0.5f) / s);
    c.mem->write<int32_t>(x_at, (int32_t)std::clamp(x, 0L, 1279L));
    c.mem->write<int32_t>(y_at, (int32_t)std::clamp(y, 0L, 719L));
    if (std::getenv("SCIMITAR_TRACE_INPUT")) game::log("input: touch at (%ld, %ld) on the UI's stage\n", x, y);
}

// The menus' mouse cursor: on a PC or Mac (Common.PCUtils.isPC) the title
// menu and the pop-ups attach the library clip "marker" as "menuCursor" (or
// "cursor"; MovieClip.attachMovie) and move it to the mouse on every move.
// Hidden, it is never attached, and the ActionScript moving it moves nothing.
// The Flash player's MovieClip methods are MovieDispatchProc(NativeInfo*),
// reached only through its table of natives, so its function table entry is
// replaced (no re-translation). NativeInfo: +8 the argument count, +0xc the
// arguments (ScriptAtom, 4 bytes each), +0x1c the method (0 attachMovie: the
// library name, the new clip's name, its depth). A string atom is a
// StringRep16* tagged 4 in its low three bits, its characters at +0x10
// (8-bit) or else at +4 (wchar_t).
scimitar::GuestFn g_movie_dispatch = nullptr;
bool (*g_cursor_hidden)() = nullptr;

bool atom_is(const scimitar::GuestMemory& m, uint32_t atom, const char* want) {
    if ((atom & 7) != 4 || atom < 8) return false;
    const uint32_t rep = atom & ~7u;
    const uint32_t chars = m.read<uint32_t>(rep + 0x10), wide = chars ? 0 : m.read<uint32_t>(rep + 4);
    if (!chars && !wide) return false;
    for (uint32_t k = 0;; k++) {
        const uint32_t ch = chars ? m.read<uint8_t>(chars + k) : m.read<uint32_t>(wide + 4 * k);
        if (ch != (uint8_t)want[k]) return false;
        if (!ch) return true;
    }
}

void movie_dispatch(Context& c) {
    const scimitar::GuestMemory& m = *c.mem;
    const uint32_t info = scimitar::arg32(c, 0);
    if (info && m.read<uint32_t>(info + 0x1c) == 0 && (int32_t)m.read<uint32_t>(info + 8) >= 2 && g_cursor_hidden()) {
        const uint32_t args = m.read<uint32_t>(info + 0xc);
        const uint32_t name = args ? m.read<uint32_t>(args + 4) : 0;
        if (args && atom_is(m, m.read<uint32_t>(args), "marker") &&
            (atom_is(m, name, "menuCursor") || atom_is(m, name, "cursor"))) {
            if (std::getenv("SCIMITAR_TRACE_UI")) game::log("ui: menu cursor left out\n");
            return scimitar::shim_return(c, 0);
        }
    }
    g_movie_dispatch(c);
}

void modifiers(Context& c, bool ctrl, bool alt, bool shift) {
    scimitar::call_guest(c, game_functions().update_modifiers, {ctrl ? 1u : 0u, alt ? 1u : 0u, shift ? 1u : 0u});
}

// ---- scripted keys ----

// SCIMITAR_KEYS="frame:keycode[:hold],...": scripted key presses (Mac virtual
// key codes, e.g. 36 Return, 53 Escape, 49 Space, 13 W), held for `hold`
// frames (default 10), for unattended runs.
struct ScriptedKey {
    uint64_t frame;
    uint16_t code;
    uint32_t hold = 10;
};
std::vector<ScriptedKey> scripted_keys() {
    std::vector<ScriptedKey> out;
    if (const char* s = std::getenv("SCIMITAR_KEYS")) {
        unsigned long long f;
        unsigned k;
        int n;
        while (std::sscanf(s, "%llu:%u%n", &f, &k, &n) == 2) {
            out.push_back({f, (uint16_t)k});
            s += n;
            unsigned hold;
            if (std::sscanf(s, ":%u%n", &hold, &n) == 1) out.back().hold = hold, s += n;
            if (*s == ',') s++;
        }
    }
    return out;
}

void press_scripted_keys(Context& c) {
    static std::vector<ScriptedKey> keys = scripted_keys();
    static size_t next = 0;
    static std::vector<ScriptedKey> held;
    const uint64_t now = g_frames.load();
    for (; next < keys.size() && keys[next].frame <= now; next++) {
        const uint16_t k = keys[next].code;
        game::log("frame %llu: key %u\n", (unsigned long long)now, k);
        if (k == kKeyF6) {
            toggle_stats();
            continue;
        }
        if (k == kKeyF13 && g_metal) {
            request_frame_dump();
            continue;
        }
        if (k == 36) scimitar::call_guest(c, game_functions().handle_char, {13u});
        modifiers(c, false, false, false);
        scimitar::call_guest(c, game_functions().handle_key_down, {(uint32_t)k});
        scimitar::hid_keyboard_event(hid_usage(k), true);
        g_keys_used = true;
        held.push_back({now + keys[next].hold, k});
    }
    for (size_t i = 0; i < held.size();)
        if (held[i].frame <= now) {
            scimitar::call_guest(c, game_functions().handle_key_up, {(uint32_t)held[i].code});
            scimitar::hid_keyboard_event(hid_usage(held[i].code), false);
            held.erase(held.begin() + i);
        } else {
            i++;
        }
}

// ---- game pads ----

// SCIMITAR_PAD="frame:control[:hold],...": scripted pad input, held for `hold`
// frames (default 10); setting it at all (SCIMITAR_PAD=1) gives the game a pad
// even with no controller connected. Controls: a b x y lb rb lt rt ls rs start
// back guide up down left right (buttons, triggers fully pulled, d-pad), and
// the sticks fully pushed: lup ldown lleft lright rup rdown rleft rright.
struct ScriptedPad {
    uint64_t frame;
    std::string control;
    uint32_t hold = 10;
};
std::vector<ScriptedPad> scripted_pad() {
    std::vector<ScriptedPad> out;
    const char* s = std::getenv("SCIMITAR_PAD");
    while (s && *s) {
        unsigned long long f;
        char control[16];
        unsigned hold;
        int n = 0;
        if (std::sscanf(s, "%llu:%15[a-z]%n", &f, control, &n) == 2) {
            out.push_back({f, control});
            s += n;
            if (std::sscanf(s, ":%u%n", &hold, &n) == 1) out.back().hold = hold, s += n;
        }
        const char* comma = std::strchr(s, ',');
        s = comma ? comma + 1 : nullptr;
    }
    return out;
}

// Applies one scripted control to the pad state; false if it is not one.
bool apply_control(scimitar::GamepadState& p, const std::string& k) {
    using B = scimitar::GamepadState;
    static const std::pair<const char*, uint32_t> kButtons[] = {
        {"a", B::A}, {"b", B::B}, {"x", B::X}, {"y", B::Y}, {"lb", B::LeftShoulder}, {"rb", B::RightShoulder},
        {"ls", B::LeftThumb}, {"rs", B::RightThumb}, {"start", B::Start}, {"back", B::Back}, {"guide", B::Guide},
        {"up", B::Up}, {"down", B::Down}, {"left", B::Left}, {"right", B::Right}};
    for (const auto& [name, bit] : kButtons)
        if (k == name) return p.buttons |= bit, true;
    if (k == "lt") return p.left_trigger = 1, true;
    if (k == "rt") return p.right_trigger = 1, true;
    if (k == "lup") return p.left_y = 1, true;
    if (k == "ldown") return p.left_y = -1, true;
    if (k == "lleft") return p.left_x = -1, true;
    if (k == "lright") return p.left_x = 1, true;
    if (k == "rup") return p.right_y = 1, true;
    if (k == "rdown") return p.right_y = -1, true;
    if (k == "rleft") return p.right_x = -1, true;
    if (k == "rright") return p.right_x = 1, true;
    return false;
}

GCController* first_gamepad() {
    for (GCController* p in GCController.controllers)
        if (p.extendedGamepad) return p;
    return nil;
}

// The pad probe (asked by iokit.cpp when the game looks for pads): controllers
// that are already connected appear a moment after launch, so wait for one:
// up to half a second on the Mac's main thread, running its run loop; up to
// two on iOS, where the game's thread waits while UIKit connects them.
std::string probe_gamepad() {
    @autoreleasepool {
        GCController.shouldMonitorBackgroundEvents = YES;  // read the pad even when our window lost focus
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(TARGET_OS_IPHONE ? 2000 : 500);
        while (!first_gamepad() && !g_virtual_pad_present.load() && std::chrono::steady_clock::now() < until) {
            if (pthread_main_np()) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, true);
            else std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        GCController* p = first_gamepad();
        if (!p) return g_virtual_pad_present.load() ? std::string("On-screen Controller") : std::string();
        NSString* name = p.vendorName ?: @"Game Controller";
        if (std::getenv("SCIMITAR_TRACE_INPUT")) game::log("controller \"%s\"\n", name.UTF8String);
        return name.UTF8String;
    }
}
const bool g_probe_set = (scimitar::set_gamepad_probe(probe_gamepad), true);

// Once per frame: the controller's state and the scripted controls to the pad.
// Returns whether the pad is in use (a button, or a stick or trigger past half).
bool poll_gamepad() {
    static std::vector<ScriptedPad> script = scripted_pad();
    static size_t next = 0;
    static std::vector<ScriptedPad> held;
    static bool had_controller = false;
    // SCIMITAR_BUTTON_PROMPTS=playstation or xbox (or SCIMITAR_PAD_KIND): the
    // button art, whatever the controller.
    static const char* forced_kind = []() -> const char* {
        const char* k = std::getenv("SCIMITAR_BUTTON_PROMPTS");
        if (!k || (std::strcmp(k, "playstation") != 0 && std::strcmp(k, "xbox") != 0)) k = std::getenv("SCIMITAR_PAD_KIND");
        if (k) scimitar::set_gamepad_kind(std::strcmp(k, "playstation") == 0 ? scimitar::GamepadKind::PlayStation
                                                                             : scimitar::GamepadKind::Xbox);
        return k;
    }();
    const uint64_t now = g_frames.load();
    scimitar::GamepadState p;
    @autoreleasepool {
        GCController* pad = first_gamepad();
        if (std::getenv("SCIMITAR_TRACE_INPUT") && (pad != nil) != had_controller)
            game::log("controller %s\n", pad ? "connected" : "disconnected");
        had_controller = pad != nil;
        if (GCExtendedGamepad* g = pad.extendedGamepad) {
            // PlayStation controllers get the PS3 button art, all others the Xbox 360 one.
            const bool playstation = [pad.productCategory containsString:@"DualShock"] ||
                                     [pad.productCategory containsString:@"DualSense"];
            if (!forced_kind)
                scimitar::set_gamepad_kind(playstation ? scimitar::GamepadKind::PlayStation : scimitar::GamepadKind::Xbox);
            using B = scimitar::GamepadState;
            p.left_x = g.leftThumbstick.xAxis.value, p.left_y = g.leftThumbstick.yAxis.value;
            p.right_x = g.rightThumbstick.xAxis.value, p.right_y = g.rightThumbstick.yAxis.value;
            p.left_trigger = g.leftTrigger.value, p.right_trigger = g.rightTrigger.value;
            const std::pair<GCControllerButtonInput*, uint32_t> buttons[] = {
                {g.buttonA, B::A}, {g.buttonB, B::B}, {g.buttonX, B::X}, {g.buttonY, B::Y},
                {g.leftShoulder, B::LeftShoulder}, {g.rightShoulder, B::RightShoulder},
                {g.leftThumbstickButton, B::LeftThumb}, {g.rightThumbstickButton, B::RightThumb},
                {g.buttonMenu, B::Start}, {g.buttonOptions, B::Back}, {g.buttonHome, B::Guide},
                {g.dpad.up, B::Up}, {g.dpad.down, B::Down}, {g.dpad.left, B::Left}, {g.dpad.right, B::Right}};
            for (const auto& [input, bit] : buttons)
                if (input.pressed) p.buttons |= bit;  // nil inputs (no Home button, ...) read as released
        }
    }
    // The on-screen pad (iOS): its buttons, and its sticks and triggers where they go further than a controller's.
    p.buttons |= g_virtual_buttons.load();
    const auto stronger = [](float a, float b) { return std::fabs(b) > std::fabs(a) ? b : a; };
    p.left_x = stronger(p.left_x, g_virtual_axis[VLeftX].load());
    p.left_y = stronger(p.left_y, g_virtual_axis[VLeftY].load());
    p.right_x = stronger(p.right_x, g_virtual_axis[VRightX].load());
    p.right_y = stronger(p.right_y, g_virtual_axis[VRightY].load());
    p.left_trigger = stronger(p.left_trigger, g_virtual_axis[VLeftTrigger].load());
    p.right_trigger = stronger(p.right_trigger, g_virtual_axis[VRightTrigger].load());
    for (; next < script.size() && script[next].frame <= now; next++) {
        game::log("frame %llu: pad %s\n", (unsigned long long)now, script[next].control.c_str());
        held.push_back({now + script[next].hold, script[next].control});
    }
    for (size_t i = 0; i < held.size();)
        if (held[i].frame <= now) held.erase(held.begin() + i);
        else apply_control(p, held[i++].control);
    scimitar::hid_gamepad_state(p);
    const auto far = [](float v) { return std::fabs(v) > 0.5f; };
    return p.buttons || far(p.left_x) || far(p.left_y) || far(p.right_x) || far(p.right_y) || far(p.left_trigger) ||
           far(p.right_trigger);
}

// The game reads only its selected input (PadProxyMac: keyboard and mouse
// profiles in slots 0-4, pads in 5-7, each slot 0x420 bytes; at the game's
// pad_proxy_selected (AC2 +0x54c) the selected index, then per slot +8 the Pad,
// +12 its type, 2 for pads)
// and selects a pad only from the options or at startup by the name saved in
// Assassin2.ini. Here the pad is selected from the first frame on when there is
// one, and whenever it is used; a key or a click goes back to the keyboard
// profile it replaced.
void switch_input(Context& c, bool pad_used) {
    static bool started = false;
    static uint32_t keyboard_slot = 0;
    const bool keys_used = g_keys_used.exchange(false);
    if (g_frames.load() == 0) return;  // the game reads its saved choice before the first frame
    const scimitar::GuestMemory& m = *c.mem;
    const uint32_t manager = m.read<uint32_t>(game_functions().pad_manager);
    const uint32_t data = manager ? m.read<uint32_t>(manager + 4) : 0;
    const uint32_t proxy = data ? m.read<uint32_t>(data) : 0;
    if (!proxy) return;
    const uint32_t sel = game::kInfo.pad_proxy_selected;
    uint32_t pad_slot = 0;
    for (uint32_t k = 5; k < 8 && !pad_slot; k++)
        if (m.read<uint32_t>(proxy + sel + 8 + 0x420 * k) && m.read<uint32_t>(proxy + sel + 12 + 0x420 * k) == 2) pad_slot = k;
    if (!pad_slot) return;
    const uint32_t selected = m.read<uint32_t>(proxy + sel);
    scimitar::set_gamepad_selected(selected == pad_slot);  // also follows choices made in the options
    uint32_t want = selected;
    if ((pad_used || !started) && selected != pad_slot) keyboard_slot = selected < 5 ? selected : 0, want = pad_slot;
    else if (keys_used && !pad_used && selected == pad_slot) want = keyboard_slot;
    started = true;
    if (want == selected) return;
    if (std::getenv("SCIMITAR_TRACE_INPUT"))
        game::log("input: %s (slot %u)\n", want == pad_slot ? "pad" : "keyboard and mouse", want);
    scimitar::call_guest(c, game_functions().select_pad, {manager, want});
    scimitar::set_gamepad_selected(want == pad_slot);
}

}  // namespace

const GameFunctions& game_functions() {
    static const GameFunctions f = [] {
        using scimitar::guest_symbol;
        GameFunctions g;
        g.handle_char = guest_symbol("_MacHandleChar");
        g.update_modifiers = guest_symbol("_MacUpdateKeyModifiersState");
        g.handle_key_down = guest_symbol("_MacHandleKeyDown");
        g.handle_key_up = guest_symbol("_MacHandleKeyUp");
        g.set_active = guest_symbol("_MacSetApplicationActive");
        g.write_bindings = guest_symbol("__ZN8scimitar22MAC_WriteInputBindingsEv");
        g.write_config = guest_symbol("__ZN8scimitar19MAC_WriteConfigFileEv");
        g.select_pad = guest_symbol("__ZN8scimitar16PadManagerNative9SelectPadEm");
        g.pad_manager = guest_symbol("__ZN8scimitar10PadManager13ms_PadManagerE");
        return g;
    }();
    return f;
}

uint32_t hid_usage(uint16_t key) { return key < 128 ? kHidUsage[key] : 0; }

uint16_t mac_key(uint32_t usage) {
    for (uint16_t k = 0; k < 128; k++)
        if (kHidUsage[k] && kHidUsage[k] == usage) return k;
    return 0xffff;
}

void key_down(Context& c, uint16_t key, uint16_t ch, bool ctrl, bool alt, bool shift, bool repeat) {
    if (ch) scimitar::call_guest(c, game_functions().handle_char, {(uint32_t)ch});
    modifiers(c, ctrl, alt, shift);
    scimitar::call_guest(c, game_functions().handle_key_down, {(uint32_t)key});
    if (!repeat) scimitar::hid_keyboard_event(hid_usage(key), true);
    g_keys_used = true;
    if (std::getenv("SCIMITAR_TRACE_INPUT")) game::log("key %u down\n", key);
}

void key_up(Context& c, uint16_t key, bool ctrl, bool alt, bool shift) {
    modifiers(c, ctrl, alt, shift);
    scimitar::call_guest(c, game_functions().handle_key_up, {(uint32_t)key});
    scimitar::hid_keyboard_event(hid_usage(key), false);
}

void request_mode(bool fullscreen, int width, int height) {
    g_mode.want_fullscreen = fullscreen;
    g_mode.want_w = width;
    g_mode.want_h = height;
    g_mode.pending = true;
    if (std::getenv("SCIMITAR_TRACE_GL"))
        game::log("mode requested: %dx%d%s\n", width, height, fullscreen ? " full screen" : "");
}

// A hidden run (tests, benchmarks) takes no mouse input: its window is on
// screen, and a pointer passing over it must not move the camera.
void mouse_motion(float dx, float dy, float dz) {
    if (g_hidden) return;
    std::lock_guard<std::mutex> l(g_mouse.lock);
    g_mouse.dx += dx, g_mouse.dy += dy, g_mouse.dz += dz;
}

void mouse_button(uint32_t number, bool down) {
    if (g_hidden) return;
    if (down) g_keys_used = true;
    std::lock_guard<std::mutex> l(g_mouse.lock);
    g_mouse.buttons.push_back({number, down});
}

void touch(Touch phase, float x, float y) {
    if (phase == Touch::Began) g_keys_used = true;
    std::lock_guard<std::mutex> l(g_mouse.lock);
    g_mouse.touched = true, g_mouse.tx = x, g_mouse.ty = y;
    if (phase != Touch::Moved) g_mouse.buttons.push_back({0, phase == Touch::Began});
}

void hide_ui_cursor(bool (*hidden)()) {
    const uint32_t at = scimitar::guest_symbol_or_0("__Z17MovieDispatchProcP10NativeInfo");
    auto* entry = at ? const_cast<scimitar::FuncEntry*>(scimitar::find_function(at)) : nullptr;
    g_cursor_hidden = hidden;
    if (!entry || g_movie_dispatch) return;
    g_movie_dispatch = entry->fn;
    entry->fn = movie_dispatch;
}

void pump_input(Context& c) {
    press_scripted_keys(c);
    switch_input(c, poll_gamepad());
}

// SCIMITAR_DUMP_SWF=dir (with SCIMITAR_EXIT_FRAME): at the exit, every Flash
// movie in guest memory to dir/swf_<address>.swf, for reading the UI's
// ActionScript. The UI's own movies stay loaded with "UEF" where an
// uncompressed SWF has "FWS" (then the version and the length, as in a SWF);
// they are written with "FWS". The Fire player's built-in ones have "FWS".
void dump_swf(scimitar::GuestMemory& m, const char* dir) {
    int n = 0;
    for (const auto& [start, len] : m.committed()) {
        const uint8_t* p = m.ptr(start);
        for (uint64_t k = 0; k + 8 <= len; k++) {
            const bool uef = p[k] == 'U' && p[k + 1] == 'E' && p[k + 2] == 'F';
            if (!(uef || (p[k] == 'F' && p[k + 1] == 'W' && p[k + 2] == 'S')) || p[k + 3] < 6 || p[k + 3] > 10) continue;
            uint32_t size;
            std::memcpy(&size, p + k + 4, 4);
            if (size < 64 || size > (64u << 20) || k + size > len) continue;
            char path[1024];
            std::snprintf(path, sizeof path, "%s/swf_%08x.swf", dir, (unsigned)(start + k));
            if (FILE* f = std::fopen(path, "wb")) {
                std::fwrite("FWS", 1, 3, f);
                std::fwrite(p + k + 3, 1, size - 3, f);
                std::fclose(f);
                n++;
            }
        }
    }
    game::log("dumped %d movies to %s\n", n, dir);
}

void count_frame(const std::function<bool(int*, int*, std::vector<uint8_t>*)>& read_pixels) {
    static uint64_t frames = 0;
    static const char* shots = std::getenv("SCIMITAR_SCREENSHOT");
    frames++;
    g_frames = frames;
    // SCIMITAR_BENCH=FIRST-LAST: frame times from FIRST to LAST, then the average
    // and percentiles, and exit.
    {
        static uint64_t bench_first = 0, bench_last = 0;
        static bool bench_init = false;
        static std::vector<double> times;
        static auto prev = std::chrono::steady_clock::now();
        if (!bench_init) {
            bench_init = true;
            if (const char* b = std::getenv("SCIMITAR_BENCH"))
                std::sscanf(b, "%llu-%llu", (unsigned long long*)&bench_first, (unsigned long long*)&bench_last);
        }
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double, std::milli>(now - prev).count();
        if (bench_last && frames > bench_first && frames <= bench_last) times.push_back(dt);
        prev = now;
        // SCIMITAR_HITCH_MS=N: every frame slower than N ms, with the time the
        // Metal backend spent compiling shaders and building pipelines in it.
        static const double hitch_ms = std::getenv("SCIMITAR_HITCH_MS") ? std::atof(std::getenv("SCIMITAR_HITCH_MS")) : 0;
        if (hitch_ms > 0) {
            static double last_compile = 0;
            const double compile = g_metal ? scimitar::metal::compile_ms() : 0;
            if (dt > hitch_ms)
                game::log("hitch: frame %llu took %.1f ms (%.1f ms compiling)\n", (unsigned long long)frames, dt,
                          compile - last_compile);
            last_compile = compile;
        }
        if (bench_last && frames == bench_last) {
            std::vector<double> t = times;
            std::sort(t.begin(), t.end());
            double sum = 0;
            for (double x : t) sum += x;
            auto pct = [&](double p) { return t.empty() ? 0.0 : t[std::min(t.size() - 1, (size_t)(p * t.size()))]; };
            std::fprintf(stderr, "bench: frames %llu-%llu: %.1f fps avg, frame ms p50 %.2f p95 %.2f p99 %.2f max %.2f\n",
                         (unsigned long long)bench_first, (unsigned long long)bench_last,
                         t.empty() ? 0.0 : 1000.0 * t.size() / sum, pct(0.5), pct(0.95), pct(0.99), t.empty() ? 0.0 : t.back());
            std::_Exit(0);
        }
    }
    static auto last = std::chrono::steady_clock::now();  // progress line timing
    static const uint64_t exit_frame = std::getenv("SCIMITAR_EXIT_FRAME") ? std::strtoull(std::getenv("SCIMITAR_EXIT_FRAME"), nullptr, 0) : 0;
    if (exit_frame && frames >= exit_frame) {
        if (const char* dir = std::getenv("SCIMITAR_DUMP_SWF"); dir && g_pump_context) dump_swf(*g_pump_context->mem, dir);
        game::log("exiting after %llu frames (SCIMITAR_EXIT_FRAME)\n", (unsigned long long)frames);
        std::_Exit(0);
    }
    scimitar::gl_frame_presented(frames);
    if (frames % 600 == 0) {
        const auto now = std::chrono::steady_clock::now();
        game::log("%llu frames (%.0f fps)\n", (unsigned long long)frames,
                     600 / std::chrono::duration<double>(now - last).count());
#if TARGET_OS_IPHONE
        {  // the memory iOS lets us grow by before it ends the app
            task_vm_info_data_t vm{};
            mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
            task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vm, &count);
            game::log("memory: %llu MB footprint, %llu MB more allowed\n", (unsigned long long)(vm.phys_footprint >> 20),
                      (unsigned long long)(os_proc_available_memory() >> 20));
        }
#endif
        last = now;
        if (g_metal) scimitar::metal::trace_frame();
        else scimitar::gl_trace_frame();
    }
    // With SCIMITAR_SCREENSHOT set to a directory, frames 60, 300, 900 and every
    // 1800th (or SCIMITAR_SCREENSHOT_EVERY) are saved there (frame_N.ppm).
    static const uint64_t every = std::getenv("SCIMITAR_SCREENSHOT_EVERY") ? std::strtoull(std::getenv("SCIMITAR_SCREENSHOT_EVERY"), nullptr, 0) : 1800;
    static const uint64_t shots_from = std::getenv("SCIMITAR_SCREENSHOT_FROM") ? std::strtoull(std::getenv("SCIMITAR_SCREENSHOT_FROM"), nullptr, 0) : 0;
    if (shots && frames >= shots_from &&
        ((!shots_from && (frames == 60 || frames == 300 || frames == 900)) || (every && frames % every == 0))) {
        int w = g_mode.have_w, h = g_mode.have_h;
        std::vector<uint8_t> px((size_t)w * h * 3);
        read_pixels(&w, &h, &px);
        char path[1024];
        std::snprintf(path, sizeof path, "%s/frame_%llu.ppm", shots, (unsigned long long)frames);
        if (FILE* f = std::fopen(path, "wb")) {
            std::fprintf(f, "P6\n%d %d\n255\n", w, h);
            for (int y = h - 1; y >= 0; y--) std::fwrite(px.data() + (size_t)y * w * 3, 1, (size_t)w * 3, f);
            std::fclose(f);
        }
    }
}

// Frame rate from presented frames, CPU from the process's user and system
// time (100% = one core), memory from the task (footprint as Activity Monitor
// shows it, and resident), and the guest heap (live blocks, high-water mark,
// size).
std::string stats_text(bool force) {
    using Clock = std::chrono::steady_clock;
    static Clock::time_point last = Clock::now();
    static uint64_t last_frames = g_frames;
    static double last_cpu = 0;
    const auto now = Clock::now();
    const double dt = std::chrono::duration<double>(now - last).count();
    if (!force && dt < 0.5) return {};
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    const double cpu = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
    const uint64_t frames = g_frames;
    const double fps = dt > 0 ? (frames - last_frames) / dt : 0;
    const double cpu_pct = dt > 0 && last_cpu > 0 ? (cpu - last_cpu) / dt * 100 : 0;
    last = now, last_frames = frames, last_cpu = cpu;

    task_vm_info_data_t vm{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vm, &count);
    const scimitar::heap::Usage h = scimitar::heap::usage();
    const double MB = 1024.0 * 1024.0;
    char text[512];
    int n = std::snprintf(text, sizeof text,
                          "FPS  %5.1f   %5.2f ms\n"
                          "CPU  %5.0f%%   (%d cores)\n"
                          "RAM  %5.0f MB footprint, %.0f MB resident\n"
                          "Heap %5.0f MB in use, %.0f MB peak, %.0f MB max",
                          fps, fps > 0 ? 1000 / fps : 0.0, cpu_pct, (int)NSProcessInfo.processInfo.activeProcessorCount,
                          vm.phys_footprint / MB, vm.resident_size / MB, h.in_use / MB, h.carved / MB, h.size / MB);
#if TARGET_OS_IPHONE
    if (n > 0 && n < (int)sizeof text)  // the memory iOS lets us grow by before it ends the app
        std::snprintf(text + n, sizeof text - n, "\nFree %5.0f MB to iOS's limit", os_proc_available_memory() / MB);
#endif
    return text;
}

PerfInfo perf_info() {
    using Clock = std::chrono::steady_clock;
    static Clock::time_point last = Clock::now();
    static uint64_t last_frames = g_frames;
    const auto now = Clock::now();
    const double dt = std::chrono::duration<double>(now - last).count();
    PerfInfo p;
    if (dt < 0.5) return p;
    const uint64_t frames = g_frames;
    p.fresh = true;
    p.fps = (frames - last_frames) / dt;
    last = now, last_frames = frames;
    task_vm_info_data_t vm{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vm, &count);
    p.ram_mb = vm.phys_footprint / (1024.0 * 1024.0);
    p.thermal = (int)NSProcessInfo.processInfo.thermalState;
    return p;
}

void request_frame_dump() {
    scimitar::metal::request_dump();
    show_notice("Capturing frame…", 10);
}

// AppKit's exit() would run static destructors under the game's running
// threads, which crashed; the rest of the original's shutdown (unloading the
// world) is skipped: it would run on this thread while the render thread draws.
void quit() {
    static bool quitting = false;
    if (quitting) return;
    quitting = true;
    if (g_pump_context) {
        scimitar::call_guest(*g_pump_context, game_functions().write_bindings, {});
        scimitar::call_guest(*g_pump_context, game_functions().write_config, {});
    }
    std::fflush(stdout);
    game::log("quit\n");
    std::_Exit(0);
}

}  // namespace shell

namespace scimitar::overrides {

using scimitar::arg32;
using scimitar::Context;
using scimitar::shim_return;
using namespace shell;

// void MacGetMouseState(float* dx, float* dy, float* dz, unsigned short max,
//                       unsigned* buttons, unsigned* pressed, unsigned short* count)
// Motion since the last call, and up to `max` button events (button number,
// and 1 for a press). A touch places the UI's cursor first, so that the game
// sends its movies the move before the press.
void ov_MacGetMouseState(Context& c) {
    std::lock_guard<std::mutex> l(g_mouse.lock);
    if (g_mouse.touched) place_ui_cursor(c, g_mouse.tx, g_mouse.ty), g_mouse.touched = false;
    c.mem->write<float>(arg32(c, 0), g_mouse.dx);
    c.mem->write<float>(arg32(c, 1), g_mouse.dy);
    c.mem->write<float>(arg32(c, 2), g_mouse.dz);
    g_mouse.dx = g_mouse.dy = g_mouse.dz = 0;
    const uint32_t max = arg32(c, 3) & 0xffff;
    uint32_t n = 0;
    for (; n < g_mouse.buttons.size() && n < max; n++) {
        c.mem->write<uint32_t>(arg32(c, 4) + 4 * n, g_mouse.buttons[n].number);
        c.mem->write<uint32_t>(arg32(c, 5) + 4 * n, g_mouse.buttons[n].down ? 1 : 0);
    }
    g_mouse.buttons.erase(g_mouse.buttons.begin(), g_mouse.buttons.begin() + n);
    if (arg32(c, 6)) c.mem->write<uint16_t>(arg32(c, 6), (uint16_t)n);
    shim_return(c, 0);
}

// void MacScheduleSwitchMode(bool fullscreen, int width, int height, int)
void ov_MacScheduleSwitchMode(Context& c) {
    request_mode(arg32(c, 0) & 0xff, (int)arg32(c, 1), (int)arg32(c, 2));
    shim_return(c, 0);
}

}  // namespace scimitar::overrides
