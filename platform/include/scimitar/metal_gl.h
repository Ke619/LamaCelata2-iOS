#pragma once
// The Metal rendering backend (docs/METAL.md): the game's GL calls on Metal,
// selected with SCIMITAR_RENDERER=metal. The GL path (platform/src/opengl.cpp)
// stays the default.
//
// The GL import shims forward to the backend (platform/src/metal/shims.h); the
// shell gives the backend its layer and drives presentation.
#include <cstdint>
#include <string>
#include <vector>

#include "scimitar/guest_memory.h"

namespace scimitar::metal {

// SCIMITAR_RENDERER=metal.
bool requested();

// Shell side. `layer` is a CAMetalLayer*; the backend creates the device and
// queue and sets the layer up. width/height: the default framebuffer.
void attach_layer(void* layer, GuestMemory* mem, int width, int height);
// The game's mode size: the default framebuffer is resized (contents lost).
void set_default_size(int width, int height);
// The drawable's pixel size (the view's backing size), applied at the next present.
void set_drawable_size(int width, int height);
void set_vsync(bool on);
// iOS: at most this many frames a second (0: the display's own rate), from the next present.
void set_frame_cap(int fps);
// Ends the frame: the default framebuffer drawn into the layer, presented.
void present();
// The default framebuffer as RGB rows, bottom row first (screenshots).
bool read_default(int* width, int* height, std::vector<uint8_t>* rgb);
// Dumps the next frame's render passes and draws to /tmp/ac2dump/ (F13).
void request_dump();
// The directory of the last finished dump, once (empty if none since).
std::string take_dump_result();
// Counts of the frame for SCIMITAR_TRACE_GL (stderr).
void trace_frame();
// Moves what is drawn from now on by (dx, dy) pixels of the target (x right,
// y down), viewport and scissor alike; (0, 0) stops. For the UI: one movie of
// the HUD placed at another edge of the screen (games/ac2/hud.cpp).
void set_draw_shift(int dx, int dy);
// Anisotropic filtering forced on the linearly filtered, mipmapped textures,
// from the next draw on: 1 (none) to 16. SCIMITAR_ANISO=N sets it at start.
void set_forced_anisotropy(int level);
int forced_anisotropy();
// Total time spent compiling MSL and building pipelines so far (ms).
double compile_ms();

}  // namespace scimitar::metal
