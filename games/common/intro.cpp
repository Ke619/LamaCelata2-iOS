// The videos: those before the title, skipped on request (SCIMITAR_SKIP_INTRO=1,
// the launcher's "Skip intro videos"), and every video's picture kept 16:9 on
// screens of other shapes.
#include <cstdlib>
#include <cstring>

#include "game.h"
#include "scimitar/image_loader.h"
#include "scimitar/runtime.h"

namespace scimitar::overrides {

// void StartupVideoManager::Start() (main): starts the videos before the title,
// the Ubisoft logo and the disclaimer (a video in the text's language).
// SCIMITAR_SKIP_INTRO=1 skips them: the sequence is done at once
// (OnVideoSequenceDone: controllers unlocked, the done callback, IsDonePlaying
// true), and nothing is loaded. Otherwise as the original, whose fields differ
// between the games: both keep a global object's pointer and its +0x14 (AC2 at
// +0x28, Brotherhood at +0x2c), loaded through a non-lazy pointer.
void ov__ZN8scimitar19StartupVideoManager5StartEv(scimitar::Context& c) {
    const uint32_t self = scimitar::arg32(c, 0);
    const bool acb = std::strcmp(game::kInfo.id, "acb") == 0;
    const auto fn = [](const char* name) { return scimitar::guest_symbol(name); };
    c.mem->write<uint8_t>(self + 0x27, 0);  // not done
    static const bool skip = [] {
        const char* v = std::getenv("SCIMITAR_SKIP_INTRO");
        return v && *v && std::strcmp(v, "0") != 0;
    }();
    if (skip) {
        game::log("skipping the intro videos (SCIMITAR_SKIP_INTRO)\n");
        scimitar::call_guest(c, fn("__ZN8scimitar19StartupVideoManager19OnVideoSequenceDoneEv"), {self});
        return scimitar::shim_return(c, 0);
    }
    const uint32_t global = c.mem->read<uint32_t>(c.mem->read<uint32_t>(acb ? 0x2c67930 : 0x278f9b4));
    const uint32_t at = acb ? 0x2c : 0x28;
    c.mem->write<uint32_t>(self + at, global);
    c.mem->write<uint32_t>(self + at + 4, c.mem->read<uint32_t>(global + 0x14));
    scimitar::call_guest(c, fn("__ZN8scimitar19StartupVideoManager17LoadVideoInMemoryEv"), {self});
    scimitar::call_guest(c, fn("__ZN8scimitar19StartupVideoManager14StartNextVideoEv"), {self});
    if (!acb) scimitar::call_guest(c, fn("__ZN8scimitar19StartupVideoManager15LockControllersEb"), {self, 1});
    scimitar::shim_return(c, 0);
}

}  // namespace scimitar::overrides

namespace {

scimitar::GuestFn g_set_viewport;  // the original OGLGraphicDevice::SetViewport(x, y, w, h)
uint32_t g_show_frame;             // OGLVideoPlayer::ShowFrame

// OGLGraphicDevice::SetViewport(unsigned long x, y, w, h), virtual. The
// videos (OGLVideoPlayer::ShowFrame) clear the screen, set the viewport to the
// view's (the whole screen, view.cpp) and draw the frame over all of it
// (DrawScreenQuad, -1..1): stretched on a screen that is not 16:9. Asked
// from ShowFrame, the viewport is the largest 16:9 one in the middle of it
// instead, with the cleared screen's black around it, as the original drew
// them inside its 16:9 view.
void set_viewport(scimitar::Context& c) {
    const uint32_t from = c.mem->read<uint32_t>(c.esp);
    if (g_show_frame && from > g_show_frame && from < g_show_frame + 0x800) {
        const uint32_t args = c.esp + 8;  // after the return address and `this`
        int64_t x = c.mem->read<uint32_t>(args), y = c.mem->read<uint32_t>(args + 4);
        int64_t w = c.mem->read<uint32_t>(args + 8), h = c.mem->read<uint32_t>(args + 12);
        if (w > 0 && h > 0) {
            if (w * 9 > h * 16) {  // wider: bars at the sides
                const int64_t fit = h * 16 / 9;
                x += (w - fit) / 2, w = fit;
            } else if (w * 9 < h * 16) {  // taller: bars above and below
                const int64_t fit = w * 9 / 16;
                y += (h - fit) / 2, h = fit;
            }
            c.mem->write<uint32_t>(args, (uint32_t)x);
            c.mem->write<uint32_t>(args + 4, (uint32_t)y);
            c.mem->write<uint32_t>(args + 8, (uint32_t)w);
            c.mem->write<uint32_t>(args + 12, (uint32_t)h);
        }
    }
    g_set_viewport(c);
}

}  // namespace

void game::install_video_fit() {
    const char* name = "__ZN8scimitar16OGLGraphicDevice11SetViewportEmmmm";
    const uint32_t addr = scimitar::guest_symbol_or_0(name);
    auto* entry = addr ? const_cast<scimitar::FuncEntry*>(scimitar::find_function(addr)) : nullptr;
    g_show_frame = scimitar::guest_symbol_or_0("__ZN8scimitar14OGLVideoPlayer9ShowFrameEv");
    if (!entry || !g_show_frame) return game::log("videos: %s is not translated\n", name);
    g_set_viewport = entry->fn;
    entry->fn = set_viewport;
}
