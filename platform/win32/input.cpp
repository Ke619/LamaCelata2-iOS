// Keyboard and mouse state for DirectInput and USER32 (win32_window.h).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

#include "win32_window.h"

namespace scimitar::win32 {

namespace {

std::mutex g_lock;
uint8_t g_dik[256];
double g_mx = 0, g_my = 0, g_mz = 0;
uint8_t g_buttons[4];
int g_cx = 640, g_cy = 360;

// Mac virtual key codes to DirectInput scan codes (DIK_*).
uint8_t dik_of(unsigned short k) {
    static const uint8_t kMap[128] = {
        0x1e, 0x1f, 0x20, 0x21, 0x23, 0x22, 0x2c, 0x2d, 0x2e, 0x2f, 0x56, 0x30, 0x10, 0x11, 0x12, 0x13,  // 0x00
        0x15, 0x14, 0x02, 0x03, 0x04, 0x05, 0x07, 0x06, 0x0d, 0x0a, 0x08, 0x0c, 0x09, 0x0b, 0x1b, 0x18,  // 0x10
        0x16, 0x1a, 0x17, 0x19, 0x1c, 0x26, 0x24, 0x28, 0x25, 0x27, 0x2b, 0x33, 0x35, 0x31, 0x32, 0x34,  // 0x20
        0x0f, 0x39, 0x29, 0x0e, 0, 0x01, 0xdc, 0xdb, 0x2a, 0x3a, 0x38, 0x1d, 0x36, 0xb8, 0x9d, 0,       // 0x30
        0, 0x53, 0, 0x37, 0, 0x4e, 0, 0x45, 0, 0, 0, 0xb5, 0x9c, 0, 0x4a, 0,                           // 0x40
        0, 0x8d, 0x52, 0x4f, 0x50, 0x51, 0x4b, 0x4c, 0x4d, 0x47, 0, 0x48, 0x49, 0, 0, 0,               // 0x50
        0x3f, 0x40, 0x41, 0x3d, 0x42, 0x43, 0, 0x57, 0, 0x64, 0, 0x65, 0, 0x44, 0, 0x58,               // 0x60
        0, 0x66, 0xd2, 0xc7, 0xc9, 0xd3, 0x3e, 0xcf, 0x3c, 0xd1, 0x3b, 0xcb, 0xcd, 0xd0, 0xc8, 0,      // 0x70
    };
    return k < 128 ? kMap[k] : 0;
}

}  // namespace

void key_event(unsigned short mac_keycode, bool down) {
    const uint8_t d = dik_of(mac_keycode);
    if (!d) return;
    std::lock_guard<std::mutex> l(g_lock);
    g_dik[d] = down ? 0x80 : 0;
}

void mouse_event(int type, double dx, double dy) {
    std::lock_guard<std::mutex> l(g_lock);
    switch (type) {
    case 1: g_buttons[0] = 0x80; break;  // NSEventTypeLeftMouseDown
    case 2: g_buttons[0] = 0; break;
    case 3: g_buttons[1] = 0x80; break;  // right
    case 4: g_buttons[1] = 0; break;
    default: break;
    }
    g_mx += dx, g_my += dy;
}

void set_cursor_position(int x, int y) {
    std::lock_guard<std::mutex> l(g_lock);
    g_cx = x, g_cy = y;
}

void mouse_wheel(double dy) {
    std::lock_guard<std::mutex> l(g_lock);
    g_mz += dy * 120;
}

void cursor_position(int* x, int* y) {
    std::lock_guard<std::mutex> l(g_lock);
    *x = g_cx, *y = g_cy;
}

void keyboard_state(uint8_t out[256]) {
    std::lock_guard<std::mutex> l(g_lock);
    for (int k = 0; k < 256; k++) out[k] = g_dik[k];
}

MouseState take_mouse_state() {
    std::lock_guard<std::mutex> l(g_lock);
    MouseState m{(int32_t)g_mx, (int32_t)g_my, (int32_t)g_mz, {g_buttons[0], g_buttons[1], g_buttons[2], g_buttons[3]}};
    g_mx -= m.dx, g_my -= m.dy, g_mz -= m.dz;
    return m;
}

// Scripted keys for unattended runs, as the Mac ports take them:
// SCIMITAR_KEYS="frame:keycode[:hold],..." (Mac key codes: 36 Return, 49
// Space, 53 Escape, 125 Down); a key is held `hold` frames (default 4).
namespace {
std::atomic<uint64_t> g_presented{0};
}
uint64_t presented_frames() { return g_presented.load(std::memory_order_relaxed); }

void input_frame(uint64_t frame) {
    g_presented.store(frame, std::memory_order_relaxed);
    // SCIMITAR_FPS_LOG=path: "seconds frame" every 300 frames, for timing runs.
    static FILE* fps_log = std::getenv("SCIMITAR_FPS_LOG") ? std::fopen(std::getenv("SCIMITAR_FPS_LOG"), "w") : nullptr;
    if (fps_log && frame % 300 == 0) {
        static const auto t0 = std::chrono::steady_clock::now();
        std::fprintf(fps_log, "%.3f %llu\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(),
                     (unsigned long long)frame);
        std::fflush(fps_log);
    }
    struct Press {
        uint64_t frame, hold;
        unsigned short key;
    };
    static const std::vector<Press> script = [] {
        std::vector<Press> v;
        const char* e = std::getenv("SCIMITAR_KEYS");
        while (e && *e) {
            char* end = nullptr;
            Press p{std::strtoull(e, &end, 10), 4, 0};
            if (*end != ':') break;
            p.key = (unsigned short)std::strtoul(end + 1, &end, 10);
            if (*end == ':') p.hold = std::strtoull(end + 1, &end, 10);
            v.push_back(p);
            e = *end == ',' ? end + 1 : end;
        }
        return v;
    }();
    for (const Press& p : script) {
        if (frame == p.frame) window_key(p.key, true);
        if (frame == p.frame + p.hold) window_key(p.key, false);
    }
}

}  // namespace scimitar::win32
