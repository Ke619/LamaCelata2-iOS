#pragma once
// The virtual game pad (platform/src/iokit.cpp): one HID device the game finds
// as a joystick, laid out as the Xbox 360 controller the game knows (vendor
// 0x045e, product 0x028e), fed by the shell from a real controller or a script.
#include <cstdint>
#include <string>

namespace scimitar {

// Controls in the controller's own terms (GameController conventions).
struct GamepadState {
    float left_x = 0, left_y = 0, right_x = 0, right_y = 0;  // -1..1, +y is up
    float left_trigger = 0, right_trigger = 0;                // 0..1
    enum Button : uint32_t {
        A = 1u << 0, B = 1u << 1, X = 1u << 2, Y = 1u << 3,
        LeftShoulder = 1u << 4, RightShoulder = 1u << 5,
        LeftThumb = 1u << 6, RightThumb = 1u << 7,
        Start = 1u << 8, Back = 1u << 9, Guide = 1u << 10,
        Up = 1u << 11, Down = 1u << 12, Left = 1u << 13, Right = 1u << 14,
    };
    uint32_t buttons = 0;
};

// Asked once, when the game looks for joysticks (it never looks again): the
// name of the connected controller, or empty for none. Without a probe, or
// when it finds none, the game sees no pad unless forced (SCIMITAR_PAD).
using GamepadProbe = std::string (*)();
void set_gamepad_probe(GamepadProbe probe);

// The pad's current state; the game reads it the next time it polls the pad.
void hid_gamepad_state(const GamepadState& s);

// The virtual pad's IOHIDDevice handle, 0 if the game did not find one, and
// the name it reports.
uint32_t hid_gamepad_device();
std::string hid_gamepad_name();

// Which button art the game's UI should show while the pad is the selected
// input (set by the shell; read by the AC2 override of GetPlatformName).
enum class GamepadKind : uint32_t { Xbox, PlayStation };
void set_gamepad_kind(GamepadKind kind);
GamepadKind gamepad_kind();
void set_gamepad_selected(bool selected);  // the game reads the pad, not the keyboard and mouse
bool gamepad_selected();
constexpr uint16_t kGamepadVendor = 0x045e, kGamepadProduct = 0x028e;  // Microsoft Xbox 360 Controller

}  // namespace scimitar
