# Assassin's Creed Revelations (Windows) on this Mac

Can the project run *Assassin's Creed Revelations*, of which only the Windows
release is at hand (Steam app 201870, `ACRSP.exe`), on arm64 macOS? Started
2026-10-28. The shared Windows pieces (PE front end, MSVC code in the
translator, Win32 layer) are built together with the AC1 port
(`AssassinsCreed_Dx9.exe`); see the git log for who did what.

## The executable

`ACRSP.exe`, 43,674,664 bytes, SHA-1 `3fd192a32e81473a2c8b107d23864cf776272d4e`.
PE32 i386, GUI subsystem, large-address-aware, `DYNAMIC_BASE` + `NX_COMPAT`,
preferred base 0x400000. Linker 9.0 (Visual C++ 2008); header timestamp
2022-11-23 and an Authenticode signature by Ubisoft from 2022 (a re-signed
re-release; the code is 2011's). PDB path
`e:\Work\assassin\assassin\acfe\pc\output\win32\exe\scimitar_f.pdb` (not shipped).
The export directory names the module `scimitar_f.exe`.

| Section | Address | Size | |
|---|---|---|---|
| `.text` | 0x00401000 | 0x1f202cf (31.1 MB) | code; MSVC puts switch tables here too |
| `.rdata` | 0x02322000 | 0x43cb28 | constants, vtables, RTTI, IAT at its start |
| `.data` | 0x0275f000 | 0x227e00 | |
| `.tls` | 0x029ff000 | 0x89 | 136 bytes of static TLS template, no callbacks |
| `.data1` | 0x02a00000 | 0x5f0 | |
| `.rsrc` | 0x02a01000 | 0x25fedc | icons, dialogs, version |
| `.reloc` | 0x02c61000 | 0x1bef6e | 862,811 `HIGHLOW` relocations |

Load configuration: a security cookie, and a SafeSEH table of **34,150**
exception handlers (all code addresses: a free source of function starts). No
CFG. TLS index at 0x029f004c.

### Relocations

The image can be loaded at its preferred base, so relocations are not needed to
run it; they matter because they say **which words are addresses**:

| Site → target | Count | What they are |
|---|---|---|
| `.text` → `.data` | 257,425 | globals |
| `.rdata` → `.text` | 244,886 | vtables, SEH tables, function pointer tables |
| `.text` → `.rdata` | 136,308 | constants, strings, vtables taken in code |
| `.data` → `.data` | 87,788 | |
| `.text` → `.text` | 57,600 | code addresses taken in code, and **jump tables inside `.text`** |
| `.rdata` → `.rdata` | 38,214 | RTTI, nested tables |
| `.data` → `.text` | 30,506 | callbacks, initialized function pointers |

222,441 distinct code addresses are relocation targets (171,675 referenced from
data). Without relocations (AC1, Prince of Persia) any data word that falls in
the code range is a candidate function start, and decoding such words as code is
what leaves the analyzer with `in`/`out`/`aas`/`iretd` "instructions" (below).
With them, a code address in data is certain, and relocation sites inside
`.text` that are not an instruction's operand are data embedded in code (MSVC's
switch tables), which must not be decoded.

### Imports (322, by DLL)

| DLL | # | Notes |
|---|---|---|
| KERNEL32 | 192 | files (incl. overlapped `ReadFileEx`, completion ports), threads, critical sections, events, semaphores, TLS, heaps, `VirtualAlloc`, INI files (`GetPrivateProfile*`), locale/code pages, toolhelp snapshots, `RaiseException`/`RtlUnwind` |
| USER32 | 31 | one window class, message pump, `SetWindowsHookExA` (keyboard hook), display enumeration, `MessageBox` |
| WS2_32 | 28 | 27 by ordinal + `WSAIoctl` (Quazal; no servers exist) |
| ubiorbitapi_r2_loader | 22 | the `mg::orbitclient` C++ client (MSVC `thiscall`): the same methods ACB Mac imports, plus `GetLoginDetails`; saves go through it |
| binkw32 | 16 | stdcall (`_BinkOpen@8`...); same functions as the Mac build plus `BinkOpenDirectSound`, `BinkSetSoundSystem`, `BinkCopyToBufferRect`, `BinkGetFrameBuffersInfo` |
| upc_r1_loader | 8 | Uplay R1: `UPLAY_Start`, `Update`, `Quit`, `GetNextEvent`-style overlapped results, product list, `UPLAY_ACH_Write` (achievements) |
| ole32 | 5 | COM: `CoCreateInstance`, which Wwise uses for DirectSound 8 (XAudio2 2.7's CLSID is in `.rdata` too, but unused at run time), WMI (`WbemLocator`) |
| ADVAPI32, SHELL32 | 4 + 4 | registry reads, `SHGetFolderPathA` (save/settings folder), `ShellExecuteA` |
| XINPUT1_3 | 3 | by ordinal: `XInputGetState`, `SetState`, `GetCapabilities` |
| dbghelp | 3 | stack walks for crash reports (stubs) |
| d3dx9_43 | 2 | `D3DXGetShaderConstantTableEx`, `D3DXCheckTextureRequirements` |
| OLEAUT32 | 2 | `SysAllocString`, `SysFreeString` (WMI) |
| d3d9, DINPUT8 | 1 + 1 | `Direct3DCreate9`, `DirectInput8Create`: everything else is COM vtables |

The C runtime is linked statically (no `MSVCR*` import; the `msvcr100d.dll` in
the folder is not used). Loaded by name at run time: `nvapi.dll`
(`nvapi_QueryInterface`), `atiadlxx.dll`/`atiadlxy.dll` (`ADL_Main_Control_Create`),
`dbghelp.dll` (`SymInitialize`, `MiniDumpWriteDump`): all optional GPU and crash
helpers that may fail.

The executable exports 169 functions, all Wwise (`AK::SoundEngine`,
`MusicEngine`, `StreamMgr`, `MemoryMgr`...): free names for the sound engine.

### Graphics, audio, input

- **Direct3D 9 only.** `Direct3DCreate9` plus two D3DX helpers; no D3D10/11 or
  DXGI anywhere in the binary. The archives hold D3D9 shader bytecode, the same
  kind the AC2 PC-data work translates (`platform/src/d3d9_arb.cpp`).
- **Audio:** the binary holds XAudio2 2.7's CLSIDs (`5a508685-…`, the debug one)
  and `IID_IXAudio2`, but at run time Wwise creates DirectSound 8 through
  `CoCreateInstance` (CLSID_DirectSound8); the port answers it with the shared
  DirectSound (`platform/win32/acr_misc.cpp`). Bink videos use DirectSound too.
- **Input:** DirectInput 8 (keyboard, mouse; `IID_IDirectInput8A/W` in
  `.rdata`), XInput 1.3 for pads, and a low-level keyboard hook.

### DRM and launcher checks

No Steam API (no `steam_api.dll`, no SteamStub `.bind` section), no packer.
The checks are Ubisoft's: `UPLAY_Start` through `upc_r1_loader.dll` (Uplay R1;
the strings `IsUplayEnabled`, `UplayPassport*` suggest the old Uplay Passport
flow), and the Orbit client, whose `StartLauncher`/`GetInstallationErrorNum`
are the launcher handshake ACB Mac also makes. Both are imports, so the platform
layer answers them: an offline Uplay that owns the base game, and the local
Orbit of `platform/src/orbit.cpp`.

## Route 1: ACR's data through the Mac Brotherhood executable

Tested 2026-10-28 with a bundle of symlinks (the ACB Mac app with ACR's
`DataPC*.forge` linked as `DataMAC*.forge` and ACR's videos), run hidden and
muted: `build/acb` starts, opens `DataMAC.forge` and dies in its loading thread
on the first object, an indirect call through a null vtable slot in
`FastLoadSerializer::SerializeObject`, reached from `MaterialTemplate::FastLoad`
→ `MetaOperator` → `LightingStack::FastLoad`. The stream was misread.

Two reasons, each enough on its own:

1. **The objects' layouts differ between the games.** FastLoad records are the
   fields in order with no tags or versions. The root `World` object of the
   title-screen archive, ACB PC against ACR PC (the same container, both
   Windows): the same fields mostly, but ACR drops a 4-byte field at +0x3a,
   changes one at +0x1d0, adds an object reference at +0x260 and adds fields at
   the end. Every class Ubisoft changed in that year would need a converter,
   and classes ACR added have no code in ACB at all.
2. **The PC records ACB's engine would need rewritten** (as AC2's
   `pc_data.cpp` does for AC2) **differ from AC2's.** Wired into ACB and run on
   ACB's own PC data, the AC2 converter fails on the first shader: ACB's
   `ShaderPermutation` has 25 flag bytes instead of 15, loads each material
   parameter through its own FastLoad, adds a byte array and ends with
   `CustomSerializeFastLoad`. (That experiment was reverted.)

Running AC2 on the Windows release's data worked because Mac and PC AC2 are the
**same build**: every class has the same layout and only platform records
(shaders, Havok packfiles, one mesh field) differ. Between two games a year
apart that premise fails. What the AC2 PC-data work does give route 2 is the
D3D9 bytecode translator and knowledge of the PC formats.

## Route 2: recompiling `ACRSP.exe`

### Function discovery and translator coverage

With the PE front end built for AC1 (in progress then, 2026-10-28) the analyzer finds
199,951 function starts by recursive descent from the entry point, exports,
RTTI vtables and code addresses found in data, and translates 197,088 of them
(98.6%; 95.5% of 7.68M instructions) on their own. What stops the rest:

| Blocker | Functions | Real? |
|---|---|---|
| cannot decode, overlapping instructions, `in`/`out`/`aas`/`daa`/`iretd`/`insb`/`cli`/`sti`/`salc`/`int1`/`into`/`xlat`/`les`/`lds`/`bound`, segment registers `es`/`ds`/`ss`/`cs` | ~2,400 | no: data decoded as code, from words that only look like code addresses. Relocations remove them |
| branch leaves the function, call to a non-function address, jump table entry outside | ~260 | partly: functions split at a false start in the middle |
| `loop`/`loope`/`loopne`/`jecxz`, `enter` | ~190 | yes, in the CRT's assembly (`memcpy`, `strlen`...) and hand-written code; cheap to add |
| `pextrw`, `pinsrw` | 22 | yes (SSE2) |
| `fs:` operands | (counted in the census) | yes: 106k accesses, SEH frames (`fs:[0]`) and TLS (`fs:[0x2c]`) |

A linear sweep (docs/ANVIL_FAMILY.md) counts 13.4M instructions: x87 1.2%
(heavier than the Mac builds' 0.2%, mostly MSVC's float returns and
conversions), 175k `ret imm` (stdcall/thiscall), 106k `fs:` accesses.

### Matching to ACB Mac (names)

The symbolized ACB Mac binary can name ACR's functions where the two compiled
the same source. Cheap anchors, measured 2026-10-28:

- **Strings:** ACB's `__cstring`/`__const` hold 12,982 strings, ACR's
  `.rdata`/`.data` 12,556; 8,723 are in both. A string referenced by exactly one
  function in each binary pairs those functions: **654** ACR functions.
- **Immediates** (32-bit constants outside the address ranges, from an
  `objdump` sweep of both): 7,602 values sit in exactly one function in each;
  they anchor **1,321** ACR functions, 711 of them by two or more agreeing
  constants. Most are statechart registrations, fight and navigation code,
  the AI's `StatechartRegister` functions, and Havok.
- **Names for free:** 169 Wwise exports, ~300 RTTI type names (libraries),
  322 imports, and the 22 Orbit methods.

So about 2,000 functions are named from cheap anchors. Propagating along call
graphs (the n-th call of a matched function to the n-th call of its partner,
vtable slot k to slot k of a matched vtable) should reach far more, but across
two compilers it is inexact; it is a tool to build, not a number yet. Names are
for understanding and for finding the functions the port overrides; the
translation does not need them.

### Work estimate per piece

Rough guesses for one developer made before the work, not measurements; shared pieces (marked *) are
shared with the AC1 port and counted once for both.

| Piece | Estimate | Notes |
|---|---|---|
| PE loader, discovery, relocation-driven code/data separation* | days | AC1's front end exists; relocations and the SafeSEH table are ACR's addition |
| Translator: `fs:` (fake TIB per thread), thiscall/fastcall/stdcall at host boundaries, `loop`/`jecxz`/`enter`, `pextrw`/`pinsrw`, x87 compares* | 1 week | SEH needs a real dispatcher only if the game raises exceptions; on the Mac nothing threw |
| Full image translating and compiling | 1-2 days | the AC2/ACB pipeline (64 parts) |
| Win32: KERNEL32/USER32/ADVAPI32/SHELL32/ole32* | 1-2 weeks | handles, files, threads, sync, heaps, INI files, window + message pump → the Cocoa shell |
| Direct3D 9 → Metal | 4-8 weeks | the largest piece: device and resource COM objects, render states, D3D9 bytecode → MSL (via the existing ARB path or directly), `D3DXGetShaderConstantTableEx` |
| XAudio2 2.7 → CoreAudio | 1 week | (as estimated; at run time Wwise used DirectSound 8 instead, shared with AC1) |
| DirectInput 8, XInput → keyboard/mouse/virtual pad | 3-5 days | |
| Bink (stdcall imports) | 1-2 days | `platform/src/bink.cpp` behind stdcall shims |
| Orbit (thiscall) and Uplay R1 offline | 2-3 days | over `platform/src/orbit.cpp` |
| Winsock, nvapi/ADL/dbghelp/WMI | 1-2 days | fail cleanly: offline, no vendor GPU APIs |

First window: about three weeks of the above (translation, Win32 core, window
creation). First rendered frame: plus the D3D9 device. Playable: two to three
months. (Those were the estimates; with AC1's Windows layer and renderer done,
Revelations reached gameplay on 2026-10-28.)

## Status

Current: menus, voiced videos, Story Mode up to gameplay on Animus Island, from
`ACR.app` (the launcher) or `acr`. Known: the menu background blinks, worked
around by reporting one processor (below). Later parts are untested.

### 2026-11-06 (night): rendering glitches

The author played it from ACR.app and saw flicker, wrong colours and textures,
and an offset or wrong-size image. Three Direct3D 9 on Metal faults were behind
the gameplay ones. All three are shared with AC1 and PoP, and both still render
afterwards.

1. **A haze band across the gameplay view** (`fix1_fog_layer_depth_before_after.png`).
   The game writes linear depth into an R32F second render target through a
   `COLOR` output of a `vs_3_0` vertex shader. Its fog layer then reads that
   depth back. Our MSL clamped every colour varying to 0..1, as GL and pre-SM3
   D3D9 do, so all depth past 1 m saturated. Shader model 3 does not clamp
   colour outputs, so the clamp now applies only below vs_3_0
   (`ArbOptions::clamp_colors`).
   Fixed with it: D3D9's `vPos` is the integer pixel coordinate. Metal's
   `[[position]]` is the pixel centre (k + 0.5), so fragment programs now
   subtract 0.5 (`ArbOptions::integer_pixel_centers`).
2. **A bright border on the right and bottom of the frame, and the image shifted
   by half a pixel** (`fix2_half_pixel_edges_before_after.png`). D3D9 pixel k
   is Metal's k + 0.5, so D3D9 positions have to move half a pixel *right and
   down* in clip space. The vertex epilogue moved them left and up. Each
   full-screen pass then sampled half a texel off, and the bloom chain
   (320x180 down to 20x11) compounded it into a glowing edge. The offset now
   uses Wine's 63/64 of a half pixel, in the right direction.
3. **Depth dropped when the depth buffer is larger than the render target**
   (`fix3_pop_1728x1117_depth_before_after.png`). D3D9 allows a depth buffer
   at least as large as the colour target. Metal accepts mismatched attachment
   sizes (checked with a test program under `MTL_DEBUG_LAYER`). The backend dropped depth on
   any size mismatch. That showed most clearly in PoP at window sizes whose
   height the game rounds down (a 1728x1112 target with a 1728x1117 depth
   buffer), as a washed-out frame. Depth is now dropped only when it is
   smaller than the target.

Not a bug: the black strand over the left of the session menu is game
content. The 1024x64 and 1024x128 DXT3 textures it draws decode to black RGB
with an alpha mask.

New opt-in debug aids:
- `SCIMITAR_D3D_DUMP_PASSES=dir` writes every render pass's targets as PPM, or
  raw `.f32` for R32F.
- `SCIMITAR_D3D_SKIP_TEX=WxH` skips draws whose texture 0 has that size.
- `SCIMITAR_DUMP_RAW_TEXTURES=dir` dumps the raw compressed uploads.
- `SCIMITAR_D3D_TRACE_FRAMES=K` traces K frames from `SCIMITAR_D3D_TRACE_FRAME`.

**The menu background blink** (`fix4_menu_blink_before_after.png`), still
open since 2026-10-28, depends on how many processors the game is told about.
With one (`GetSystemInfo`, `GetProcessAffinityMask`) the menu is steady. With
two or four it switches between two projections from frame to frame: field of
view about 46° (the intended "data columns" view) or about 109°. The game
submits both from the same call path on its render thread; the Direct3D side
draws what it is given.

What it is not:
- Not our synchronisation shims. The menu uses only events, critical
  sections and single waits: no wait-all, `PulseEvent` or semaphore traffic,
  and no semaphore release goes past its maximum.
- Not ARM's weaker memory ordering alone. A build with a barrier around every
  guest load and store (x86-like ordering) still varies from frame to frame,
  only in a different pattern.

So it is a race in the game's multi-processor path, which our thread timing
exposes and real Windows machines evidently do not. `acr` now reports one
processor by default (`games/acr/main.cpp`). Gameplay runs at about the same
frame rate that way: 25 to 27 fps hidden, either way.
`SCIMITAR_WIN32_CPUS=N` overrides the count, for AC1 and PoP as well; their
default stays four.

### 2026-10-28 (afternoon)

The author played it: the menus blinked, the videos had no voices, the 3D was
broken (missing textures, characters showing only their eyes). With a save
from the beginning, an unattended run now loads Animus Island from Story
Mode: Desmond whole, the island, fog and grass, steady from frame to frame.
Fixed on the way:

- Videos: Revelations' Bink files carry ten audio tracks (music and effects,
  then a voice track per language) and the game picks several; all picked
  tracks are now mixed (`platform/src/bink.cpp`).
- Direct3D 9: recorded state blocks (the game resets its states with two of
  them, thousands of times a minute), buffers and textures renamed when
  rewritten after a recorded draw used them, boolean constants moved past the
  float registers (bone palettes reach c255).
- Vertex shaders: writes inside `if` blocks merged with a select (`CMP`, MSL
  `select`) instead of `x + mask * (new - x)`, which turned an infinity in
  the branch not taken into NaN (`ArbOptions::vertex_cmp`, Metal only; ARB
  vertex programs for OpenGL have no CMP).
- `QueryPerformanceCounter` at 10 MHz from process start (nanoseconds since
  boot overflowed the game's `ticks * 1000000 / frequency`).
- With a game pad attached the game asks DirectInput for rumble effects:
  `CreateEffect` now says unsupported instead of aborting.

Still open (worked around on 2026-11-06, see above): **the menu background blinks** between a few brightness levels
(the gameplay does not). It is not the renderer: frame-wide traces with every
constant show the game itself submitting different data on the blinking
frames (one pass's vertex constant alternates irregularly between two
values, 0.849 and 0.258, frame to frame), and some menu frames upload NaN
in a 2x2 texture-coordinate matrix. Not the x87 stack (no underflow), not
SSE NaN rules, not atomics (locked instructions and the Interlocked imports
are atomic). (The 2026-11-06 findings above rule out our synchronisation
shims.)

Tools for this, all opt-in: `SCIMITAR_D3D_TRACE_FRAME`,
`SCIMITAR_SCREENSHOT` with `_EVERY`, `_FROM` and `_COUNT`.

### 2026-10-28 (morning)

**Revelations plays visibly on Metal**: the Ubisoft logo, the disclaimer, the
title screen ("Press Enter"), the game's own "Offline: the online
multiplayer service is not available anymore" notice, the Story Mode
session menu, the "Previously on" video and the start of the first memory
("Loading firmware"), driven by Return presses (`SCIMITAR_KEYS`), hidden and
muted, without a crash.

What it took after the night: AC1's Direct3D 9 renderer fixes (StretchRect
scaling, NULL render targets, Reset keeping the back buffer, the device holding
references on what is bound to it), and Windows' full-screen behaviour: a
full-screen `CreateDevice` or `Reset` sizes the focus window to the back buffer
and the game gets `WM_SIZE`. Revelations creates its window at 1280x720, sizes
its render targets from `GetWindowRect`, and with the window left at 1280x720
it composited into a 1280x720 target of its own and never drew to the
1920x1080 back buffer: every frame after the disclaimer was black.

Open then: a black smear over the left of the session menu; sound untried (muted
runs); play past the first memory's start. (All since resolved: the smear is the
game's own, videos play with voices, and play reached Animus Island; see the later
sections.)

### 2026-10-28 (night, later)

`build/acr` and `build/ACR.app` (`cmake -DSCIMITAR_FULL_IMAGE=ON -DSCIMITAR_GAMES=acr
-DACR_EXE=/path/to/ACRSP.exe`, target `acr`; or with the other ports) builds from the
repository: the executable
translated whole in 64 parts, linked with AC1's Windows layer and ACR's own
pieces (7 minutes to build). Run from the repository's root like the other
ports; it finds `ACRSP.exe` at `../windows-releases/Steam/Assassin's Creed
Revelations/` (or `$ACR_EXE`, the .exe or its folder, or a path to the `.exe` as the first argument).

How far it gets, unattended (`SCIMITAR_HIDDEN=1 SCIMITAR_MUTE=1`,
`SCIMITAR_KEYS` pressing Return): the window, the Direct3D 9 device on Metal,
the Ubisoft logo and the disclaimer (the first frame drawn by the game shows
the disclaimer's text), the title screen; Return goes on to the menus: the
game scans its downloadable content archives, reads `version.bin`, loads its
profile through Orbit (it wrote `acr/saves/p40-1.sav` and its settings INI on
an earlier run) and starts the "Previously on" video; no crash in two and a
half minutes. Everything after the disclaimer is black: the game draws 60 to
110 calls a frame in the menus, all its shaders translate, but nothing shows
yet (under investigation in the Direct3D 9 renderer, which AC1 brought up
for its own title screen the same night). Sound: DirectSound is mixed to the
host (AC1's), untried with sound on.

### 2026-10-28 (night)

How far it got first: the window, the Direct3D 9 device, the Ubisoft logo and
the disclaimer videos, the title-screen archives and the main loop (51,600
frames and 3.4 million draw calls in two minutes, no crash), before the
renderer drew anything.

The run, step by step (`SCIMITAR_TRACE_WIN32=1`): the MSVC C runtime and the
static initializers; `WinMain`; the special folders, `localization.lang`, the
INI and registry lookups (absent); eight engine threads; Wwise opens the 22
PC sound packs; `DataPC.forge` and `DataPC_extra.forge`; DirectInput keyboard
and mouse; `CreateWindowExA("Assassin's Creed Revelations")` from an engine
thread; `DataPC_ACFE_Masyaf.forge` through an I/O completion port;
`Direct3DCreate9`, `CreateDevice` (1920x1080), format checks, shader
constant tables; the GPU vendor libraries (absent); `ubi_logo.bik`;
`DataPC_WhiteRoom.forge`, `DataPC_TitleScreen.forge`; device `Reset`;
`warning_disclaimer.bik`, `enddemo.bik`; the main loop.

- **Function discovery with relocations.** With AC1's PE front end
  (commit 58f49bb) plus relocation-driven discovery (code addresses only from
  relocated data words outside code and relocated immediates; now in
  `recomp/src/flow.cpp`, for images that have relocations): 274,540
  functions, **100.0% translatable**, 8.18M instructions. Without relocations
  the same front end found 188,802 functions and 6.5M instructions: code
  reached only through vtables was missing (the C runtime stores the address
  of a `TlsAlloc` wrapper that follows a `ret`, without padding, and calls it),
  and ~200 "functions" were data decoded as code. The one left as a stub: an
  MMX `movntq` copy loop. AC1's later discovery (runs of code addresses in
  data) finds 254,302 but misses the C runtime's x87 dispatch labels, stored
  unaligned in `.data` (the game calls one at its title screen).
- **The whole image translates and compiles**: 37 s to translate, about
  5 minutes to compile at `-O2` on 10 cores, 179 MB of objects (the `acr`
  target now uses `-O1 -gline-tables-only`, `SCIMITAR_ACR_GEN_OPT`).
- **Translator fixes** (both PE only): a function whose entry jumps back to
  code below it started at the lowest address instead of its entry (a static
  initializer ran its constructor with `ecx` = 0); MSVC's backward `memcpy`
  jump table (`cmp $8, %ecx; jb L` ... `L: neg %ecx; jmp *T(,%ecx,4)`, the
  eight words ending at T) was read forwards.
- **ACR's imports beyond AC1's** (`platform/win32`): `orbit_msvc.cpp` (the 22
  `thiscall` Orbit methods over the Mac ports' `orbit.cpp`, listeners through
  proxies, UTF-16 names; saves in `scimitar-recomp/acr/saves`), `uplay.cpp`
  (Uplay R1 offline: started, owned, no DLC), `kernel32_acr.cpp`
  (wide-character twins over the ANSI functions, `ReadFileEx`/`SleepEx`
  completion routines, I/O completion ports, file mappings, toolhelp),
  `acr_misc.cpp` (USER32, SHELL32, ole32, registry, dbghelp, Winsock, D3DX's
  texture requirements and `D3DXGetShaderConstantTableEx`, DirectSound 8 as a
  COM class), `games/acr/main.cpp`.
- **Fixes in the shared Windows layer**: USER32's AppKit work runs on the host
  main thread (Revelations creates and pumps its window on an engine thread,
  so its guest main thread runs on a host thread of its own); overlapped reads
  on a file tied to a completion port queue their packet; DirectInput's
  keyboard and mouse GUIDs were compared wrongly (every device was refused);
  a texture's level surfaces now share the texture's reference count.
- **Names from ACB Mac** (scripts outside the repository): 1,932 seed pairs
  from unique strings and constants, extended along call graphs and through
  vtables (GCC's two destructor slots folded to MSVC's one): about 5,200 ACR
  functions and 480 vtables named. Recovering held-out seeds, every match was
  correct (42 of 42), but recall is low: most functions are reached only
  virtually, inlining differs between the compilers and many vtables changed
  length between the games. Aligning unequal call lists around matched callees
  names 7,639 but only 79% correctly. Enough to name `Engine::Initialize` and
  `Initialize_BootStrapEngine` in backtraces.

**The archives' shaders** (extracted from the 22 archives' decompressed
data: length-prefixed D3D9 programs): 12,728 distinct, 1,982 `vs_3_0` and
10,742 `ps_3_0` (plus four `ps_1_x`/`ps_2_x`). They use five bool constants:
`g_VolumeFogEnabled` (b3, 10,285 shaders), `g_UseDepthTextureShadows` (b15 in
pixel shaders, 8,399), `g_DeconstructionEnabled` (b15 in vertex shaders,
1,982), `g_SpritesReferencePointEnabled` (b2), `g_HasSun` (b0). The D3D9 →
ARB → MSL path of the AC2 PC-data work maps bool registers to float
registers (`ArbOptions::bool_registers`); with AC2's name-based mapping
(`d3d9_arb_check`) only 2,389 translate, but with every bool register given a
float slot of its own, as a Direct3D 9 device that copies its bool constants
there would do, **12,696 of the 12,728 translate, compile with this Mac's
OpenGL, and convert to MSL that Metal compiles**. Refused: 28 with input usage 5 and four with a
relative destination register.

Next then: the Direct3D 9 renderer (shared with AC1; the D3D9 bytecode translator
from the PC-data work and the Metal backend are its parts), the host mix for
DirectSound, XInput through the virtual pad, then the title screen and menus.
(All done since: see the later sections.)
