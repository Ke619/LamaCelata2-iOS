# games/ac2

Assassin's Creed II (Mac 1.0.1). What only AC2 has is here: `game.cpp` (its
`game::Info`: names, paths, the PadProxyMac offset), `native_math.cpp` and
`native_gfx.cpp` (native render-thread code, below), `pc_data.cpp` (running on the
Windows release's data, [docs/PC_DATA.md](../../docs/PC_DATA.md)), `view.cpp`, `hud.cpp`, `language.cpp` (text and voices in different languages, every installed
voice language in the options),
and the iOS build (`ios/`). The build also makes `build/AC2.app`, which opens
with the launcher, the game's settings before it starts
([../common/launcher/README.md](../common/launcher/README.md)). The port's entry point, shells and overrides are shared
with Brotherhood in `games/common` (below), and the top-level CMakeLists builds
it with `scimitar_add_game(ac2 ...)`, which lists the functions AC2 overrides.
The game's location is the CMake variable `AC2_APP`; at run time `$AC2_APP` or a
`.app` passed as the first argument.

`games/common/shell.mm` replaces the game's Cocoa layer on the Mac, and
`shell_ios.mm` on iOS
(UIKit and Metal, see [ios/README.md](ios/README.md)); what both share (game pads,
scripted input, mouse state, frame bookkeeping, statistics, quitting) is in
`shell_common.mm`. Overridden functions are named after their symbol
(`scimitar::overrides::ov_MacCreateWindow`), and the game functions the shell
calls are found by symbol in the loaded image (`scimitar::guest_symbol`). The Mac shell replaces (the `Mac*` functions listed in
`SCIMITAR_SHELL_OVERRIDES` and AC2's `scimitar_add_game` in the top-level `CMakeLists.txt`): the application and its
menu, the window (macOS's native full screen in a Space of its own, toggled with
Cmd-Return or Ctrl-Cmd-F; the window is resizable and the GL view letterboxed to the
mode's aspect; `SCIMITAR_WINDOWED=1` starts in a window), the OpenGL context and its
surface size per display mode (with `SCIMITAR_RENDERER=metal`, which the app sets, a
CAMetalLayer the Metal backend presents into: [docs/METAL.md](../../docs/METAL.md)), the event pump (keys to the virtual HID keyboard and
to the game's text handlers, mouse deltas and buttons for `MacGetMouseState`),
activation (`MacSetApplicationActive`), mouse capture while active
(`SCIMITAR_NO_MOUSE_CAPTURE=1` turns it off), presenting, and the F6 statistics
overlay (frame rate and frame time, process CPU with 100% = one core, memory
footprint and resident size, guest heap in use, peak and maximum;
`SCIMITAR_STATS=1` starts with it shown), the F4 graphics menu
(`games/common/graphics_menu.cpp`: anisotropic filtering, draw distance,
character detail, environment and shadow distances, shadow map size, changed
live with the arrow keys; the launcher's Graphics section sets them from the
start; `SCIMITAR_MENU_KEYS="FRAME:KEYS;..."` drives it in unattended runs: M opens or
closes it, U D L R are the arrows, P logs the current values), and game pads: the first controller the
GameController framework offers as an extended gamepad (Xbox, PlayStation, MFi...)
drives the virtual HID pad in `platform/src/iokit.cpp`, polled once per frame. It also
carries the switches for unattended runs (`SCIMITAR_KEYS`, `SCIMITAR_PAD`,
`SCIMITAR_EXIT_FRAME`, `SCIMITAR_SCREENSHOT`), see docs/PLAN.md.

`games/common/overrides.cpp` holds the other replaced functions: Orbit's library loader;
`io_kit::Device::vendorId`/`productId`/`product`, which the Mac build never fills in
(every pad would be a generic one, with no shipped bindings): for the virtual pad they
report the Xbox 360 controller (vendor 0x045e, product 0x028e), which the game maps
with its built-in layout (pad type 6), and the controller's name; and
`GetPlatformName`, so that the in-game HUD, whose movies are the consoles' too, draws
the controller's buttons (the UI's `PLATFORM` variable is "360" or "PS3" for the HUD
and the memory start screens while the pad is the selected input; the menus keep
"PC", which keeps Exit to Desktop and the PC options).

`native_math.cpp` replaces hot math of the render thread with native code that
computes the same bits (`native_gfx.cpp` does the same for the shader-constant
uploads, `update_shader_constants` for `SetVSCEnv` and `SetPSCEnv`, through
`games/common/native_constants.cpp`):
- `CullPlaneAABBoxRes`;
- `Gear::Matrix44::operator*=`;
- `Gear::Quaternion4::ToMatrix`;
- `IsConstantEqual<Vector4>`;
- `IColor::ConvertFromFloat`.

`tests/difftest/native_test.cpp` (`ctest -R native_math`, needs the game) runs each
against the original x86 in Unicorn.

`view.cpp` replaces `View::UpdateViewportSize` so the game fills screens that are
not 16:9. Every view is created with the aspect type `Aspect_16x9`, which fits a
16:9 viewport inside the render target and leaves the rest black (bars on 16:10
Macs, the iPad and the iPhone); nothing in the options changes it. With the
replacement, `Aspect_16x9` behaves as the engine's unused `Aspect_Full`: the
viewport is the whole target, and the projection follows its shape (the
vertical field of view is kept, so wider screens see more to the sides).
`SCIMITAR_ASPECT=16:9` keeps the original bars. The videos (Bink, drawn over the
whole viewport by `OGLVideoPlayer::ShowFrame`) stay 16:9 with black bars around
them: `games/common/intro.cpp` wraps `OGLGraphicDevice::SetViewport` for them. The UI, Flash movies laid out on a
1280x720 stage, is then scaled to fit the screen from its top left; `view.cpp`
sets the two constants the UI's scale and the icons over people are computed from.

`games/common/hud.cpp` (a replacement of `FireItem::Render`, shared with Brotherhood;
AC2's layout is `hud.cpp` here) puts the HUD at the screen's edges
on screens of other shapes: each movie, or each component of the HUD's master
movie (`HUD_Money`, `HUD_Controls`, ...), moves to its anchor, by its player's
viewport origin, by its clip's `_x`/`_y` (set as ActionScript's `setProperty` sets
them), or, for the minimap, whose map picture the game draws itself, by shifting
its draws on the screen (Metal; on GL the minimap keeps the fitted place). Menus
are centred. `SCIMITAR_TRACE_UI=1` prints the movies and their anchors;
`SCIMITAR_EXTRA_MODE=WxH` offers one more display mode, to try other shapes on a
Mac.

Playing with a controller: connect it (USB or Bluetooth) before starting `build/ac2`;
the game looks for pads once, at startup. The shell then makes it the selected input
from the first frame, and again whenever it is used; a key press or a click goes back
to the keyboard and mouse profile (the options still list both). The layout is the
Xbox 360 one: A legs, X weapon hand, Y head, B empty hand, RT high profile, d-pad, both
sticks, LB/RB/LT, Start, Back. The HUD shows Xbox buttons, or PlayStation ones for a
DualShock or DualSense (`SCIMITAR_BUTTON_PROMPTS=playstation` or `xbox` forces
either, `keyboard` keeps the keyboard's; `SCIMITAR_PAD_KIND` is the older name); it
picks them when a game loads, so after switching between pad and keyboard the prompts
follow at the next load.
