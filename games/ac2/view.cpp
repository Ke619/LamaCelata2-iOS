// AC2's view aspect, natively (scimitar-recomp --override).
//
// Every scimitar::View starts with the aspect type Aspect_16x9 (the
// constructor stores 2 at +0x58; nothing in the options or Assassin2.ini
// changes it, and the cutscene camera copies it from the gameplay view), and
// View::UpdateViewportSize fits a 16:9 viewport inside the render target,
// centred: on any other screen shape the rest stays black (bars at the sides
// of an iPhone, above and below on an iPad or a 16:10 Mac). Aspect_Full, which
// the engine has but never uses, makes the viewport the whole target, and the
// projection follows the viewport's own width and height (ComputeAspectRatio).
//
// The UI (Flash movies laid out on a 1280x720 stage) is scaled by the view's
// width / 1280 from the top left (scimitar::FireRenderer), and the HUD's icons
// over people are placed at their screen position x 1280 and x 720
// (Hud*Icon::SendUpdateEvent). Both read 1280 and 720 from two constants of
// the game's data; filling the screen, those become the screen's size in stage
// units at the scale that fits the 1280x720 stage (min of width / 1280, height
// / 720): the UI is never cut off (a phone is wider, an iPad taller than 16:9),
// and the icons stay over their people.
//
// SCIMITAR_ASPECT=full (the default) treats Aspect_16x9 as Aspect_Full;
// SCIMITAR_ASPECT=16:9 keeps the original's bars.
#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "scimitar/runtime.h"
#include "shell_common.h"

#pragma clang fp contract(off)

namespace {

// DisplayOptions::AspectRatio (g_DisplayOptions_AspectRatio_EnumItems).
enum Aspect : uint32_t { kFull = 0, k4x3 = 1, k16x9 = 2, k16x9In4x3 = 3, kCustom = 4 };

constexpr uint32_t kStageWidth = 0x0206400c;   // float 1280 (__TEXT,__const)
constexpr uint32_t kStageHeight = 0x020620ac;  // float 720

// The UI's stage as the screen w x h shows it, in stage units.
void set_stage(scimitar::GuestMemory& m, float w, float h) {
    const float scale = std::min(w / 1280.0f, h / 720.0f);
    m.write<float>(kStageWidth, w / scale);
    m.write<float>(kStageHeight, h / scale);
}

bool fill_screen() {
    static const bool full = [] {
        const char* v = std::getenv("SCIMITAR_ASPECT");
        return !(v && std::strcmp(v, "16:9") == 0);
    }();
    return full;
}

}  // namespace

namespace scimitar::overrides {

// void scimitar::View::UpdateViewportSize(): the viewport, as fractions of the
// render target (+0x178; its width and height floats at +0xc, +0x10), into
// +0x64 x, +0x68 y, +0x6c width, +0x70 height. Aspect_Custom leaves it alone.
// The arithmetic is the original's, in its order.
void ov__ZN8scimitar4View18UpdateViewportSizeEv(scimitar::Context& c) {
    GuestMemory& m = *c.mem;
    const uint32_t self = arg32(c, 0);
    uint32_t type = m.read<uint32_t>(self + 0x58);
    const uint32_t target = m.read<uint32_t>(self + 0x178);
    if (type == kCustom || !target) return shim_return(c, 0);
    if (type == k16x9 && fill_screen()) type = kFull;
    const float w = m.read<float>(target + 0xc), h = m.read<float>(target + 0x10);
    // The main view (rendering at the display mode's size) sets the UI's stage.
    if (type == kFull && fill_screen() && (int)w == shell::g_mode.have_w && (int)h == shell::g_mode.have_h)
        set_stage(m, w, h);
    float vw = w, vh = h;
    if (type == k16x9) {
        const float fit = w * 0.5625f;
        if (h > fit) vh = fit;
        else vw = h * 1.77777779f;
    } else if (type == k4x3 || type == k16x9In4x3) {
        const float fit = w * 0.75f;
        if (h > fit) vh = fit;
        else vw = h * 1.33333337f;
    }
    m.write<float>(self + 0x64, (w - vw) * 0.5f / w);
    m.write<float>(self + 0x68, (h - vh) * 0.5f / h);
    m.write<float>(self + 0x6c, vw / w);
    m.write<float>(self + 0x70, vh / h);
    shim_return(c, 0);
}

}  // namespace scimitar::overrides
