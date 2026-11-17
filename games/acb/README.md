# games/acb

Assassin's Creed Brotherhood (Mac, the executable of Steam depot 48193). The
port shares its entry point, shells and overrides with AC2 (`games/common`);
what is Brotherhood's own is `game.cpp` (its `game::Info`), `native_math.cpp`
and `native_gfx.cpp` (native render-thread code, below), `view.cpp` and
`hud.cpp` (screen shapes, below), `language.cpp` (text and voice languages
chosen outside the game, and every one installed offered in the options,
Russian text included, which the game's own list leaves out), and the overrides
listed in its `scimitar_add_game(acb ...)` in the top-level CMakeLists.

## Building and running

```bash
cmake -S . -B build -DSCIMITAR_FULL_IMAGE=ON \
      -DACB_APP="/path/to/Assassin's Creed Brotherhood.app"
cmake --build build --target acb -j 10
build/acb                                     # GL
SCIMITAR_RENDERER=metal build/acb             # Metal
```

`build/ACB.app` is the same port as an app that opens with the launcher (game
location, full screen or window, screen shape, graphics (also F4 while playing),
text and voice languages, button prompts, skipping the intro videos, the frame rate
overlay, game options; [../common/launcher/README.md](../common/launcher/README.md)).

The game is found at `ACB_APP` (the CMake variable), `$ACB_APP` at run time, or a
`.app` passed as the first argument. The switches for
unattended runs are AC2's (`SCIMITAR_KEYS`, `SCIMITAR_PAD`, `SCIMITAR_EXIT_FRAME`,
`SCIMITAR_SCREENSHOT`; docs/PLAN.md; `SCIMITAR_HIDDEN=1`: a transparent,
click-through window with no Dock icon; `SCIMITAR_MUTE=1`: no sound), and
`SCIMITAR_HITCH_MS=N` logs every frame slower than N ms (with Metal's shader
compile time in it).

## Where it stands

Checked by scripted runs and by the author: the Ubisoft logo video, the
disclaimer, the title, the main menu, the Uplay dialog (dismissed offline),
Story Mode, name entry, the "Previously on" recap, the story
from the start to the arrival in Rome (Monteriggioni and the siege; on the Mac
and on the iPhone), loading the author's saves into the
Animus room, keyboard and mouse, an Xbox controller and a DualSense with their
names and layout in the options and their buttons in the HUD, a GameSir USB-C pad
on the iPhone; on GL and on Metal. The iOS build: [../ac2/ios/README.md](../ac2/ios/README.md).
Open: occasional stutter at camera cuts in cutscenes (`SCIMITAR_HITCH_MS` logs
slow frames), the multiplayer menu entry (should report that the
multiplayer executable could not start), the developer options.

## What differs from AC2

- **The executable.** 246,517 functions (AC2 208,782). Its linker emits external
  relocations for every reference to a weak definition (52,024; its vtables are
  zeros in the file), which the loader applies. A few more instructions and jump
  table forms (unoptimized PIC code in curl and Quazal, Havok tables kept in a
  callee-saved register) are in `recomp/`.
- **Settings and saves.** The game keeps both itself, as files:
  `~/Library/Application Support/Ubisoft/Assassin's Creed Brotherhood/`
  (`ACBrotherhood.ini`, `SAVES/ACBROTHERHOODSAVEGAMEn.SAV`, `SAVES/OPTIONS`);
  saves do not go through Orbit.
- **Orbit.** The C++ client (`mg::orbitclient`) is not linked in but comes from
  `Contents/Frameworks/libubiorbitapi_r2_loader.dylib`, so its 21 methods are
  imports; `platform/src/orbit.cpp` implements them over the same 22 functions
  AC2's statically linked client calls. The game starts offline; the Uplay
  dialog says the rewards need a login.
- **The shell.** `MacSwitchModeOpenGL` switches the mode at once (AC2 schedules
  it); `MacLockFocusIfCanDraw`/`MacUnlockFocus` wrap drawing.
  `MacLaunchExecutable` (the multiplayer executable `acbmp_sf`, whose servers
  are gone), `MacCheckUpdaterLaunch` (the launcher's updater) and
  `MacLaunchBundle` start nothing and report so.
- **Input.** `io_kit::Device` opens one device (`initDevice`, the device at
  +0x14) instead of AC2's array; PadProxyMac's selected slot is at +0x580.
- **Hardware detection.** `HardwareDetection::GetGraphicsInformation` runs
  `system_profiler SPDisplaysDataType -xml` through `popen` and matches the card
  against a table of 2010 Mac GPUs, or else picks the settings profile by video
  memory. `popen` answers that command with this Mac's name and 2 GB, the high
  profile; without it the game warns of an unsupported card and uses the lowest
  settings.
- **Rendering.** APPLE vertex array objects and `glDrawBuffers` (colour
  attachments 1-3: the Metal backend draws them as extra attachments of the
  pass). The engine deletes a bound render target, generates the same texture
  name again and specifies it without binding it; Apple's GL lands that in the
  new texture, and the Metal backend now does too (otherwise the floor's planar
  reflection and the eyes sample an empty texture).
- **Screen shapes** (`view.cpp`). As in AC2, the views' `Aspect_16x9` type
  behaves as `Aspect_Full`, so the picture fills screens that are not 16:9 and
  the vertical field of view is kept (`SCIMITAR_ASPECT=16:9` keeps the bars);
  Brotherhood computes it in `View::UpdateViewportSizeConst`. Its UI renderer
  already fits the 1280x720 stage to the view and centres it, so the UI needs
  no stage constants; `UIUtils::OffsetToLowerLeft` maps the icons over people
  (and the money animations) into that centred stage. Checked at 1728x1117 and
  2560x1080 (`SCIMITAR_EXTRA_MODE`). The HUD goes to the screen's edges with
  AC2's replacement of `FireItem::Render` (`games/common/hud.cpp`; Brotherhood's
  layout is `hud.cpp` here). Its Flash player draws nothing that starts outside
  the 1280x720 stage (AC2's does), so each HUD component is drawn from its own
  corner instead of being moved there by its clip, and icons over people at the
  sides of a wider screen (outside the centred stage) are not drawn.
  The videos stay 16:9 with black bars, as in AC2 (`games/common/intro.cpp`).
- **Native hot code** (`native_math.cpp`, `native_gfx.cpp`): the same functions as
  AC2's, written from Brotherhood's own code, which is compiled differently (other
  instruction order and constants); `ctest -R native_math_acb` checks them against
  the original x86 in Unicorn.

## `Contents/Resources/scimitar.ini`

A file the retail game never reads: neither its name nor its keys
(`COMMANDLINE`, `BFLOCATION`) occur in `scimitar_sf`, and a traced start opens
only `ACBrotherhood.ini`. It is Ubisoft's development configuration, shipped by
accident: default command lines per platform (`[XENON]`, `[PS3]`, `[PC]`,
`[TOOL]`; no `[MAC]`), a debug server share path, and an online test account.
Its interest is the list of the engine's developer options for Brotherhood,
which the port passes to the engine's own parser the same way as for AC2
(`build/acb /name:value ...`, docs/AC2_DEV_OPTIONS.md); whether each still works
in the retail Mac build is untested:

| Option | What its name says |
|---|---|
| `/debugmenu:on` | the developer menu |
| `/unlockall` | everything unlocked |
| `/dlclocal` | downloadable content from local files |
| `/world:NAME`, `/mapmenu` | load a world directly (`ACR_Rome_Multi`, `Map_Menu_AC2MP`...); tried: `build/acb /nobink /world:ACR_Rome` starts in Rome, as in AC2 |
| `/fast`, `/fardist:1200`, `/shadows:on`, `/postfx:on`, `/msaa:none`, `/skipmips:1` | rendering |
| `/pausetimeofday`, `/initialtimeofday:15`, `/lightmode:Normal` | time of day |
| `/VersionInfo`, `/showonlineinfo`, `/scimitaroutput`, `/memdebug`, `/heartbeat` | diagnostics |
| `/localbigfile`, `/keepobjectnames`, `/localgamemodes` | data and game modes |
| `/lanmode`, `/gamemode:Wanted`, `/lanWorld:...`, `/privatecode:N`, `/autojoin`, `/onlineconfig:...` | multiplayer (no servers) |
