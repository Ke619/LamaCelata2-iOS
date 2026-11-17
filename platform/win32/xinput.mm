// XInput 1.3: user 0 is the first controller the GameController framework
// sees (or a scripted one), as an Xbox 360 pad. SCIMITAR_PAD="frame:control[:hold],..."
// scripts it for unattended runs (controls: a b x y lb rb lt rt ls rs start
// back up down left right, and lup ldown lleft lright rup rdown rleft rright
// for the sticks; any value, e.g. "1", connects a pad without a controller),
// counted in presented frames like SCIMITAR_KEYS.
#import <GameController/GameController.h>

#include <dlfcn.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "win32.h"
#include "win32_window.h"

namespace scimitar::win32 {

namespace {

using Pad = PadState;

std::mutex g_lock;
Pad g_script;                 // what the script holds down now
std::atomic<uint32_t> g_packet{0};
const char* const g_script_env = std::getenv("SCIMITAR_PAD");

struct Press {
    uint64_t frame, hold;
    std::string control;
};

std::vector<Press> script() {
    std::vector<Press> v;
    for (const char* e = g_script_env; e && *e;) {
        char* end = nullptr;
        Press p{std::strtoull(e, &end, 10), 4, ""};
        if (*end != ':') break;
        const char* name = end + 1;
        const char* stop = name;
        while (*stop && *stop != ':' && *stop != ',') stop++;
        p.control.assign(name, stop);
        end = const_cast<char*>(stop);
        if (*end == ':') p.hold = std::strtoull(end + 1, &end, 10);
        v.push_back(p);
        e = *end == ',' ? end + 1 : end;
    }
    return v;
}

void apply(Pad& p, const std::string& c, bool on) {
    static const struct { const char* name; uint16_t bit; } kButtons[] = {
        {"up", 0x1}, {"down", 0x2}, {"left", 0x4}, {"right", 0x8}, {"start", 0x10}, {"back", 0x20},
        {"ls", 0x40}, {"rs", 0x80}, {"lb", 0x100}, {"rb", 0x200}, {"a", 0x1000}, {"b", 0x2000},
        {"x", 0x4000}, {"y", 0x8000}};
    for (const auto& b : kButtons)
        if (c == b.name) p.buttons = on ? p.buttons | b.bit : p.buttons & ~b.bit;
    const int16_t full = on ? 32767 : 0;
    if (c == "lt") p.lt = on ? 255 : 0;
    if (c == "rt") p.rt = on ? 255 : 0;
    if (c == "lup") p.ly = full;
    if (c == "ldown") p.ly = (int16_t)-full;
    if (c == "lright") p.lx = full;
    if (c == "lleft") p.lx = (int16_t)-full;
    if (c == "rup") p.ry = full;
    if (c == "rdown") p.ry = (int16_t)-full;
    if (c == "rright") p.rx = full;
    if (c == "rleft") p.rx = (int16_t)-full;
}

int16_t axis(float v) { return (int16_t)std::lround(std::fmax(-1.0f, std::fmin(1.0f, v)) * 32767.0f); }

// The first extended gamepad. GameController lives on the main thread, which
// reads it 250 times a second into a snapshot; the game's thread only copies
// the snapshot (a round trip to the main thread per poll slowed the game).
std::mutex g_pad_lock;
Pad g_pad;
bool g_pad_found = false;

Class gc_class() {
    // The framework is loaded on first use (not linked: the port's build
    // stays as it is).
    static Class gc = [] {
        dlopen("/System/Library/Frameworks/GameController.framework/GameController", RTLD_LAZY);
        return NSClassFromString(@"GCController");
    }();
    return gc;
}

// Main thread.
void read_controller() {
    bool found = false;
    Pad p;
    for (GCController* c in [gc_class() controllers]) {
        GCExtendedGamepad* g = c.extendedGamepad;
        if (!g) continue;
        found = true;
        const std::pair<GCControllerButtonInput*, uint16_t> buttons[] = {
            {g.dpad.up, 0x1}, {g.dpad.down, 0x2}, {g.dpad.left, 0x4}, {g.dpad.right, 0x8},
            {g.buttonMenu, 0x10}, {g.buttonOptions, 0x20}, {g.leftThumbstickButton, 0x40},
            {g.rightThumbstickButton, 0x80}, {g.leftShoulder, 0x100}, {g.rightShoulder, 0x200},
            {g.buttonA, 0x1000}, {g.buttonB, 0x2000}, {g.buttonX, 0x4000}, {g.buttonY, 0x8000}};
        for (const auto& [b, bit] : buttons)
            if (b.pressed) p.buttons |= bit;
        p.lt = (uint8_t)std::lround(g.leftTrigger.value * 255.0f);
        p.rt = (uint8_t)std::lround(g.rightTrigger.value * 255.0f);
        p.lx = axis(g.leftThumbstick.xAxis.value), p.ly = axis(g.leftThumbstick.yAxis.value);
        p.rx = axis(g.rightThumbstick.xAxis.value), p.ry = axis(g.rightThumbstick.yAxis.value);
        break;
    }
    std::lock_guard<std::mutex> l(g_pad_lock);
    g_pad = p, g_pad_found = found;
}

void on_main_sync(void (^block)(void)) {
    if ([NSThread isMainThread]) block();
    else dispatch_sync(dispatch_get_main_queue(), block);
}

bool controller(Pad* out) {
    if (!gc_class()) return false;
    // Controllers already connected appear a moment after launch, and the game
    // looks for pads once at startup (DirectInput's EnumDevices): the first
    // look waits up to half a second for one, as the Mac ports' shells do.
    static std::once_flag first;
    std::call_once(first, [] {
        on_main_sync(^{
            if ([gc_class() respondsToSelector:@selector(setShouldMonitorBackgroundEvents:)])
                [gc_class() setShouldMonitorBackgroundEvents:YES];  // read it when the window is not focused
        });
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        for (;;) {
            on_main_sync(^{ read_controller(); });
            {
                std::lock_guard<std::mutex> l(g_pad_lock);
                if (g_pad_found) break;
            }
            if (std::chrono::steady_clock::now() >= until) break;
            if ([NSThread isMainThread]) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, true);
            else std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        static dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
        dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 0), NSEC_PER_SEC / 250, NSEC_PER_MSEC);
        dispatch_source_set_event_handler(timer, ^{ read_controller(); });
        dispatch_resume(timer);
        std::lock_guard<std::mutex> l(g_pad_lock);
        trace("game controller: %s", g_pad_found ? "found" : "none");
    });
    std::lock_guard<std::mutex> l(g_pad_lock);
    *out = g_pad;
    return g_pad_found;
}

}  // namespace

void pad_frame(uint64_t frame) {
    if (!g_script_env) return;
    static const std::vector<Press> s = script();
    std::lock_guard<std::mutex> l(g_lock);
    for (const Press& p : s) {
        if (frame == p.frame) apply(g_script, p.control, true);
        if (frame == p.frame + p.hold) apply(g_script, p.control, false);
    }
}

bool pad_state(PadState* out) {
    Pad p;
    const bool real = controller(&p);
    if (!real && !g_script_env) return false;
    std::lock_guard<std::mutex> l(g_lock);
    p.buttons |= g_script.buttons;
    p.lt = std::max(p.lt, g_script.lt), p.rt = std::max(p.rt, g_script.rt);
    if (g_script.lx || g_script.ly) p.lx = g_script.lx, p.ly = g_script.ly;
    if (g_script.rx || g_script.ry) p.rx = g_script.rx, p.ry = g_script.ry;
    *out = p;
    return true;
}

}  // namespace scimitar::win32

namespace scimitar::imports {
using namespace scimitar::win32;

W32(xinput1_3, XInputGetState) {
    const uint32_t user = arg(c, 0), out = arg(c, 1);
    static std::atomic<bool> said{false};
    if (!said.exchange(true)) trace("XInputGetState(%u): first poll", user);
    Pad p;
    if (user != 0 || !pad_state(&p)) return ret(c, 1167, 2);  // ERROR_DEVICE_NOT_CONNECTED
    wr(out, ++g_packet);
    wr16(out + 4, p.buttons);
    gptr(out + 6)[0] = p.lt, gptr(out + 7)[0] = p.rt;
    wr16(out + 8, (uint16_t)p.lx), wr16(out + 10, (uint16_t)p.ly), wr16(out + 12, (uint16_t)p.rx),
        wr16(out + 14, (uint16_t)p.ry);
    ret(c, 0, 2);
}

W32(xinput1_3, XInputSetState) {
    // Vibration: accepted, not played.
    Pad p;
    const bool connected = arg(c, 0) == 0 && (g_script_env || controller(&p));
    ret(c, connected ? 0 : 1167, 2);
}

W32(xinput1_3, XInputGetCapabilities) {
    const uint32_t user = arg(c, 0), out = arg(c, 2);
    trace("XInputGetCapabilities(%u)", user);
    Pad p;
    if (user != 0 || (!g_script_env && !controller(&p))) return ret(c, 1167, 3);
    std::memset(gptr(out), 0, 20);
    gptr(out)[0] = 1;  // XINPUT_DEVTYPE_GAMEPAD
    gptr(out)[1] = 1;  // XINPUT_DEVSUBTYPE_GAMEPAD
    wr16(out + 4, 0xf3ff);  // every button
    gptr(out + 6)[0] = 255, gptr(out + 7)[0] = 255;
    wr16(out + 8, 0xffc0), wr16(out + 10, 0xffc0), wr16(out + 12, 0xffc0), wr16(out + 14, 0xffc0);
    wr16(out + 16, 0xffff), wr16(out + 18, 0xffff);
    ret(c, 0, 3);
}

}  // namespace scimitar::imports
