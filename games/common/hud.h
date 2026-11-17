// The HUD on screens that are not 16:9 (hud.cpp, a replacement of
// scimitar::FireItem::Render): what each game says about its own HUD
// (games/<id>/hud.cpp defines hud::kLayout).
#pragma once
#include <cstdint>
#include <optional>
#include <vector>

#include "scimitar/runtime.h"

namespace hud {

// Where something belongs on the screen: the share of the room beside the
// 1280x720 stage it sits at (0 left/top, 0.5 centre, 1 right/bottom).
struct Anchor {
    float x, y;
};
constexpr Anchor kTopLeft{0, 0}, kTop{0.5f, 0}, kTopRight{1, 0}, kLeft{0, 0.5f}, kCentre{0.5f, 0.5f},
    kRight{1, 0.5f}, kBottomLeft{0, 1}, kBottom{0.5f, 1}, kBottomRight{1, 1};

struct Clip {
    const char* name;  // a clip of the movie's root
    Anchor anchor;
};
// One drawing of a movie: from where (its viewport origin), with its clips
// moved to their anchors; with `draw_at`, everything it draws is moved on the
// screen to there instead (the minimap, whose map the game draws itself). With
// more than one pass, each shows only its own clips.
struct Pass {
    Anchor origin;
    std::vector<Clip> clips = {};
    std::optional<Anchor> draw_at = {};
};
struct Movie {
    const char* name;  // the FireItem's name: "Fire::" and the movie's
    std::vector<Pass> passes;
};

struct Layout {
    // The HUD's movies; the others (menus, messages, panels) stay centred.
    std::vector<Movie> movies;
    // Where the game itself puts the stage in the room: AC2 at the top left,
    // Brotherhood centred.
    Anchor stage;
    // A byte of the FireItem that makes Render draw nothing (0: none).
    uint32_t hidden_flag;
    // The room beside the stage, in stage units, on the screen the main view
    // fills (0 x 0 on a 16:9 screen, or with the bars kept).
    void (*room)(scimitar::GuestMemory& m, float* w, float* h);
};
extern const Layout kLayout;

}  // namespace hud
