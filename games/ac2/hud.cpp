// AC2's HUD on screens that are not 16:9: where its movies belong
// (games/common/hud.cpp moves them there).
//
// view.cpp fits the 1280x720 stage to the screen from its top left and sets
// the two constants below to the screen's size in stage units, so the room is
// at the right of a wider screen and at the bottom of a taller one.
#include "hud.h"

namespace {

constexpr uint32_t kStageWidth = 0x0206400c;   // view.cpp: the screen's width in stage units
constexpr uint32_t kStageHeight = 0x020620ac;  // and height

void room(scimitar::GuestMemory& m, float* w, float* h) {
    *w = m.read<float>(kStageWidth) - 1280.0f;
    *h = m.read<float>(kStageHeight) - 720.0f;
}

}  // namespace

namespace hud {

const Layout kLayout = {
    {
        {"Fire::HudHealthBar", {{kTopLeft}}},
        {"Fire::HudIconManager", {{kTopLeft}}},
        {"Fire::HudMiniMap", {{kTopLeft, {}, kBottomRight}}},
        // The weapon in hand and the subtitles' text are pictures the game
        // draws: they follow the viewport origin, not their clips, so each is
        // drawn from its own origin.
        {"Fire::MultiComponentShowingFireItem",
         {{kBottomLeft, {{"HUD_Money", kBottomLeft}, {"HUD_Equipment", kBottomLeft}}},
          {kBottom, {{"HUD_Subtitles", kBottom}}},
          {kTopLeft, {{"HUD_Controls", kTopRight}, {"HUD_Party", kTopLeft}, {"mcLogo", kCentre}}}}},
    },
    kTopLeft,
    0,
    room,
};

}  // namespace hud
