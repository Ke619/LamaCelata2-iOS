# AC2 developer command-line options

The retail AC2 1.0.1 Mac executable still contains the engine's developer
command-line options: 148 options registered by
`scimitar::InitializeCommandLineOptions` (`0xf32a0`), each with a
`CommandLineCallback_*` handler. The retail Mac build never calls that function,
so on a real Mac the options do nothing. The port calls it itself before the
game's `main` (`games/common/main.cpp`), with its arguments joined into one string;
`SCIMITAR_GAME_OPTIONS="/a /b:c"` (the launcher's Advanced → Game options) adds more.

The syntax is the engine's own, Windows style: `/name`, with the parameter after
`:` and several parameters separated by `,`. A leading `-` is not accepted (the
option is skipped).

```bash
build/ac2 /godmode /NoFogOfWar
build/ac2 /nobink /world:Firenze
build/ac2 "/path/to/Assassin's Creed 2.app" /nobink /world:0x5b29460a
```

Most handlers set one field of `scimitar::CommandLineOptions` (the object behind
the pointer at `0x278f950`), so whether an option does anything depends on whether
game code reads that field. The table below comes from static analysis (the field
each handler writes and the functions that read it). Only `/nobink /world:<name>`
has been tried in play so far: it loads the world directly.

## Options with code behind them

| Option | Field | Read by | Likely effect |
|---|---|---|---|
| `/godmode` | `+0x1d1d` | `ac2::CSrvPlayerHealth` statechart | invincible player |
| `/NoFogOfWar` | `+0x207c` | `ac2::MapManager::MapManager` | map revealed |
| `/pausetimeofday` | `+0x203b` | `ac2::TimeOfDayManager` | time of day frozen |
| `/initialtimeofdaytransition`, `/pausetimeofdaytransition` | | time of day | |
| `/world:<name or 0xID>[,...]` | `+0x370` (count), entries of 0x208 bytes | `LoadCommandLineWorld` (`0xfb660`) | loads up to 10 worlds through `EngineContext::LoadWorldAsync`, and turns the save system off (`System::SetUseSaveSystem(false)`) |
| `/worldarea:<name>` | per world entry | `LoadCommandLineWorld` | area within the last `/world` |
| `/mapmenu` | `+0x2048` | `LoadCommandLineWorld` | goes down the command-line world path even without `/world` (also needs `+0x2031`, see below) |
| `/E3`, `/E3onstage`, `/gamescom` | `+0x2075`, `+0x2077` | about 19 reads for `E3` | trade-show demo modes |
| `/fullscreen` | `+0x2073` | | |
| `/shadows`, `/postfx`, `/msaa`, `/lightmode`, `/fardist`, `/loadingrange`, `/layersDepthBias`, `/brightness` | | renderer | rendering parameters |
| `/skipmips`, `/skipmipscharacter`, `/skipmipsenvironment` | | textures | lower texture mips |
| `/language:<code>`, `/soundlanguage:<code>` | `+0x17e0`, `+0x17e8` (bit masks) | `SetCurrentTextLanguage`, `SetCurrentSoundLanguage` | text or voice language, by short code (`rus`); overridden at startup by the installed language, so no visible effect (see [Language](#language), and `SCIMITAR_TEXT_LANGUAGE`/`SCIMITAR_VOICE_LANGUAGE` there instead) |
| `/nosound`, `/nobink` | | | no sound, no videos |
| `/sound*` (about 15: pools, budgets, streamer, thread priorities) | | Wwise setup | sound engine tuning |
| `/maxthread`, `/heartbeat`, `/quit`, `/autoprofile`, `/profilerout` | | | threads, heartbeat, automatic quit and profiling |
| `/memlog`, `/memvalidate`, `/meminfodump`, `/memall`, `/memdebugfreequeue*` | | memory manager | memory diagnostics |
| `/logfastloadobjects`, `/exportfastload`, `/exportchanges`, `/import` | | data tools | |
| `/orbit_*`, `/uplay_*` | | Orbit / Uplay | unused by the port (Orbit is replaced) |

How `/world` works (`LoadCommandLineWorld`, `0xfb660`, called from
`scimitar::main`):
- `main` runs the command-line world block only when options field `+0x2031` is
  set. That field is off by default and set by `/nobink`, `/walkthrough` and
  `/memstatdatalayers`. Without one of them `/world` is parsed but ignored, and
  the normal title and menu flow runs.
- A parameter containing `0x` is a hex object ID (`StringBuilder::StringToID`).
  Anything else is looked up by name with `GameBootstrap::GetObjectId`, so the
  names below work as they are.
- Each world loads through `EngineContext::LoadWorldAsync`, and
  `LoadWorldCallback` makes it the current game world. With no `/world`, the
  same path loads `GameBootstrap`'s default world. `/world` also turns saving
  off.

Tried: `build/ac2 /nobink /world:Firenze` starts in Florence.

World objects: each `.forge` archive has a root entry named after it, read from the
archive's file table (`FILEDATA` records, 16-byte FAT entries: 64-bit offset,
32-bit id, 32-bit size):

| World | ID | Archive |
|---|---|---|
| Firenze | `0x5b29460a` | `DataMAC_Firenze.forge` |
| Venezia | `0x48b659dc` | `DataMAC_Venezia.forge` |
| Toscana | `0x7c82d813` | `DataMAC_Toscana.forge` |
| Wetlands (Romagna) | `0x701bc87d` | `DataMAC_Wetlands.forge` |
| Mountains (Apennine) | `0x6ad1d250` | `DataMAC_Mountains.forge` |
| Villa (Monteriggioni) | `0x83ac7022` | `DataMAC_Villa.forge` |
| Roma | `0x652bbd7a` | `DataMAC_Roma.forge` |
| Altair_Memory | `0x3b71eebb` | `DataMAC_Altair_Memory.forge` |
| Abstergo | `0x27ee8b8a` | `DataMAC_Abstergo.forge` |
| Hideout | `0x2baf105b` | `DataMAC_Hideout.forge` |
| WhiteRoom, WhiteRoom_E3 | `0x32e21e58`, `0xa90f051e` | `DataMAC_WhiteRoom.forge` |
| TitleScreen | `0x1333e0f7` | `DataMAC_TitleScreen.forge` |
| LGS01_Duomo | `0x28996aad` | |
| LGS02_Palazzo_Medici | `0x37c799b9` | |
| LGS03_Gimignano | `0x3f13009e` | |
| LGS04_Novella | `0x3ce27b15` | |
| LGS05_Forli_Citadel | `0x3be6cbf7` | |
| LGS06_Frari | `0x289eafb5` | |
| LGS07_San_Marco | `0x567e9b8c` | |
| LGS08_ArsenalShipyard | `0x3fb8181a` | |
| LGS09_Monteregionni | `0x43606cdd` | |
| LGS10_Visitazione | `0x579779d4` | |

The LGS worlds are the assassin tombs, each in `DataMAC_<name>.forge`.

## Language

The language the game plays in is the **installed language**: the `Language` key
of the app's `Info.plist`, which the original installer wrote (`eng` in the Steam
copy). `scimitar::SelectInstalledLanguage` (`0xeec70`) reads it through
`MacGetLanguageName` (`CFBundleGetValueForInfoDictionaryKey`) and sets the text,
subtitle and voice language together. Codes come from `GetShortLanguageName`: `eng`,
`fre`, `spa`, `pol`, `ger`, `chi`, `hun`, `ita`, `jap`, `cze`, `kor`, `rus`, `dut`,
`dan`, `nor`, `swe` (1–16). The Mac app holds text and voice for eng, fre, ger, ita,
spa and rus; dan, dut, nor and swe have text only, and the other codes are not in
the Mac data.

Neither `/language:rus` nor the system language changes it: the installed language
is presumably applied after the options run (order not traced), and `scimitar::GetSystemLanguage`
(`MacGetSystemLanguage`, reading `NSLocale preferredLanguages`) has no callers.
`SCIMITAR_LANGUAGE` only sets what `NSLocale preferredLanguages` returns, so it has
no effect on the language either.

The port answers the `Language` key with `SCIMITAR_GAME_LANGUAGE` when set
(`platform/src/cf.cpp`), and replaces `SelectInstalledLanguage`
(`games/ac2/language.cpp`) so that `SCIMITAR_TEXT_LANGUAGE` sets the text and
subtitles and `SCIMITAR_VOICE_LANGUAGE` the voices; the launcher sets both.
The options' Dialogue Language offers the text's language or Italian:
`OptionsMenu::GetVariable` answers the Flash list `OptionsSupportedLanguages`
(names joined by `;`) and the row `OptionsSpokenLanguage`, and `SetVariable`
takes the row back (as the text of a number: row 0 is the text's language,
`OptionsMenu +0x8c`, any other Italian). The choice goes through
`GameConfig::ValidateAndSetSoundLanguage`, which accepts only the game's list
of voice languages, and is saved with the player's options
(`PlayerOptionsSaveData +0xc`, written when the options menu closes), but
`PlayerOptionsSaveData::LoadData` keeps a saved one only if it is the text's
language or Italian and resets any other to the text's.

The port lists every installed voice language there
(`sounddata/Mac/sounds_<code>.pck`; `games/ac2/language.cpp` wraps
`GetVariable` and `SetVariable` in the function table, the Flash movie calling
them through the vtable), and its `ValidateAndSetSoundLanguage` accepts them,
also in `LoadData`'s reset (found by its return address), so the choice
survives a restart. `SCIMITAR_VOICE_LANGUAGE` wins at startup, the menu after.
Tried 2026-11-05: Russian, then French, picked in the menu, each opened its
`sounds_<code>.pck`, and French came back after a restart.

Tried 2026-10-27: a bundle whose `Info.plist` says `rus` (with `MacOS` and
`Resources` symlinked to the real app) shows Russian menus and opens
`sounds_rus.pck`.

## Stripped options

The handlers of these are empty (`push ebp; mov ebp, esp; leave; ret`), so they do
nothing in retail: `/flythrough`, `/startpos`, `/startlookat`, `/walkthroughloading`,
`/walkthroughGPU`, `/walkthroughGPUScreenShot`, `/infinitewalkthroughloadingworlds`,
`/loadondemand`, `/nodefrag`, `/nooverlappedregions`, `/testgroup`,
`/customphysmemalloc`, `/restoreoriginalwwisebehavior`.

About 60 others only set a flag (six instructions). Whether that flag is read
anywhere has been checked only for the ones in the table above.

## No debug console

`MacHandleKeyDown` passes keys to `scimitar::AddKeyDownToCommandQueue`, which is
an empty function in retail: the in-game console is compiled out.
