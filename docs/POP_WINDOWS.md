# Prince of Persia (2008) on this Mac: the Windows executable, recompiled

Running Ubisoft Montreal's *Prince of Persia* (2008, the Scimitar engine of AC1) on
arm64 macOS with this project's recompiler, the way AC1 and Revelations run
([AC1_WINDOWS.md](AC1_WINDOWS.md), [ACR_WINDOWS.md](ACR_WINDOWS.md)). Started
2026-10-28. Nothing from the game enters the repository; generated code stays in
the build directory.

## The executable and the data

The source is the Windows executable inside the Mac release, which is TransGaming
Cider (a Wine-based wrapper) around the Windows game
([ANVIL_FAMILY.md](ANVIL_FAMILY.md)):
`../steam_depot/Prince of Persia.app/Contents/Resources/transgaming/c_drive/Program Files/Ubisoft/Prince of Persia/Prince of Persia.exe`.

| | |
|---|---|
| SHA-1 | `8243f4162c21054fffc329f1e48cb10dcd9acb6d`, 12,988,928 bytes |
| Built | 2008-12-17 (PE time stamp), MSVC; PDB path `d:\dev\pc-pvlasie-pop0-mac\pop0mac\…` |
| Layout | PE32, image base 0x400000, **no base relocations**, `.text` 0x9241ec bytes at 0x401000, `.rdata` at 0xd26000 |
| RTTI | 383 type names, none of the engine's |
| Imports | 287 from 19 DLLs (below) |

**The data is the Russian edition's.** The 13 archives the bundle shares with the
GOG copy are byte-identical because they are the same files: the GOG download is the
Russian one (`goggame-1207659109.info`: `"language": "Russian"`), and in the Mac
bundle those 13 archives and `DataPC_StreamedSoundsRus.forge` carry the GOG files'
date (2 Dec 2008) and permissions, while the Mac release's own files (the five other
voice packs, the executable, `Videos/eng` …) are dated 6 Feb 2009. The engine's
localization settings in `DataPC.forge` list one supported language, Russian, and
make it the default: whatever the registry (`HKCU\Software\Ubisoft\Prince of
Persia\1.0\Engine\LNG_Language`) or the locale says, the game falls back to
Russian. The text exists only in Russian (with English text the title screen's
prompt is empty). The voices exist in six languages, so the port lets them be
chosen: `SCIMITAR_POP_VOICES=English` (or French, German, Italian, Spanish)
plays `DataPC_StreamedSoundsEng.forge`; `SCIMITAR_POP_TEXT` does the same for the
text, for data that has it (`games/pop/game.cpp` replaces the engine's two
language setters, 0x51f820 and 0x51f840).

## Translation

`scimitar-analyze --coverage`: 59,055 of 59,066 functions translate (2,950,692 of
2,952,350 instructions). The 11 others are weak starts inside other instructions
(round constants such as 0x006c0020 decoding to far jumps and segment loads).

AC1's PE front end found almost all of it; three discovery fixes (in
`recomp/src/flow.cpp`, shared with AC1) were needed at run time:
- **Packed code after `jmp rel32` thunks.** A statically linked library (callbacks
  registered at 0xc0ec8f) has no padding: functions follow runs of `jmp` thunks.
  Code addresses taken by immediates right after a `jmp rel32` are now plausible
  starts: 1,195 more functions, among them the allocator callback the game calls at
  its first frames (0xc0ebe5).
- **Dead bytes after a `throw`.** MSVC left a `pop %ebx` after
  `_CxxThrowException` at 0x4bc70f, which "runs into" the next function (0x4bc710),
  and code that runs into a weak start used to drop it. A start that is a direct
  call's target is now kept in that case.
- **SSE moves between a switch's compare and branch** (`cmp $4, %eax; movss …; ja`
  at 0x87d68a): the bound was missed, the table read on into the next one, and the
  whole function failed. `movss`, `movsd`, `movaps`, `movups`, `fld`, `fst`/`fstp`
  are now skipped like `mov` when looking for the compare.

## Imports

287 from 19 DLLs. AC1's and Revelations' Windows layer (`platform/win32`) covered
all but 44; Prince of Persia's own are in `platform/win32/pop_misc.cpp`, plus:

| What | Where |
|---|---|
| Named mutexes (`CreateMutexA`: "Local\Prince of Persia", the second-instance check), `ReleaseMutex` | `pop_misc.cpp` |
| The executable's resources (`FindResourceA`, `LoadResource`, `LockResource`, `SizeofResource`), read from `.rsrc` where it is loaded | `pop_misc.cpp` |
| Wide file searches, module path and disk space (`FindFirstFileW`, `GetModuleFileNameW`, `GetDiskFreeSpaceExW`, `SHGetFolderPathW`) over the ANSI shims | `pop_misc.cpp` |
| `D3DXGetShaderConstantTable` (d3dx9_39, as d3dx9_36's), `D3DXCreateTexture` (the device's `CreateTexture`), `D3DXFloat16To32Array` | `pop_misc.cpp` |
| The registry: values the game writes are kept in the port's folder (`registry.txt`) and read back | `registry.cpp` (shared; AC1 and Revelations never write) |
| `RegisterClassA`, `Get/SetWindowLongA`, `SetWindowPos`, `GetActiveWindow`, `FindWindowA`, `GetDC`, cursor and focus calls | `user32.mm` |
| Bink: `BinkRegisterFrameBuffers` (the game decodes into its own Y, cR and cB textures and converts them in a shader), `BinkGoto`, `BinkSetMixBinVolumes` | `platform/src/bink.cpp`, `bink_win.cpp` |
| GDI's `GetStockObject`, COMCTL32, `SysFreeString`, `VariantClear`, ANSI cookies | `pop_misc.cpp` |

A shared Direct3D fix: `GetDevice` (on any resource) and `GetDirect3D` add a
reference as Direct3D's do. The game asks a surface for its device and releases
it, which destroyed the device right after `CreateDevice`.

## Names

PoP has no symbols and no engine RTTI, so its functions borrow names the way AC1's
did ([AC1_WINDOWS.md](AC1_WINDOWS.md), "Names from AC2 Mac"), through AC1. Matched
2026-10-29 (scripts and the result are kept outside the repository: the names come
from Ubisoft's binaries):

- **PoP to AC1.** Both are MSVC builds, so equal code (instructions with addresses
  masked, at least 12 of them, unique on each side) is an anchor: 5,508 pairs.
  There are also 866 pairs from strings and constants, and 6,041 pairs of very
  similar code (each the other's best by a margin, 804 of them new). Where two
  kinds of anchor name the same function, they never disagree (580 times). Pairs
  then spread along call lists, unique callers, callee sets, vtables (paired
  through matched constructors, or through slots that point into one AC1 vtable)
  and gaps in address order. Every pair needs similar code. **12,446 of 59,066**
  functions are matched. Of half the string and constant anchors held out, spreading
  recovered 227 correctly, 2 wrongly, and did not reach 183. Of half the
  equal-code anchors, it recovered 2,199 correctly and 2 wrongly. About half of
  PoP's code looks like nothing in AC1 (similarity under 0.3).
- **PoP to AC2 Mac directly** (seeded by the AC1 route, by AC1-paired vtables
  matched to AC2's vtable of the same class, and by strings): 916 pairs, 295 of
  them names the AC1 route lacks.
- **Class IDs.** A Scimitar class's `GetClassID` is `mov $crc32("Name"), %eax;
  ret` (slot 6), and the names are strings in the executable. That names 2,167 PoP
  vtables (2,032 classes, PoP's own included: `BhvNPC`, `PopMarketingCamera`), and
  through them the Object virtuals: slots 0-6 are the destructor, `Serialize`,
  `FastLoad`, a shared stub, `Duplicate`, `GetClassDescriptor` and `GetClassID`.
  Checked against them, 245 of 256 names carried over from AC1 or AC2 have the
  right class. The 11 wrong ones are `FastLoad`/`Duplicate` of classes with the
  same layout, and there the class ID wins.
- **Result** (`pop_names.tsv`: address, name, source, counterpart): **16,031
  functions**. 1,043 have AC2 names (748 through AC1, 295 directly), among them
  `scimitar::EngineLoop::Enter`, `Engine::Initialize`, `Engine::RegisterClasses`,
  `GameBootstrap::Initialize` and `LoadingThread::ThreadRun`. 8,990 are a class's
  standard virtuals from its class ID. 5,998 are only `Class::vslot_N` (from class
  IDs, AC1's RTTI, or AC1-paired vtables). Tiny functions in many vtables are
  code the linker folded, and their names say `[shared by N vtables]`.

A script outside the repository shows the names in backtraces. The translation
doesn't use them.

## Status

**2026-10-28 (night): playable from start to gameplay**, hidden and muted in
scripted runs: the Ubisoft logo, the title screen, the main menu, New Game, the
intro video, the opening cutscene in the engine and the first canyon, where the
Prince runs, jumps and climbs under keyboard or game pad control. At the main
display's size (2560x1440 here) it runs at 110-120 fps throughout; with the
game's vertical sync on, at 60. The sound engine plays music, cutscene sound
and ambience (checked in the mix, not heard).

2026-11-06: the Direct3D 9 fixes found with Revelations apply here too
([ACR_WINDOWS.md](ACR_WINDOWS.md)): depth buffers larger than the colour target are
kept (a washed-out frame at window sizes whose height the game rounds down, such as
1728x1117), and the half-pixel offset goes right and down.

Not yet checked: playing by hand (visible window, mouse look, a real pad),
hearing the sound, saves (none had been written by the end of the first
canyon; the game saves at its own checkpoints), anything past the first
minutes. The text is Russian only (the data's), the voices selectable.

### How it got there (2026-10-28)

`build/pop` (`cmake -DSCIMITAR_FULL_IMAGE=ON -DSCIMITAR_GAMES=pop`, target
`pop`; 32 generated parts, about 2 minutes to build from scratch on 10 cores):
the C runtime, the engine's settings, eight engine threads, DirectInput,
`DataPC.forge`, the window (800x600, the game's default), Direct3D 9 on Metal,
the shaders from `EKShadersPCCompress.bin`, DARE's sound on DirectSound (EAX
absent), the archives of every level, then the **Ubisoft logo video**,
"Ubisoft presents", and the **title screen** ("press any key", in Russian) over
its animated menu video, hidden and muted, without a crash.

Then, with Return pressed by `SCIMITAR_KEYS`: the **main menu** (New Game, Load
Game, Options, Extras, Quit) over its video, New Game, the loading screen, the
intro video with subtitles, and the **opening cutscene in the engine** (the
Prince in the sandstorm, Elika, the soldiers), drawn right: the painted look,
the ink outlines, fog and light shafts.

What input needed: the game creates its DirectInput keyboard and mouse from
`EnumDevices(DI8DEVCLASS_ALL)`, which listed only the pad (Windows lists the
system mouse and keyboard first), acquires them on `WM_SETFOCUS`, which never
came (showing a window now activates it: `WM_ACTIVATEAPP`, `WM_ACTIVATE`,
`WM_SETFOCUS`), and names every key through `GetObjectInfo` (now answered with
Windows' names for a US keyboard, for its controls menu). Its window procedure
ignores key messages; text entry is `WM_CHAR`.

After the cutscene, **gameplay**: the Prince in the canyon, the tutorial
prompt ("use the mouse to move the camera, Esc for the options"), the desert
lit and shadowed as it should be (the shadow map is a 4096x1024 R32F target in
four cascades). That took three things in the D3D9 shader path (shared with
AC1 and Revelations, Metal only):
- **Texture coordinates 8 and 9.** Shader model 3 passes ten interpolators of
  any index; the world shaders use `texcoord8` and `texcoord9`, which ARB (and
  so the translator) did not have: their vertex and pixel shaders were drawn as
  nothing. The ARB -> MSL translator now has `texcoord[8..15]` as varyings.
- **`dsx`/`dsy`** (5 shaders): emitted as `DDX`/`DDY` (NV_fragment_program2's
  names), which the ARB -> MSL translator turns into `dfdx`/`dfdy`.
- **Tangents.** The translator bound vertex inputs as NVIDIA's ARB aliasing
  does, where `tangent` and `binormal` share generic attributes 14 and 15 with
  `texcoord6` and `texcoord7`. The terrain reads both a tangent and texcoord6:
  with the light coefficients in the tangent's place it came out white. On
  Metal (31 attributes) they now have their own (16, 17; texcoords past 7
  from 18).

Input, sound and display after that:
- **Game pad**: the game lists the pad through DirectInput (the virtual Xbox
  360 controller, as AC1 sees it), reads it through both DirectInput and
  XInput, and shows the pad's buttons in its prompts. `SCIMITAR_PAD` scripts
  move and jump the Prince.
- **Sound**: DARE plays through DirectSound 3D buffers. They are now placed:
  distance rolloff between each buffer's minimum and maximum distance, and a
  pan by the direction from the listener (shared `dsound.cpp`; velocities and
  cones kept, not applied). `SCIMITAR_AUDIO_DUMP=file` writes the muted mix
  (raw 48 kHz stereo float): menu music, silence while loading, the cutscene,
  then gameplay ambience. Videos play their sound through Bink's own output.
- **Resolution**: the game reads its mode from the registry
  (`Engine\ScreenResolutionWidth`/`Height`) and starts at 800x600 without
  them; the port seeds them with the main display's size. The options menu
  changes them (kept in `registry.txt`).
- **Pacing**: with vertical sync off the game limits itself to about 120 fps.
  With it on (`Engine\VerticalSync`) it relies on `Present` blocking, which a
  hidden layer does not do: its loading screen ran at 1,400 fps and never
  ended. `D3DPRESENT_INTERVAL_ONE` now paces presents to 60 Hz.

The shared changes (discovery, DirectInput, activation messages, the shader
path, DirectSound 3D, present pacing) were checked against the other Windows
ports: AC1 and Revelations built from the same tree (regenerated) run to
AC1's intro videos and Revelations' main menu, hidden and muted.

Scripted runs: Return presses (`SCIMITAR_KEYS`, Mac key code 36) at frames
1200-4000, every 400, reach gameplay around frame 8000 with vertical sync on
(60 fps); W (13) moves, Space (49) jumps.

## Running

```bash
cmake -S . -B build -DSCIMITAR_FULL_IMAGE=ON -DSCIMITAR_GAMES=pop \
      -DPOP_EXE="/path/to/Prince of Persia.exe"
cmake --build build --target pop -j 10
open build/PoP.app
```

`PoP.app` shows the launcher first (full screen, statistics overlay, anisotropic
filtering) and sets `$POP_EXE`; `build/pop` runs without it. `$POP_EXE` takes the
.exe, its folder (a GOG install) or the Mac release's Cider `.app`; a path to the
`.exe` as the first argument works too. The game runs in its own folder. The game's
own files (saves under `My Documents\Prince of Persia\Save`, the registry) go to
`~/Library/Application Support/scimitar-recomp/pop`.

Environment: `SCIMITAR_POP_VOICES`, `SCIMITAR_POP_TEXT` (above), and the Windows
ports' `SCIMITAR_HIDDEN`, `SCIMITAR_MUTE`, `SCIMITAR_TRACE_WIN32`,
`SCIMITAR_KEYS`, `SCIMITAR_PAD`, `SCIMITAR_SCREENSHOT`, `SCIMITAR_FPS_LOG`
([AC1_WINDOWS.md](AC1_WINDOWS.md)), `SCIMITAR_AUDIO_DUMP` (above), and for
the renderer `SCIMITAR_D3D_TRACE_FRAME`, `SCIMITAR_D3D_TRACE_PARAMS`,
`SCIMITAR_DUMP_SHADERS`. For unattended runs set `SCIMITAR_HIDDEN=1
SCIMITAR_MUTE=1` and a scratch `HOME`, so your own save folder is untouched.
The game's own switches follow on the command line (its strings name
`language`, `mission`, `startpos`, `noconsole`, `msaa`, `shadows` and others;
the syntax is not worked out yet: `-language English` changed nothing).
