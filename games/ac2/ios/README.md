# AC2 and Brotherhood on iOS (iPhone, iPad)

The same ports as on the Mac (AC2 here; Brotherhood, `acb`, built the same way,
see [Brotherhood](#brotherhood)): the translated game, the platform layer and the
Metal backend, built for iOS, with a UIKit shell (`games/common/shell_ios.mm`) in
place of the AppKit one. What the two shells share (game pads, scripted input,
mouse state, frame bookkeeping, the F6 statistics) is in
`games/common/shell_common.mm`.

Tested on an iPhone 17 and an iPad Air 11-inch (M3): AC2 with its videos, and
Brotherhood up to the arrival in Rome on the iPhone 17; with an Xbox controller, a
DualSense and a GameSir USB-C pad. The launcher shows at every start, in one
scrolling form (the whole app is landscape-only), and its Graphics choices hold while
playing ([../../common/launcher/README.md](../../common/launcher/README.md)).

What differs from the Mac:

- **Metal only.** There is no desktop OpenGL; `platform/src/opengl_metal.cpp`
  sends every GL import to the backend. The backend uses its own GL enum header
  (`platform/src/metal/gl_enums.h`, from `tools/gen_gl_enums.sh`).
- **Threads.** UIKit owns the main thread, so the game's `main` (`game::game_main` in
  `games/common/main.cpp`) runs on a thread of its own, with a 64 MB stack. Its event pump
  takes the input UIKit queued.
- **Input.** Game pads through the GameController framework, as on the Mac; a
  hardware keyboard (UIKey presses, as Mac key codes); a mouse or trackpad on
  iPad (GCMouse, pointer locked). Touch works the menus: a finger places the
  UI's cursor and holds the left button (`shell::touch`), and the menus' own
  cursor (a Flash clip they attach on a PC or Mac) is left out unless a mouse is
  connected (`hide_ui_cursor`, `games/common/shell_common.mm`). No touch
  controls for gameplay yet. `SCIMITAR_TRACE_INPUT=1` logs where touches land
  on the UI's 1280x720 stage.
- **Audio.** No CoreAudio HAL: Wwise's device questions get a fixed 48 kHz
  stereo device (with a Mac's stereo channel layout), and its output unit is
  RemoteIO (`platform/src/audio.cpp`).
- **File names.** iOS file systems are case-sensitive and the game names files
  as on a Mac's disk (`sounddata/mac/` for `sounddata/Mac/`: without the fix,
  Wwise loads no sounds and mixes silence). Paths that do not exist as written
  are resolved ignoring case (`host_path`, `platform/src/posix_fs.cpp`).
  `SCIMITAR_TRACE_FILES=1` logs opens, `SCIMITAR_TRACE_AUDIO=1` the audio calls
  and the level the game renders.
- **Display.** The screen's pixels and refresh rate are the one display mode
  set the game sees (`set_host_display`); the picture is letterboxed to the
  mode's aspect, and the game fills that mode whatever its shape, with the HUD
  at the screen's edges (`games/ac2/view.cpp`, `hud.cpp`). 120 Hz is allowed on iPhone (`CADisableMinimumFrameDurationOnPhone`).
- **Background.** The game is told it is inactive, its settings are written,
  and the render thread waits at a frame boundary until the app is in front.
- **Videos.** The Bink videos play through FFmpeg's bink decoders
  (`platform/src/bink.cpp`, sound through RemoteIO), built for iOS as static
  libraries by `tools/build_ffmpeg_ios.sh iphoneos` (or `iphonesimulator`):
  FFmpeg 9.0.2's source, checked against its SHA-256, configured with only the
  file protocol, the bink demuxer, the bink and binkaudio decoders, swscale and
  swresample (about 3.7 MB), installed to
  `~/Library/Developer/scimitar/ffmpeg-<sdk>`, outside the repo, where
  `ios.cmake` finds them (`SCIMITAR_FFMPEG_IOS`). Without them the game skips
  its videos. FFmpeg is LGPL: an app given to others would have to let them
  relink it against another FFmpeg.
- **Shaders.** The launcher precompiles only the pipelines recorded in earlier
  play (`SCIMITAR_PRECOMPILE=full` compiles all the archives' programs, which takes
  many minutes on a phone); the rest compile in the background as they first
  appear.

## The game's files

The Mac game as installed, `Assassin's Creed 2.app` (about 8.7 GB, the
executable included: the port loads its segments), goes into the app's
Documents folder: Finder, the device, Files tab, drag it onto AC2. `AC2_APP`
overrides the place.

## Build

The translator runs on the Mac, so build the Mac port first
(`SCIMITAR_FULL_IMAGE=ON`, see the top README); the iOS build compiles its
`build/gen/full` (or `-DSCIMITAR_GEN_DIR=...`).

The app icon is made at build time from the Mac game's icon
(`-DAC2_ICON_SOURCE="/path/to/Assassin's Creed 2.app/Contents/Resources/AC2.icns"`;
the default is the author's layout, `../steam_depot/33362_5/...`, and does not follow
`AC2_APP`): `make_icon.swift` draws it over a dark background at
1024 px and `actool` compiles it into the bundle (`Assets.car`). It is game art,
so it stays in the build directory; without the file the app has no icon.

Simulator:

```bash
cmake -S . -B build-ios-sim -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator \
      -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 -DCMAKE_BUILD_TYPE=Release
cmake --build build-ios-sim --target ac2 -j10
xcrun simctl install booted build-ios-sim/ac2.app
SIMCTL_CHILD_AC2_APP="/path/to/Assassin's Creed 2.app" \
    xcrun simctl launch --console-pty booted github.ke619.ac2
```

The Simulator reads the Mac's copy of the game directly. Environment variables
(`SCIMITAR_KEYS`, `SCIMITAR_PAD`, `SCIMITAR_STATS`, ...) pass the same way, with
the `SIMCTL_CHILD_` prefix.

Device:

1. A provisioning profile for the bundle ID (`github.ke619.ac2`, or
   `-DAC2_BUNDLE_ID=...`). Xcode's automatic signing makes one when it builds
   any project with that bundle ID and the team
   (`xcodebuild ... -allowProvisioningUpdates -allowProvisioningDeviceRegistration`
   on a one-file stub app is enough); it lands in
   `~/Library/Developer/Xcode/UserData/Provisioning Profiles/`. With
   `-DAC2_MEMORY_ENTITLEMENTS=ON` (below) the stub's entitlements must hold
   the two memory keys too, so that the profile has those capabilities.
2. Build and sign with it:

   ```bash
   cmake -S . -B build-ios -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
         -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 -DCMAKE_BUILD_TYPE=Release \
         -DAC2_SIGN_IDENTITY="Apple Development: ..." -DAC2_TEAM_ID=<team> -DAC2_PROFILE=<profile>
   cmake --build build-ios --target ac2 -j10
   xcrun devicectl device install app --device <udid> build-ios/ac2.app
   ```

   `AC2_TEAM_ID` fills in `ac2.entitlements.in`; `-DAC2_MEMORY_ENTITLEMENTS=ON`
   adds the memory entitlements.
3. The game's files, straight into the app's container (repeat runs skip
   unchanged files):

   ```bash
   xcrun devicectl device copy to --device <udid> --domain-type appDataContainer \
         --domain-identifier github.ke619.ac2 --source ".../Assassin's Creed 2.app" \
         --destination "Documents/Assassin's Creed 2.app"
   ```

Memory: the guest's address space is reserved as one 2 GB block on iOS (the
compact layout, `runtime/include/scimitar/guest_memory.h`: the heap sits low,
at 0x08000000, instead of at 0x80000000 of a 4 GB block;
`SCIMITAR_COMPACT_GUEST=0` brings the 4 GB block back). The game then needs no
special entitlements: on the iPad Air M3 it peaked at about 1.3 GB in Florence,
where iOS allows an app about 5 GB. The statistics overlay (a tap in the top
left corner) shows the room left under iOS's limit, and the log reports it
every 600 frames. For devices with less memory, `-DAC2_MEMORY_ENTITLEMENTS=ON`
signs with the two entitlements that raise the limits, which need a paid
developer account:

- `com.apple.developer.kernel.extended-virtual-addressing`: a larger address
  space;
- `com.apple.developer.kernel.increased-memory-limit`: a higher memory limit.

Without them a free account's personal team should be able to sign the app
(untested; its profiles last 7 days).

## Brotherhood

One game per build directory: `-DSCIMITAR_IOS_GAME=acb` builds `acb.app`
(bundle ID `github.ke619.acb`, `ACB_BUNDLE_ID`; icon from `ACB.icns`,
`ACB_ICON_SOURCE`) from the Mac build's `gen/full/acb_*.cpp` and Brotherhood's
sources (`games/acb`), with the same shell, launcher and platform layer. The
signing options keep their `AC2_` names. Any profile whose App ID covers the
bundle ID works, a team wildcard one (`*`) included:

```bash
cmake -S . -B build-ios-acb -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
      -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 -DCMAKE_BUILD_TYPE=Release \
      -DSCIMITAR_IOS_GAME=acb -DAC2_SIGN_IDENTITY="Apple Development: ..." -DAC2_TEAM_ID=<team> \
      -DAC2_PROFILE=<profile>
cmake --build build-ios-acb --target acb -j10
xcrun devicectl device install app --device <udid> build-ios-acb/acb.app
xcrun devicectl device copy to --device <udid> --domain-type appDataContainer \
      --domain-identifier github.ke619.acb --source ".../Assassin's Creed Brotherhood.app" \
      --destination "Documents/Assassin's Creed Brotherhood.app"
```

The game (about 10 GB) goes into Documents as `Assassin's Creed Brotherhood.app`;
`ACB_APP` overrides the place.
