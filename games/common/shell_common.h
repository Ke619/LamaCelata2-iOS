// What the port's two shells share (shell_common.mm): the game's input and
// shell functions, scripted input for unattended runs, game pads, the mouse
// state the game asks for, mode requests, frame bookkeeping and quitting. The
// Mac shell (shell.mm: AppKit, GL or Metal) and the iOS shell (shell_ios.mm:
// UIKit, Metal) each provide the hooks at the end.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "scimitar/runtime.h"

namespace shell {

using scimitar::Context;

// The game's input and shell functions, found by symbol in the loaded image.
struct GameFunctions {
    uint32_t handle_char;       // void MacHandleChar(unsigned short)
    uint32_t update_modifiers;  // void MacUpdateKeyModifiersState(bool ctrl, bool alt, bool shift)
    uint32_t handle_key_down;   // void MacHandleKeyDown(unsigned short)
    uint32_t handle_key_up;     // void MacHandleKeyUp(unsigned short)
    uint32_t set_active;        // void MacSetApplicationActive(bool)
    uint32_t write_bindings;    // void scimitar::MAC_WriteInputBindings()
    uint32_t write_config;      // void scimitar::MAC_WriteConfigFile()
    uint32_t select_pad;        // void scimitar::PadManagerNative::SelectPad(unsigned long)
    uint32_t pad_manager;       // scimitar::PadManager::ms_PadManager (the PadManager*)
};
const GameFunctions& game_functions();

constexpr uint16_t kKeyF4 = 118;   // the graphics menu (graphics_menu.h; it takes the arrow keys while open)
constexpr uint16_t kKeyF6 = 97;    // the statistics overlay (not passed to the game)
constexpr uint16_t kKeyF13 = 105;  // Metal: dump the next frame (platform/src/metal/dump.mm)

constexpr uint32_t kWindowHandle = 0x0a2c0001;  // what the game holds as its NSWindow*

extern std::atomic<uint64_t> g_frames;  // presented frames
extern std::atomic<bool> g_keys_used;   // a key or mouse button went down since the last pump
// The on-screen pad (iOS, touchpad_ios.mm), merged into the game's pad: buttons (scimitar::GamepadState::Button
// bits), sticks (-1..1, +y up) and triggers (0..1).
enum VirtualAxis { VLeftX, VLeftY, VRightX, VRightY, VLeftTrigger, VRightTrigger };
extern std::atomic<uint32_t> g_virtual_buttons;
extern std::atomic<float> g_virtual_axis[6];
extern std::atomic<bool> g_virtual_pad_present;  // the game finds a pad even with no controller connected
extern const bool g_hidden;             // SCIMITAR_HIDDEN
extern const bool g_metal;              // the Metal renderer
extern Context* g_pump_context;         // the event pump's guest context, for quitting

// Mac virtual key code -> HID keyboard usage (page 7), 0 for none; and back.
uint32_t hid_usage(uint16_t mac_key);
uint16_t mac_key(uint32_t hid_usage);  // 0xffff for none

// A key as the original delegate passed it to the game, and to the virtual HID
// keyboard (gameplay reads keys there). `ch` is the first UTF-16 unit typed (0
// for none); `repeat` keys only reach the text functions.
void key_down(Context& c, uint16_t mac_key, uint16_t ch, bool ctrl, bool alt, bool shift, bool repeat);
void key_up(Context& c, uint16_t mac_key, bool ctrl, bool alt, bool shift);

// Mouse motion and buttons, for MacGetMouseState (any thread).
void mouse_motion(float dx, float dy, float dz);
void mouse_button(uint32_t number, bool down);

// A touch on the game's picture (iOS), at x, y as fractions of its width and
// height: the UI's mouse cursor goes there (with the next MacGetMouseState),
// and the left button is down from its start to its end (any thread).
enum class Touch { Began, Moved, Ended };
void touch(Touch phase, float x, float y);

// The menus' own mouse cursor (the Flash clip they attach on a PC or Mac) is
// left out while `hidden` says so. Once the image is loaded, before the UI.
void hide_ui_cursor(bool (*hidden)());

// Once per pump: scripted keys and pad (SCIMITAR_KEYS, SCIMITAR_PAD), the game
// pad, and the switch between pad and keyboard input.
void pump_input(Context& c);

// The mode the game asked for (MacScheduleSwitchMode) and the one it has.
struct Mode {
    bool want_fullscreen = false;
    int want_w = 0, want_h = 0, have_w = 800, have_h = 600;
    bool pending = false;
};
extern Mode g_mode;
// A mode the game asked for (MacScheduleSwitchMode, MacSwitchModeOpenGL); the
// shell applies it at its switch_mode_if_needed.
void request_mode(bool fullscreen, int width, int height);

// MacUpdateOpenGL's bookkeeping before presenting: the frame count,
// SCIMITAR_BENCH, SCIMITAR_EXIT_FRAME, progress lines, and SCIMITAR_SCREENSHOT
// (read_pixels gives the frame as RGB rows, bottom row first).
void count_frame(const std::function<bool(int* w, int* h, std::vector<uint8_t>* rgb)>& read_pixels);

// The F6 overlay's text, at most twice a second (empty in between unless forced).
std::string stats_text(bool force);

// What the one-line display in the corner shows: frame rate, the phone's thermal state (0 nominal, 1 fair,
// 2 serious, 3 critical) and the memory the game uses (as Activity Monitor shows it). `fresh` is false until
// half a second has passed since the last fresh one.
struct PerfInfo {
    bool fresh = false;
    double fps = 0;
    int thermal = 0;
    double ram_mb = 0;
};
PerfInfo perf_info();

// F13 with Metal: dump the next frame, with a notice.
void request_frame_dump();

// Quitting from the shell, as the original's MacApplicationShutDown: the input
// bindings and the config file are written, then the process ends at once.
void quit();

// ---- provided by each shell ----
void toggle_stats();
void show_notice(const char* text, double seconds);

}  // namespace shell
