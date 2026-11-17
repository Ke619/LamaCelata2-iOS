// CoreGraphics display imports (the pre-10.6 dictionary API the game uses for
// monitors and modes). There is one display, the main one; it offers common
// modes up to its own size in pixels (a Retina display's native resolution
// too), as dictionaries with "Width", "Height",
// "BitsPerPixel", "RefreshRate" and the safe/stretched flags. The display mode
// is never really changed: the shell reports the game's switches
// (set_display_mode) and scales its GL surface instead. On iOS, where there is
// no CGDisplay API, the shell describes the screen (set_host_display).
//
// On a Mac with a camera housing (notch), full screen places the window below
// it, so the panel's own size is taller than the full-screen window and shows
// with bars at the sides. The shell reports the housing's height
// (set_display_safe_top) and the modes then include the area below it, in
// points and in pixels, which a first start picks and full screen fills.
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include <CoreGraphics/CoreGraphics.h>
#else
#include <ApplicationServices/ApplicationServices.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

#include "bridge.h"
#include "scimitar/imports.h"
#include "scimitar/runtime.h"

namespace scimitar::imports {

namespace {

#if TARGET_OS_IPHONE
constexpr uint32_t kMainDisplay = 1;
std::atomic<int> g_host_w{1920}, g_host_h{1080};
std::atomic<double> g_host_hz{60};
uint32_t main_display() { return kMainDisplay; }
std::pair<int, int> display_size() { return {g_host_w.load(), g_host_h.load()}; }
std::pair<int, int> display_pixels() { return display_size(); }
double refresh_rate() { return g_host_hz.load(); }
#else
std::atomic<int> g_safe_top{0};  // points above the area below the camera housing
uint32_t main_display() { return CGMainDisplayID(); }
// The desktop's size in points (on a Retina display half its pixels).
std::pair<int, int> display_size() {
    const CGDirectDisplayID d = CGMainDisplayID();
    return {(int)CGDisplayPixelsWide(d), (int)CGDisplayPixelsHigh(d)};
}
// The desktop mode's size in pixels: the panel's own resolution on a Retina
// display in its default scaling (3456x2234 for 1728x1117 points).
std::pair<int, int> display_pixels() {
    static const std::pair<int, int> px = [] {
        std::pair<int, int> r = display_size();
        if (CGDisplayModeRef m = CGDisplayCopyDisplayMode(CGMainDisplayID())) {
            const int w = (int)CGDisplayModeGetPixelWidth(m), h = (int)CGDisplayModeGetPixelHeight(m);
            if (w > r.first && h > r.second) r = {w, h};
            CGDisplayModeRelease(m);
        }
        return r;
    }();
    return px;
}
// The area below the camera housing (what full screen shows), in points and in
// pixels; {0, 0} on a display without one.
std::pair<int, int> safe_size() {
    const int top = g_safe_top.load();
    if (top <= 0) return {0, 0};
    const auto [w, h] = display_size();
    return {w, h - top};
}
std::pair<int, int> safe_pixels() {
    const int top = g_safe_top.load();
    if (top <= 0) return {0, 0};
    const auto [pw, ph] = display_pixels();
    const auto [w, h] = display_size();
    return {pw, ph - (int)std::lround((double)top * ph / h)};
}

// The main display's refresh rate (60 when it reports none). The game saves
// the rate with the resolution in Assassin2.ini and matches it, so a saved
// mode from another rate falls back to 800x600 until it is chosen again.
double refresh_rate() {
    static const double r = [] {
        double hz = 0;
        if (CGDisplayModeRef m = CGDisplayCopyDisplayMode(CGMainDisplayID())) {
            hz = CGDisplayModeGetRefreshRate(m);
            CGDisplayModeRelease(m);
        }
        return hz > 0 ? std::round(hz) : 60.0;
    }();
    return r;
}
#endif

uint32_t mode_dict(Context& c, int w, int h) {
    bridge::Object d;
    auto key = [&](const char* k) { return bridge::make_string(c, k); };
    d.pairs.emplace_back(key("Width"), bridge::make_number(c, w, true));
    d.pairs.emplace_back(key("Height"), bridge::make_number(c, h, true));
    d.pairs.emplace_back(key("BitsPerPixel"), bridge::make_number(c, 32, true));
    d.pairs.emplace_back(key("BitsPerSample"), bridge::make_number(c, 8, true));
    d.pairs.emplace_back(key("SamplesPerPixel"), bridge::make_number(c, 3, true));
    d.pairs.emplace_back(key("RefreshRate"), bridge::make_number(c, refresh_rate(), false));
    d.pairs.emplace_back(key("Mode"), bridge::make_number(c, w * 10000 + h, true));
    // The engine offers only modes that are safe for the hardware and not stretched.
    d.pairs.emplace_back(key("kCGDisplayModeIsSafeForHardware"), bridge::make_boolean(c, true));
    d.pairs.emplace_back(key("kCGDisplayModeIsStretched"), bridge::make_boolean(c, false));
    d.pairs.emplace_back(key("kCGDisplayModeUsableForDesktopGUI"), bridge::make_boolean(c, true));
    return bridge::make(c, bridge::Kind::Dictionary, "NSDictionary", std::move(d));
}

// The modes offered, smallest first: common ones up to the display's pixels,
// the desktop's size in points, and on a Retina display its pixels (the last).
std::vector<std::pair<int, int>> modes() {
    const auto [W, H] = display_pixels();
#if TARGET_OS_IPHONE
    // The screen's own shape at fractions of its pixels, smallest first, the last its pixels: every
    // mode fills the screen (the usual 4:3 and 16:9 ones would be letterboxed on a phone).
    {
        std::vector<std::pair<int, int>> out;
        for (const double s : {0.5, 0.6, 0.7, 0.8, 0.9})
            out.emplace_back((int)std::lround(W * s / 2) * 2, (int)std::lround(H * s / 2) * 2);
        out.emplace_back(W, H);
        return out;
    }
#endif
    static const int kModes[][2] = {{800, 600}, {1024, 768}, {1280, 720}, {1280, 800}, {1280, 1024},
                                    {1440, 900}, {1600, 900}, {1680, 1050}, {1920, 1080}, {1920, 1200},
                                    {2560, 1440}, {2560, 1600}, {3840, 2160}};
    std::vector<std::pair<int, int>> out;
    for (const auto& m : kModes)
        if (m[0] <= W && m[1] <= H) out.emplace_back(m[0], m[1]);
    auto add = [&](std::pair<int, int> m) {
        if (std::find(out.begin(), out.end(), m) != out.end()) return;
        auto at = std::find_if(out.begin(), out.end(), [&](const auto& o) {
            return (long)o.first * o.second > (long)m.first * m.second;
        });
        out.insert(at, m);
    };
    add(display_size());
#if !TARGET_OS_IPHONE
    if (safe_size().first > 0) add(safe_size()), add(safe_pixels());
#endif
    // SCIMITAR_EXTRA_MODE=WxH: one more mode (tests of other screen shapes, such
    // as a phone's 2560x1178).
    static const std::pair<int, int> extra = [] {
        int w = 0, h = 0;
        if (const char* e = std::getenv("SCIMITAR_EXTRA_MODE")) std::sscanf(e, "%dx%d", &w, &h);
        return std::make_pair(w, h);
    }();
    if (extra.first > 0 && extra.second > 0) add(extra);
    std::erase(out, std::make_pair(W, H));
    out.emplace_back(W, H);
    return out;
}

// The current mode: the desktop's until the shell reports a switch.
std::mutex g_mode_lock;
int g_mode_w = 0, g_mode_h = 0;
uint32_t g_current = 0, g_current_w = 0, g_current_h = 0;

}  // namespace

// CGError CGGetActiveDisplayList(uint32_t max, CGDirectDisplayID* ids, uint32_t* count)
void imp_CGGetActiveDisplayList(Context& c) {
    if (arg32(c, 1) && arg32(c, 0)) c.mem->write<uint32_t>(arg32(c, 1), main_display());
    if (arg32(c, 2)) c.mem->write<uint32_t>(arg32(c, 2), 1);
    shim_return(c, 0);
}
void imp_CGMainDisplayID(Context& c) { shim_return(c, main_display()); }
// CFDictionaryRef CGDisplayCurrentMode(CGDirectDisplayID): the mode the game
// switched to, initially the desktop's size in points (a first start does not
// pick a Retina display's four times as many pixels), below the camera housing
// if there is one.
void imp_CGDisplayCurrentMode(Context& c) {
    std::lock_guard<std::mutex> l(g_mode_lock);
    auto m = display_size();
#if !TARGET_OS_IPHONE
    if (safe_size().first > 0) m = safe_size();
#endif
    if (g_mode_w) m = {g_mode_w, g_mode_h};
    if (!g_current || g_current_w != (uint32_t)m.first || g_current_h != (uint32_t)m.second) {
        g_current = mode_dict(c, m.first, m.second);
        g_current_w = m.first, g_current_h = m.second;
    }
    shim_return(c, g_current);
}
// CFArrayRef CGDisplayAvailableModes(CGDirectDisplayID)
void imp_CGDisplayAvailableModes(Context& c) {
    std::vector<uint32_t> items;
    for (const auto& m : modes()) items.push_back(mode_dict(c, m.first, m.second));
    shim_return(c, bridge::make_array(c, std::move(items)));
}
// CFDictionaryRef CGDisplayBestModeForParametersAndRefreshRate(CGDirectDisplayID, size_t bpp,
//     size_t width, size_t height, CGRefreshRate refresh (double), boolean_t* exact)
void imp_CGDisplayBestModeForParametersAndRefreshRate(Context& c) {
    const int w = (int)arg32(c, 2), h = (int)arg32(c, 3);
    std::pair<int, int> best = modes().back();
    for (const auto& m : modes())
        if (m.first >= w && m.second >= h) {
            best = m;
            break;
        }
    if (arg32(c, 6)) c.mem->write<uint32_t>(arg32(c, 6), best.first == w && best.second == h);
    shim_return(c, mode_dict(c, best.first, best.second));
}
// CFDictionaryRef CGDisplayBestModeForParameters(CGDirectDisplayID, size_t bpp,
//     size_t width, size_t height, boolean_t* exact)
void imp_CGDisplayBestModeForParameters(Context& c) {
    const int w = (int)arg32(c, 2), h = (int)arg32(c, 3);
    std::pair<int, int> best = modes().back();
    for (const auto& m : modes())
        if (m.first >= w && m.second >= h) {
            best = m;
            break;
        }
    if (arg32(c, 4)) c.mem->write<uint32_t>(arg32(c, 4), best.first == w && best.second == h);
    shim_return(c, mode_dict(c, best.first, best.second));
}
void imp_CGDisplaySwitchToMode(Context& c) { shim_return(c, 0); }
void imp_CGCaptureAllDisplays(Context& c) { shim_return(c, 0); }
void imp_CGReleaseAllDisplays(Context& c) { shim_return(c, 0); }
void imp_CGDisplayIsCaptured(Context& c) { shim_return(c, 0); }
#if TARGET_OS_IPHONE
void imp_CGDisplayHideCursor(Context& c) { shim_return(c, 0); }
void imp_CGDisplayShowCursor(Context& c) { shim_return(c, 0); }
#else
void imp_CGDisplayHideCursor(Context& c) { shim_return(c, (uint32_t)CGDisplayHideCursor(arg32(c, 0))); }
void imp_CGDisplayShowCursor(Context& c) { shim_return(c, (uint32_t)CGDisplayShowCursor(arg32(c, 0))); }
#endif
// CGFloat CGRectGetMidX(CGRect): CGRect by value (four 32-bit floats); result in ST0
void imp_CGRectGetMidX(Context& c) { shim_return_f32(c, arg_f32(c, 0) + arg_f32(c, 2) / 2); }
void imp_CGRectGetMidY(Context& c) { shim_return_f32(c, arg_f32(c, 1) + arg_f32(c, 3) / 2); }
// Mouse warping events (the original recentred the cursor this way): not posted.
void imp_CGEventCreateMouseEvent(Context& c) { shim_return(c, 0); }
void imp_CGEventCreateSourceFromEvent(Context& c) { shim_return(c, 0); }
void imp_CGEventPost(Context& c) { shim_return(c, 0); }

}  // namespace scimitar::imports

namespace scimitar {

#if TARGET_OS_IPHONE
void set_host_display(int width, int height, double refresh_hz) {
    imports::g_host_w = width, imports::g_host_h = height;
    imports::g_host_hz = refresh_hz > 0 ? std::round(refresh_hz) : 60.0;
}
#else
void set_display_safe_top(int points) { imports::g_safe_top = std::max(points, 0); }
#endif

void set_display_mode(int width, int height) {
    std::lock_guard<std::mutex> l(imports::g_mode_lock);
    imports::g_mode_w = width, imports::g_mode_h = height;
}

namespace {
using namespace imports;
#define S(N) {"_" #N, imp_##N},
const ImportShim kCG[] = {
    S(CGGetActiveDisplayList) S(CGMainDisplayID) S(CGDisplayCurrentMode) S(CGDisplayAvailableModes)
    S(CGDisplayBestModeForParametersAndRefreshRate) S(CGDisplayBestModeForParameters) S(CGDisplaySwitchToMode) S(CGCaptureAllDisplays)
    S(CGReleaseAllDisplays) S(CGDisplayIsCaptured) S(CGDisplayHideCursor) S(CGDisplayShowCursor)
    S(CGRectGetMidX) S(CGRectGetMidY) S(CGEventCreateMouseEvent) S(CGEventCreateSourceFromEvent)
    S(CGEventPost)
};
#undef S
SCIMITAR_REGISTER_SHIMS(kCG);
}  // namespace
}  // namespace scimitar
