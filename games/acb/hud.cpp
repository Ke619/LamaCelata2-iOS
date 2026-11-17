// Brotherhood's HUD on screens that are not 16:9: where its movies belong
// (games/common/hud.cpp moves them there).
//
// Brotherhood's UI renderer fits the 1280x720 stage to the view and centres
// it, so the room is shared between both sides of a wider screen, or above
// and below a taller one, and the HUD moves left and up as well as right and
// down. Its Flash player draws nothing that starts outside the 1280x720 stage
// (AC2's does), so every component moves with its movie's origin, none by its
// clip. FireItem::Render draws nothing while the item's byte +0x82 is set.
#include <algorithm>

#include "hud.h"
#include "shell_common.h"

namespace acb {
bool fill_screen();  // view.cpp
}

namespace {

void room(scimitar::GuestMemory&, float* w, float* h) {
    const float sw = (float)shell::g_mode.have_w, sh = (float)shell::g_mode.have_h;
    *w = *h = 0;
    if (!acb::fill_screen() || sw <= 0 || sh <= 0) return;
    const float scale = std::min(sw / 1280.0f, sh / 720.0f);
    *w = std::max(0.0f, sw / scale - 1280.0f);
    *h = std::max(0.0f, sh / scale - 720.0f);
}

}  // namespace

namespace hud {

const Layout kLayout = {
    {
        {"Fire::HudHealthBar", {{kTopLeft}}},
        // The icons over people stay in the centred stage (view.cpp maps them
        // there, UIUtils::OffsetToLowerLeft).
        {"Fire::HudIconManager", {{kCentre}}},
        {"Fire::HudMiniMap", {{kCentre, {}, kBottomRight}}},
        // The Flash player draws nothing that starts outside the stage, so
        // each component is drawn from its own corner rather than moved
        // there by its clip.
        {"Fire::MultiComponentShowingFireItem",
         {{kBottomLeft, {{"HUD_Money", kBottomLeft}, {"HUD_Equipment", kBottomLeft}}},
          {kBottom, {{"HUD_Subtitles", kBottom}}},
          {kTopRight, {{"HUD_Controls", kTopRight}}},
          {kTopLeft, {{"HUD_Party", kTopLeft}}},
          {kCentre, {{"mcLogo", kCentre}}}}},
    },
    kCentre,
    0x82,
    room,
};

}  // namespace hud
