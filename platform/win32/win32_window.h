#pragma once
// The window and input state shared by USER32 (user32.mm), DirectInput and
// Direct3D: the game's window as a Cocoa view, and what the keyboard and mouse
// are doing (fed from Cocoa's events as PeekMessageA pumps them).
#include <cstdint>

#ifdef __OBJC__
@class NSView;
#else
typedef struct objc_object NSView;
#endif

namespace scimitar {
struct Context;
}

namespace scimitar::win32 {

NSView* window_view(uint32_t hwnd);  // the window's view (a CAMetalLayer's), or the first window's
void window_size(uint32_t hwnd, int* w, int* h);
// The window's size changed as Windows changes it for a full-screen device
// (CreateDevice, Reset): its rectangles follow, and the game gets WM_SIZE.
void window_resized(Context& c, uint32_t hwnd, int w, int h);
// Cmd-Q or the close button: WM_CLOSE to the game, then the process ends.
void request_quit();

// Input, from Cocoa events (Mac virtual key codes, NSEventType values).
void key_event(unsigned short mac_keycode, bool down);
void mouse_event(int ns_event_type, double dx, double dy);
void mouse_wheel(double dy);
// The cursor in the window's client coordinates (GetCursorPos; the window is
// the whole screen as the game sees it).
void cursor_position(int* x, int* y);
void set_cursor_position(int x, int y);
// A key pressed or released as if typed in the window (scripted input):
// DirectInput's state, GetKeyboardState and WM_KEYDOWN/WM_KEYUP.
void window_key(unsigned short mac_keycode, bool down);
// The game pad (xinput.mm), in XInput's terms: false when none is connected
// (no controller and no SCIMITAR_PAD script).
struct PadState {
    uint16_t buttons = 0;  // XINPUT_GAMEPAD_* bits
    uint8_t lt = 0, rt = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};
bool pad_state(PadState* out);

// Called once per presented frame: plays SCIMITAR_KEYS and SCIMITAR_PAD.
void input_frame(uint64_t frame);
uint64_t presented_frames();  // the last frame input_frame saw (the F6 overlay's frame rate)
void pad_frame(uint64_t frame);

// DirectInput's view: DIK scan codes (256 bytes, 0x80 = down), and the mouse's
// motion since the last read with its buttons.
void keyboard_state(uint8_t out[256]);
struct MouseState {
    int32_t dx, dy, dz;
    uint8_t buttons[4];
};
MouseState take_mouse_state();

}  // namespace scimitar::win32
