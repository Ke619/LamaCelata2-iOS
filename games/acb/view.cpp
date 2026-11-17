// Brotherhood's view aspect, natively (scimitar-recomp --override).
//
// As in AC2 (games/ac2/view.cpp), every scimitar::View is created with the
// aspect type Aspect_16x9 (+0x58), which fits a 16:9 viewport inside the
// render target and leaves the rest black; the cutscene camera
// (SplineCamera::ApplyScreenFraming) copies it from the gameplay view.
// Brotherhood computes the viewport in View::UpdateViewportSizeConst, which
// View::UpdateViewportSize and View::UpdateInternalConst call with the type
// and the rect to fill; replacing it, Aspect_16x9 behaves as Aspect_Full, the
// viewport is the whole target, and the projection follows its shape (the
// vertical field of view is kept).
//
// Unlike AC2's, Brotherhood's UI renderer (FireRenderer::Render*) already
// fits the 1280x720 Flash stage to the view's size and centres it, so the
// menus need nothing. The icons over people do: UIUtils::OffsetToLowerLeft
// turns a projected position (-1..1 over the viewport) into a fraction of the
// stage (0..1, y down), which the icons multiply by 1280 and 720; that holds
// only when the stage covers the viewport. Its replacement maps into the
// centred stage instead (so do the money and wealth animations, its other
// users).
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

// DisplayOptions::AspectRatio.
enum Aspect : uint32_t { kFull = 0, k4x3 = 1, k16x9 = 2, k16x9In4x3 = 3, kCustom = 4 };

}  // namespace

namespace acb {

bool fill_screen() {
    static const bool full = [] {
        const char* v = std::getenv("SCIMITAR_ASPECT");
        return !(v && std::strcmp(v, "16:9") == 0);
    }();
    return full;
}

}  // namespace acb

namespace {
using acb::fill_screen;
}  // namespace

namespace scimitar::overrides {

// void scimitar::View::UpdateViewportSizeConst(Rect& out, DisplayOptions::AspectRatio& type) const:
// the viewport, as fractions of the render target (+0x178; its width and
// height floats at +0xc, +0x10), into out's x, y, width, height. Aspect_Custom
// leaves it alone. The arithmetic is the original's, in its order.
void ov__ZNK8scimitar4View23UpdateViewportSizeConstERNS_4RectERNS_14DisplayOptions11AspectRatioE(
    scimitar::Context& c) {
    GuestMemory& m = *c.mem;
    const uint32_t self = arg32(c, 0), out = arg32(c, 1);
    uint32_t type = m.read<uint32_t>(arg32(c, 2));
    const uint32_t target = m.read<uint32_t>(self + 0x178);
    if (type == kCustom || !target) return shim_return(c, 0);
    if (type == k16x9 && fill_screen()) type = kFull;
    const float w = m.read<float>(target + 0xc), h = m.read<float>(target + 0x10);
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
    m.write<float>(out, (w - vw) * 0.5f / w);
    m.write<float>(out + 4, (h - vh) * 0.5f / h);
    m.write<float>(out + 8, vw / w);
    m.write<float>(out + 12, vh / h);
    shim_return(c, 0);
}

// void scimitar::ac2::UIUtils::OffsetToLowerLeft(Gear::Vector2<float>&): a
// projected position (x, y from -1 to 1, y up) as a fraction of the screen
// from its top left, here of the 1280x720 stage the UI renderer fits to the
// screen and centres (scale min(w / 1280, h / 720)). Icons beyond the stage
// (at the sides of a wider screen) are not drawn: Brotherhood's Flash player
// draws nothing that starts outside it.
void ov__ZN8scimitar3ac27UIUtils17OffsetToLowerLeftERN4Gear7Vector2IfEE(scimitar::Context& c) {
    GuestMemory& m = *c.mem;
    const uint32_t v = arg32(c, 0);
    float x = (m.read<float>(v) + 1.0f) * 0.5f;
    float y = (m.read<float>(v + 4) + 1.0f) * -0.5f + 1.0f;
    const float w = (float)shell::g_mode.have_w, h = (float)shell::g_mode.have_h;
    if (fill_screen() && w > 0 && h > 0) {
        const float scale = std::min(w / 1280.0f, h / 720.0f);
        x = 0.5f + (x - 0.5f) * (w / scale / 1280.0f);
        y = 0.5f + (y - 0.5f) * (h / scale / 720.0f);
    }
    m.write<float>(v, x);
    m.write<float>(v + 4, y);
    shim_return(c, 0);
}

}  // namespace scimitar::overrides
