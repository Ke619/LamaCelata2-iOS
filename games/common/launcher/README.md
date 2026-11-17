# games/common/launcher

The ports as apps, and the settings screen they show before the game starts.

## The Mac apps

Building a port also makes its app next to the plain executable:
`build/AC2.app` for `build/ac2`, `build/ACB.app` for `build/acb`, and for the
Windows ports `AC1.app`, `ACR.app` and `PoP.app` next to `ac1`, `acr` and `pop`
(`scimitar_add_game` or `scimitar_add_windows_game` with `APP_NAME` and
`APP_TITLE` in the top-level CMakeLists, both through `scimitar_add_app`, then
`make_app.cmake` after each link). An app holds:

- the executable, an APFS clone of the port (no extra disk space),
- an `Info.plist` (`local.scimitar.<id>`, Retina, games category, and the
  microphone note macOS wants from every app that plays sound through Core
  Audio; the game records nothing),
- the installed game's icon, copied at build time (game art never enters the
  repo; without the game the app has no icon): a Mac release's `.icns` (also
  Prince of Persia's, from its Cider bundle), else the Windows executable's own
  (`tools/pe_icon.cpp` writes its first icon group as an `.ico`, which `sips`
  converts),
- an ad hoc signature.

The app does not contain the game: it points to the installed one. Copy or
drag the app anywhere (`/Applications`, the Dock); the settings stay in its
user defaults (`defaults read local.scimitar.ac2`).

## The Windows ports' apps

AC1, Revelations and Prince of Persia run a Windows executable through
`platform/win32`, but start the same way: `games/<id>/game.cpp` gives each a
`game::Info`, and its `main.cpp` calls `game::run_launcher` (`launch.cpp`, which
the Mac ports' `games/common/main.cpp` calls too) before loading the game.

| | Game location (`app_env`: the `.exe` or its folder) | `game_file` |
|---|---|---|
| AC1 | `AC1_EXE`; the build's: `windows-releases/Ubisoft Launcher/Assassin's Creed 1` | `AssassinsCreed_Dx9.exe` |
| Revelations | `ACR_EXE`; the build's: `windows-releases/Steam/Assassin's Creed Revelations` | `ACRSP.exe` |
| Prince of Persia | `POP_EXE`; the build's: `steam_depot/Prince of Persia.app` (the Mac release, its Cider bundle) | the `.exe` in the bundle's `transgaming/c_drive/Program Files/Ubisoft/Prince of Persia`, or `Prince of Persia.exe` in a Windows copy's folder (GOG's) |

`game::Info::game_file` is the file the port runs under the game's location
(alternatives separated by `|`); the launcher checks it to say "Found", and
its Choose… takes a folder (or the `.exe`, or a `.app`) for these ports.
`game::Info::launcher_settings` lists the settings a port supports; the
launcher shows and sets only those (null, for AC2 and Brotherhood: all). The
Windows ports have `full-screen stats aniso`:

- Mode: they start in a window; Full screen sets `SCIMITAR_FULL_SCREEN=1`,
  which puts the window in full screen when it first shows
  (`platform/win32/user32.mm`). Command-Return switches while playing.
- Show frame rate and memory (`SCIMITAR_STATS=1`; F6 while playing).
- Graphics: anisotropic filtering alone (`SCIMITAR_ANISO`, the Direct3D 9
  layer's, `d3d9_metal.mm`), so the tab has that one setting.

Not offered: the Screen shape (the picture keeps the game's own shape), Skip
intro videos, the languages (Prince of Persia's `SCIMITAR_POP_TEXT` and
`SCIMITAR_POP_VOICES` stay switches for now), the button prompts, the game
options, the F4 graphics menu's switches, and the shader precompile: they use
the Direct3D 9 backend, which compiles shaders as they come, not the Metal
renderer's cache.

## The launcher

`Launcher.swift` (SwiftUI, one file for the Mac and iOS, compiled with
`swiftc` to an object the ports link; C entry points in `launcher.h`). It shows
when the port starts as an app: on the Mac from its `.app` with no arguments
(not `build/ac2`, not with `SCIMITAR_HIDDEN` or `SCIMITAR_NO_LAUNCHER`), on iOS
always, unless the run is scripted (`SCIMITAR_KEYS`, `SCIMITAR_PAD`,
`SCIMITAR_EXIT_FRAME`) or `SCIMITAR_NO_LAUNCHER` is set. On the Mac "Show this
window at start" can turn it off; holding Option while the app opens brings
it back.

On the Mac the settings are in four tabs (General, Graphics, Language and
controls, Advanced; only those with settings the game supports), drawn as a row of
plain SwiftUI buttons: TabView puts its tabs in the window's toolbar, where they
collapse behind an overflow button, and a segmented Picker came up stretched past
the window;
`SCIMITAR_LAUNCHER_TAB=graphics` (or `general`, `language`, `advanced`) opens
on that tab and shows the window even when "Show this window at start" is off,
for screenshots. iOS shows them in one scrolling form.

**Compiling shaders.** With the Metal renderer, Play first compiles the
shaders the cache does not hold yet (`ShaderProgress` in `Launcher.swift`
calls the port's `scimitar_shader_precompile`, found with `dlsym`;
`platform/src/metal/shader_cache.mm`, docs/METAL.md "Shader precompile and
cache"). The window shows "Compiling shaders", a bar and "N of M shaders
(P%)", then the game starts. The first start compiles every program in the
game's archives (about 25-29k: minutes on a cold system cache); later starts
compile only what is new, usually nothing. It is resumable: quit midway and
the next Play continues. `SCIMITAR_PRECOMPILE=0` skips it, `background` compiles
the archives' programs after the first frame, `recorded` compiles only the
pipelines recorded in earlier play. On iOS `recorded` is the default (the
archives' programs would hold a phone's launch for many minutes;
`SCIMITAR_PRECOMPILE=full` compiles them all); the rest compile in the background
as they first appear. When the launcher
does not show, the game does the same step itself at its first frame, behind
a plain progress bar.

Each choice becomes one of the port's switches, set in the process's
environment before the game starts:

| Setting | Switch |
|---|---|
| Game (Mac: a path, with Choose…; iOS: the copy in the app's Documents) | `AC2_APP`, `ACB_APP`, `AC1_EXE`, `ACR_EXE`, `POP_EXE` (`game::Info::app_env`) |
| Mode: full screen or window (Mac) | `SCIMITAR_WINDOWED=1`; the Windows ports: `SCIMITAR_FULL_SCREEN=1` |
| Screen shape: fill the screen or 16:9 (original); the videos are 16:9 with bars either way | `SCIMITAR_ASPECT=16:9` |
| (always, Mac: Metal; OpenGL only when `SCIMITAR_RENDERER` is already set, for tests) | `SCIMITAR_RENDERER=metal` |
| Show frame rate and memory | `SCIMITAR_STATS=1` |
| Skip intro videos (the Ubisoft logo and the disclaimer; `games/common/intro.cpp`) | `SCIMITAR_SKIP_INTRO=1` |
| Remove color filter (AC2 only; `docs/AC2_COLOR_FILTER.md`) | `SCIMITAR_NO_COLOR_FILTER=1` |
| Text and subtitles (those the game has, where it lists them: Brotherhood's `localization.lang`, and Russian where its data has it) | `SCIMITAR_TEXT_LANGUAGE=<code>` |
| Voices (those installed: `sounddata/Mac/sounds_<code>.pck`) | `SCIMITAR_VOICE_LANGUAGE=<code>` |
| Button prompts in the HUD and the memories' start screens, and Brotherhood's menus: follow the controller, keyboard, Xbox, PlayStation (also with the keyboard) | `SCIMITAR_BUTTON_PROMPTS=keyboard`, `xbox`, `playstation` |
| Game options | `SCIMITAR_GAME_OPTIONS="/a /b:c"` (added to the command line) |
| Graphics: anisotropic filtering | `SCIMITAR_ANISO=2`, `4`, `8`, `16` |
| Graphics: draw distance (Brotherhood's LOD multiplier, 0.5 to 2.0 in its options; AC2's LOD scale, 1.0) | `SCIMITAR_DRAW_DISTANCE=<x>` |
| Graphics: character detail (Brotherhood: 1.0 to 1.6, higher is better; AC2: 1.6 to 1.0, lower is better) | `SCIMITAR_CHARACTER_DETAIL=<x>` |
| Graphics: environment distances | `SCIMITAR_ENV_DISTANCES=<near>/<far>` |
| Graphics: shadow distances | `SCIMITAR_SHADOW_DISTANCES=<near>/<far>` |
| Graphics: shadow map size | `SCIMITAR_SHADOW_MAP=512` to `4096` |

The menus' button art: Brotherhood draws a legend's or help text's button
tags (`LocalizationManager::ReplaceTag`) by the selected pad's type
(`PadManagerNative::GetSelectedPadType`): the keyboard's key in a box, or the
button library's art under the type's prefix (`360_` for the Xbox 360's); the
prompts chosen answer that (overrides.cpp). The Mac library has no
PlayStation art, so PlayStation gets the Xbox art there. AC2's menus draw the
Xbox 360 art whatever the input (their library has no other), and their
movies' `PLATFORM` answer is the PC's: a console's would also drop what
consoles lack (Exit to Desktop, Customize Controls). For tests,
`SCIMITAR_UI_PLATFORM=ps3` or `xbox` gives every movie that console's answer,
and `SCIMITAR_UI_PAD_TYPE=<n>` gives Brotherhood's tags that pad type.

On the Mac, Play starts the game in a fresh image of the process (`execv`
of the same executable, with the choices in its environment and
`SCIMITAR_NO_LAUNCHER=1`): started in the process that had shown the
launcher's window, the game's full-screen window stayed black until the
application was activated again.

The Graphics section offers, for each of those settings, "Game setting"
(no switch: the game's graphics options decide) or the values of the F4
graphics menu (`games/common/graphics_menu.cpp`), which go past the options'
highest. A switch's value is kept in effect over the game's options for the
whole run: the menu puts it back at every event pump when the game applies its
own (at start, from its options menu). F4 while playing shows them and changes
them live; a change there is kept the same way until the game quits, and is not
saved. On iOS there is no F4 menu: the shell keeps the launcher's choices
in effect the same way (`graphics_menu::enforce` at every pump). The menu is
host code that finds the game's options by symbol, so it needs no new
translation of the game.

The Text and Voices lists are read once for each game location (not at each
redraw); a choice the chosen copy lacks is not applied.

Where the game is looked for when none was chosen: the environment, where the
build found it, `/Applications`, `~/Applications`, and Steam's library
(`~/Library/Application Support/Steam/steamapps/common/*/`). For AC2, Steam's
library holds the 1.0.0 executable the port cannot run; the 1.0.1 app comes from
the Steam console ([docs/MAC_RELEASES.md](../../../docs/MAC_RELEASES.md)) and has to be
chosen: the launcher does not check the version and shows Found for the 1.0.0
install too.

The resolution (including the display's native one) stays in the game's own
options.

### Languages

Language codes are `scimitar::Language`'s short names (`eng`, `fre`, `ger`,
`ita`, `spa`, `rus`; text only: `dan`, `dut`, `nor`, `pol`, `swe`).
"Default" leaves the game's own options in charge. A language chosen
here applies at each start; one picked in the game's options menu afterwards
wins for the rest of that run (and is saved with the options, for the starts
with "Default").

- AC2 sets text, subtitles and voices from its installed language (the
  `Info.plist` key `Language`) in `SelectInstalledLanguage`, which
  `games/ac2/language.cpp` replaces to take the two switches apart.
  `SCIMITAR_GAME_LANGUAGE=<code>` answers the `Language` key itself
  (`platform/src/cf.cpp`), for both. A text language the game lacks falls back
  to its default. Its Dialogue Language option offered only the text's
  language or Italian, and the saved options kept no other; it now lists every
  installed voice language (`OptionsMenu::GetVariable` and `SetVariable`
  wrapped, `GameConfig::ValidateAndSetSoundLanguage` replaced).
- Brotherhood starts from the system's preferred language (which the port
  answers with the text language's ISO code, `platform/src/objc.cpp`), then
  applies its options (`SAVES/OPTIONS`; the ini's `[Language]` only records
  them) through `LocalizationUtils::ValidateAndSet{Text,Subtitle,Sound}Language`,
  which `games/acb/language.cpp` replaces so that the chosen languages win.
  Its options' Subtitle Language offered only the text's language and Spoken
  Language only the voices `localization.lang` lists; `ReadSupportedLanguages`,
  replaced there too, keeps the file's text languages and adds every installed
  voice language, and Russian text: the file leaves it out, but
  `DataMAC.forge` carries `LocalizationPackage_Russian` and `_Subtitles` and
  the Cyrillic font, and the forge's index (its first megabyte) names the
  packages; the launcher's list looks there too.

### Tests

`SCIMITAR_LAUNCHER_PLAY_AFTER=seconds` presses Play by itself:

```bash
defaults write local.scimitar.ac2 ac2.fullScreen -bool false
open --env SCIMITAR_MUTE=1 --env SCIMITAR_LAUNCHER_PLAY_AFTER=2 --stderr /tmp/ac2.log build/AC2.app
```

## Adding a game

`scimitar_add_game` (or `scimitar_add_windows_game`) with `APP_NAME` and
`APP_TITLE` makes the app; the launcher takes the rest from the game's
`game::Info`: its `launcher_settings` names the settings to offer
(`Launcher.swift`, `LauncherGame.settings`: `windowed`, `full-screen`,
`aspect`, `stats`, `intro`, `languages`, `prompts`, `options`, `graphics`,
`aniso`, `metal`).
