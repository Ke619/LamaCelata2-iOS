# Other Scimitar/Anvil games

Can this project become a base for running other games on the engine, given their PC
release? Static assessment of 2026-10-27; nothing was executed. Methods: PE and Mach-O
header parsing, `objdump` linear-sweep census of the code sections, `nm` symbol
comparisons, and scans of the `.forge` archives (index tables, `FILEDATA` records,
compressed-block headers, shader markers). The scripts were ad hoc and are not kept.

Since then AC1, Revelations and Prince of Persia (2008) have been recompiled and run:
see [AC1_WINDOWS.md](AC1_WINDOWS.md), [ACR_WINDOWS.md](ACR_WINDOWS.md) and
[POP_WINDOWS.md](POP_WINDOWS.md).

## The engine family

Scimitar was written for *Assassin's Creed* (2007) and renamed Anvil for *Assassin's
Creed II*. The first generation covers:

- **Scimitar**: Assassin's Creed (2007), Prince of Persia (2008), Shaun White
  Snowboarding (2008).
- **Anvil**: Assassin's Creed II (2009); Brotherhood, Prince of Persia: The Forgotten
  Sands, Shaun White Skateboarding, Your Shape: Fitness Evolved (2010); Revelations,
  Just Dance 3 and Michael Jackson: The Experience (360), Your Shape 2012 (2011).
  Brotherhood and Forgotten Sands use an improved revision; Revelations was the last
  game on the first generation.

The Mac executables still call themselves `scimitar_sf`, and Brotherhood's game code
still lives in the `scimitar::ac2` namespace (194,560 symbols, against AC2's 145,087).

## Builds on hand

Outside the repository, next to it:

| Game | Platform | Executable | Notes |
|---|---|---|---|
| AC1 | Windows (Ubisoft Connect) | `AssassinsCreed_Dx9.exe`, `AssassinsCreed_Dx10.exe` | Scimitar; D3D9 and D3D10 builds |
| AC2 | Mac (Steam 33362) | `scimitar_sf` | this project's target (an older executable than Steam's current one, see [MAC_RELEASES.md](MAC_RELEASES.md)) |
| AC2 | Windows (Ubisoft Connect) | `AssassinsCreedIIGame.exe` | `AssassinsCreedII.exe` is a small launcher |
| ACB | Mac (Steam 48193) | `scimitar_sf`, `acbmp_sf` (multiplayer) | newer Anvil revision; closest relative of ACR |
| ACB | Windows (Ubisoft Connect) | `ACBSP.exe` | single player only; `AssassinsCreedBrotherhood.exe` is a small launcher |
| ACR | Windows (Steam 201870) | `ACRSP.exe` | single player |
| Prince of Persia (2008) | Windows (GOG) | `Prince of Persia.exe` | Scimitar; includes the Russian audio |
| Prince of Persia (2008) | Mac (TransGaming Cider, 2009) | `Prince of Persia.exe` inside `Contents/Resources/transgaming/c_drive` | the Windows game in a Wine-based wrapper; English, French, German, Italian, Spanish and Russian voices |

## Mac and PC data (AC2)

- **Same container.** Both start with `scimitar`, container version 25, and have the
  same index layout; in the title-screen archive even the file offsets match. AC1,
  ACB (Mac) and ACR (PC) archives are version 25 too; Prince of Persia's are
  version 26, newer than any of the AC games'.
- **Different compression.** The type byte of the compressed blocks is 0 on Mac and 2
  on PC. In the engine's codec table (LZO1X_1, LZO1X_999, LZO2A, LZX) that is LZO1X_1
  against LZO2A. The Mac binary links `lzo2a_decompress`, so reading PC blocks should
  not need new code.
- **Different shaders.** Mac archives hold ARB assembly programs (17k `!!ARBvp1.0` and
  `!!ARBfp1.0` markers in `DataMAC.forge`); PC archives hold D3D9 shader bytecode
  (2,899 `CTAB` constant tables in `DataPC.forge`, none on Mac).
- **Mac archives are 30–120% larger** (`DataMAC.forge` 372 MB against 171 MB). The
  weaker LZO1X_1 compression is the likely main cause; not measured.
- **ACB is the same story.** Mac and PC ship the same 28 archives (the Mac build adds
  a 1.8 MB `_Ext` companion for each, plus `multi/` for multiplayer), container
  version 25 on both. Mac has ARB programs (23k markers in `DataMAC.forge`), PC D3D9
  bytecode (7,432 `CTAB`). ACB Mac blocks use type 1 (LZO1X_999) where AC2 Mac used
  type 0, and PC uses type 2 (LZO2A) for both games. `DataMAC.forge` is 525 MB
  against 429 MB on PC.
- **Locales differ**: AC2 Mac has Russian (text, subtitles and a `sounds_rus.pck`
  voice pack, as ACB Mac does); this AC2 PC copy has no Russian, but Chinese, Korean
  and Polish text instead.

So PC data in the Mac engine would at least need D3D9 shaders translated. That is
done for AC2: the Mac executable runs on the Windows release's archives, see
[PC_DATA.md](PC_DATA.md). It matters little by itself (the Mac AC2 and ACB already
have their own data), but the shader translation is a first piece of a Windows
executable's port (below).

## Executables compared

| | AC2 Mac | ACB Mac | AC2 PC | ACB PC | ACR PC | AC1 PC (Dx9) | PoP 2008 PC |
|---|---|---|---|---|---|---|---|
| Compiler | Apple GCC 4.x | Apple GCC 4.x | MSVC | MSVC | MSVC | MSVC | MSVC |
| Symbols | 308k | 407k | none (PDB not shipped) | none | none | none | none |
| RTTI type names | n/a (symbols) | n/a (symbols) | ~700, mostly libraries | 615, none `scimitar` | ~300 | 4,323 names, 2,582 of them `scimitar` (string scan; the PE reader finds 4,376 type descriptors, see AC1_WINDOWS.md) | 383, none `scimitar` |
| Base relocations | n/a (not PIE) | n/a (not PIE) | yes (1.4 MB) | yes (1.8 MB) | yes (1.8 MB) | none | none |
| Instructions (linear sweep) | 8.9M | 9.8M | 10.7M | 13.6M | 13.4M | 9.9M | 3.4M |
| x87 | 0.1% | 0.2% | 1.6% | 1.1% | 1.2% | 1.6% | 2.4% |
| `fs:` accesses (SEH frames, TLS) | 7 | 12 | 82,512 | 108,850 | 106,409 | 46,844 | 2,632 |
| `ret imm` (stdcall/thiscall) | 1,883 | 2,081 | 132,303 | 167,822 | 175,362 | 76,904 | 32,425 |
| Graphics | OpenGL + ARB | OpenGL + ARB | D3D9 (`d3dx9_42`) | D3D9 (`d3dx9_42`) | D3D9 (`d3dx9_43`) | D3D9 or D3D10 | D3D9 (`d3dx9_39`) |
| Input | IOKit HID | IOKit HID | DirectInput 8, XInput 1.3 | same | same | same | same |
| Saves / launcher | Orbit, 22 `MgOrbitdll*` C exports | Orbit, 21 `OrbitClient` C++ methods from a linked `libubiorbitapi_r2_loader.dylib` | — | no Orbit; Uplay R1 (`upc_r1_loader`, 8 `UPLAY_*`: start, events, product list) | Orbit, 22 `OrbitClient` C++ methods; `upc_r1_loader` (8) | `uplay32` (1) | — |
| Packing / DRM | — | — (code-signed) | none found | none found | none found | none found (an extra `.nidata` section) | none found in the game |

Census counts are approximate: a linear sweep also decodes data embedded in `.text`.

ACB Mac (`scimitar_sf`, SHA-1 `380d23b7d0bea1e71ed9d65a1ff230e0894bb310`) looks like
AC2 Mac grown by 10%: 590 imports (88 OpenGL, 16 IOKit HID), the same frameworks plus
a direct link to `libubiorbitapi_r2_loader.dylib` in `Contents/Frameworks`. Where AC2
`dlopen`s Orbit and resolves 22 C functions, ACB calls the loader's C++
`mg::orbitclient::OrbitClient`, `SavegameInfo`, `SavegameReader` and `SavegameWriter`
methods: the same API ACR imports on Windows (22 there, 21 here). A replacement
written for ACB's Orbit therefore covers ACR's too, apart from the calling
convention.

ACB PC is the Ubisoft Connect re-release: `ACBSP.exe` (PDB path
`F:\Projects\ACB\assassin\pc-rome\main\…\scimitar_f.pdb`) imports no Orbit at all.
Its only Ubisoft link is Uplay R1 (`UPLAY_Start`, `UPLAY_Update`, `UPLAY_Quit`,
`UPLAY_GetNextEvent`, two overlapped-operation calls and
`UPLAY_PRODUCT_GetProductList`/`ReleaseProductList`, i.e. ownership and DLC). There is
no multiplayer executable or `multi/` data. The Mac build's Orbit save calls have
no counterpart here, so saves are presumably plain files.

Main imports: AC2 PC has KERNEL32 184, WS2_32 41, USER32 35, `binkw32` 16, plus a few
each from ADVAPI32, SHELL32, dbghelp, WININET, WINMM, ole32 and iphlpapi. AC2 and ACR
import no audio DLL directly; AC1 and Prince of Persia import DSOUND and EAX
(Prince of Persia's `DARE.INI` configures Ubisoft's DARE sound engine on DirectSound 3D).

The Mac release of Prince of Persia (`steam_depot/Prince of Persia.app`, Cider build 2178,
v1.00.2178) is not a native port. TransGaming's Cider runs the Windows executable
under a Wine-based layer. The executable is a separate Windows build that Ubisoft made for it:
- **Identity:** SHA-1 `8243f4162c21054fffc329f1e48cb10dcd9acb6d`, linked 17 Dec 2008, PDB path
  `d:\dev\pc-pvlasie-pop0-mac\pop0mac\…`, build string `Dec 11 2008`.
- **Against the GOG executable** (12 Nov 2008, `d:\Anthill\pop0pc\…`, `Nov  7 2008`,
  Authenticode-signed):
  - The code is laid out a little differently (`.text` 0x9241ec against 0x923f1c).
  - It imports one more D3DX function, `D3DXFloat16To32Array`, probably for Cider's renderer.
  - Otherwise the imports are the same: 287 functions from 19 DLLs.
  - D3DX is only `D3DXGetShaderConstantTable`, `D3DXCreateTexture` and the half-float
    conversion, so there is no HLSL compiler at run time. Shaders come precompiled in
    `ekshaderspc.bin` (44 MB).
- **Data:** all 13 `.forge` archives the two copies share are byte-identical (container version 26). The Mac copy
  adds `DataPC_StreamedSounds{Eng,Fre,Ger,Ita,Spa}.forge`; the GOG copy has only the Russian one.
  They are identical because they are the GOG Russian download's files (dated 2 Dec 2008 in the Mac
  bundle too, where the Mac release's own files are from 6 Feb 2009): their localization settings
  support Russian alone, and the text is Russian only ([POP_WINDOWS.md](POP_WINDOWS.md)).
- **Our Windows layer** (AC1 and ACR) already covered all but 44 of the imports
  (2026-10-28; [POP_WINDOWS.md](POP_WINDOWS.md) has the list). The missing ones are registry writes, resources, window management, a few Bink entry
  points and the two D3DX helpers.

Prince of Persia's folder also holds `render3d.exe`, `render3d.dll` and `render3d.x86`:
UPX-packed helpers whose version resources name Akella (the Russian publisher) and
the original names `drender.*`. The game executable imports none of them; they
probably came with the Russian release, and the port would not need them. The
executable is the smallest of the set (`.text` 9.6 MB), with few SEH frames.

## What a Windows port would reuse

The way to run another game is to recompile **its own Windows executable**; PC data
fed to the Mac AC2 or ACB executable does not help, because each game's code is in
its executable.

Carries over: the x86 → C++ translator (x87 included), the guest-memory runtime, the
Metal renderer's infrastructure and the ARB → MSL experience, audio output, input
(including the virtual 360 pad), Bink through FFmpeg, local Orbit saves (ACR asks for
the same operations through a C++ ABI), the windowing shell, and what we know about
the engine and the forge format.

New for each Windows game:

1. **PE front end and function discovery without symbols**: recursive descent from
   the entry point, call targets, relocations (AC2, ACR), RTTI and vtables (AC1).
2. **MSVC code in the translator**: `thiscall`/`stdcall`/`fastcall`, SEH frames
   through `fs:[0]` (a fake TEB is probably enough if the game never throws, as on
   Mac), TLS, heavier x87 use.
3. **A Windows platform layer**: Win32 (~190 KERNEL32 functions, USER32), Direct3D 9
   → Metal including D3D9 shader bytecode → MSL (the largest piece), DirectInput and
   XInput, Winsock, Uplay stubs.

Alternatives exist today (CrossOver or Wine with D3DMetal probably run AC1 and ACR on
this Mac). The recompiled route costs far more; what it adds is native arm64 code and
a path to iOS.

### Suggested order

(Written before the ports. The order taken was AC1, Revelations, then Prince of
Persia; AC2 PC was not attempted.)

1. **AC2 PC** first, although AC2 already runs: same game, data and behaviour, with
   the Mac port as the oracle. The Windows pieces get tested without new-game
   unknowns.
   ACB PC pairs with ACB Mac the same way, and is the closer step towards ACR.
2. **ACR**, with ACB Mac as the source of names.
3. **AC1**: the older Scimitar revision and no relocations, though its RTTI helps.
4. **Prince of Persia (2008)**: the smallest executable, but no relocations, no
   engine RTTI and no Mac relative to borrow names from.

## How much the Mac symbols matter

Without symbols, the translation would be harder but still mechanical:

- Imports keep their names in a stripped Mach-O, so the platform layer is unaffected.
- `eh_frame` alone gives 127,232 of the 208,730 function starts; GCC keeps frame
  pointers, so call targets, addresses in data and a `push %ebp; mov %esp, %ebp` scan
  should recover most of the rest.
- Vtables would come from scanning read-only data for runs of code pointers, instead
  of from `__ZTV…` symbols.

Understanding the game is what gets much harder. Nearly every runtime fix so far
started from a name (`MacSetApplicationActive`, `BigFile`/`CompressBinStream`,
`GetPlatformName`, the render thread's math, the developer options). A stripped AC2
would keep little else: no RTTI names for engine classes, about 460 source paths in
assert messages and about 120 function-name strings.

The same holds for the Windows builds, where the Mac binaries can act as a map: match
AC2 Mac against AC2 PC and ACB Mac against ACR to carry names across. Matching across
two compilers is inexact, but shared strings, constants and call structure take it far.
A game with neither a Mac relative nor RTTI is the hardest case: Prince of Persia
(2008), which we have, and Forgotten Sands. Prince of Persia's names came through AC1
and Scimitar class IDs instead ([POP_WINDOWS.md](POP_WINDOWS.md)).

## Running AC2 data on the ACB executable

Not expected to work, and it would bring few of ACB's improvements:

- About 91% of AC2's names under `scimitar::ac2` (22,857 of 25,093 first components)
  also exist in ACB, but 2,236 do not: mostly AI and mission state machines (horse
  intercepts, shopkeepers, blocking guards, beat-up and kill-target missions, chases,
  cannons). AC2's data refers to such classes by type, and a missing type fails the
  load.
- Classes that kept their names have probably changed layout, and objects are stored
  in their game's layout; the shared container version says nothing about that. Not
  checked: how far the serializer's versioning tolerates it.
- Startup is game-specific (world list, menus, multiplayer, archive names).
- Much of what makes ACB better is in its data (lighting, LODs, streaming layout,
  shaders). Code-side improvements could be backported one by one as overrides.

The renderer is ours now, so resolution, frame rate, upscaling and shadow filtering
can be improved for both games without either engine's help.

A cheap experiment, not tried yet: point the ACB port at AC2's archives and log the
first missing type.

## Open questions

- How AC2 on PC reaches audio (no audio DLL imported). Revelations' Wwise creates
  DirectSound 8 through `CoCreateInstance` ([ACR_WINDOWS.md](ACR_WINDOWS.md)); AC2's
  probably does the same.
- What changed in forge container version 26 (Prince of Persia) against 25.
- How ACR's archives compare with ACB's beyond the root `World` object, whose
  FastLoad layout differs ([ACR_WINDOWS.md](ACR_WINDOWS.md)).
