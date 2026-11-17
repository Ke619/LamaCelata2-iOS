# Assassin's Creed (2007) on this Mac: the Windows executable, recompiled

Assessment and status of running the Windows release of the first game (Ubisoft
Connect, `../windows-releases/Ubisoft Launcher/Assassin's Creed 1/`) on arm64 macOS
with this project's recompiler. Started 2026-10-28. Nothing from the game enters
the repository; generated code stays in the build directory.

## Two routes

**1. AC1 data in the Mac AC2 executable: ruled out.** `tools/pc_bundle.sh` makes an
AC2 bundle whose `DataMAC*.forge` are AC1's `DataPC*.forge`. The AC2 port starts,
reads the sound packs and opens `DataMAC.forge`, then dies (SIGBUS) in
`scimitar::BigFileAsynchStream::RequestPrefetch` under `HandleChunkPrefetches` and
`AsynchCallBack` on the I/O thread, on the archive's first file (`GlobalMetaFile`).
The container header is the same as AC2's (`scimitar`, version 25), but the
FILEDATA record before each file's data is 4 bytes shorter in AC1 (compressed-block
magic at +0x9b8, against +0x9bc in AC2) and AC1's blocks are LZO1X_999 (type 1).
Patching the reader would only move the failure: the next objects (`GlobalMetaFile`,
`Game Bootstrap Settings`, `Game Fix`) are AC1 classes in AC1's layouts, and
AC1's game logic is not in AC2's executable ([ANVIL_FAMILY.md](ANVIL_FAMILY.md)).

**2. Recompile AC1's own executable** with the translator, behind a Win32
platform layer. This is the route taken; the rest of this document is about it.

## The executable

`AssassinsCreed_Dx9.exe` (the Direct3D 9 build; the Dx10 build is the same game on
Direct3D 10 and is not targeted):

| | |
|---|---|
| SHA-1 | `b3fff1737cf77bc3ef5bb0ede6905b40c8f4e476`, 24,671,912 bytes |
| Built | 2008-04-23 (PE time stamp), MSVC 8.0 (Visual Studio 2005) linker, internal name `scimitar_f.exe` |
| Layout | PE32, image base 0x400000, **no base relocations** (fixed address, like the Mac builds), entry 0xf9f445 (CRT `WinMainCRTStartup`) |
| Packing / DRM | none: the code is plain. Ubisoft Connect added an import directory in a new `.nidata` section that repeats the original one plus `uplay32.dll!Test`; nothing calls `Test`, so loading that DLL is the whole check, and the port skips it |
| Debug | `/GS` security cookie at 0x19ca838 (load config), no SafeSEH table |

Sections:

| Section | Address | Size | |
|---|---|---|---|
| `.text` | 0x00401000 | 19.4 MB | code; a 3.5 MB run of `int3` at 0x11b5239–0x1520860 that nothing references, then the C++ unwind funclets |
| `.rdata` | 0x0167c000 | 2.3 MB | constants, vtables, RTTI locators, CRT init tables |
| `.data` | 0x018c2000 | 1.6 MB (1.3 MB in the file) | |
| `.idata` | 0x01a47000 | 9 KB | import tables; the IAT is here |
| `.tls` | 0x01a4a000 | 673 bytes | static TLS template (index at 0x1a3561c, no callbacks) |
| `_RDATA`, `.data1` | 0x01a4b000 | 11 KB | |
| `.rsrc` | 0x01a4f000 | 1.5 MB | icons, dialogs |
| `.nidata` | 0x01bbd000 | 4 KB | the Ubisoft Connect import directory |

**Imports**: 261 from 17 DLLs. KERNEL32 157 (files, threads, events, semaphores,
critical sections, TLS, heaps, virtual memory, time, locale and console functions of
the statically linked CRT, INI files), USER32 32 (one window, messages, cursor,
keyboard state, a low-level keyboard hook, `MessageBoxA`), WS2_32 30 (by ordinal),
`binkw32` 14, ADVAPI32 7 (registry, crypto random, user name), ole32 3, XINPUT1_3 3,
SHELL32 2, DSOUND 2 (`DirectSoundCreate8`, `DirectSoundEnumerateA`), d3dx9_36 2
(`D3DXGetShaderConstantTable`, `D3DXCheckTextureRequirements`), WININET 2 (cookies),
and one each from d3d9 (`Direct3DCreate9`), DINPUT8 (`DirectInput8Create`), EAX
(`EAXDirectSoundCreate8`), OLEAUT32 (`SysAllocString`), WINMM, iphlpapi and uplay32.
No C runtime DLL: the CRT is linked in. Direct3D, DirectInput and DirectSound are COM
interfaces, so behind the 5 graphics, input and sound imports there are a few hundred
methods called through vtables.

**Exports**: 178, all from Ubisoft's DARE sound engine (`DareObjectFactory`,
`SND_fn_*`), which is linked in and runs on DirectSound (with EAX).

**RTTI**: 4,376 type descriptors (3,407 in `scimitar`), 4,597 vtables found through
their complete object locators, with 59,899 slots (28,717 distinct functions). These
give classes' names to their virtual functions, which helps where nothing else does.

## Instruction census

A linear sweep of `.text` (`x86_64-w64-mingw32-objdump`) decodes 9.85M instructions,
but 5.44M of them are `int3` (alignment padding and the dead block above): about 4.4M
real ones. Following control flow from discovered functions (below) gives 91,969
functions and 3.31M instructions. Against the AC2 Mac build, MSVC's code has:

| | AC1 PC (linear sweep) | translator before this work |
|---|---|---|
| `fs:` accesses (SEH frame links, the TEB) | 46,844 | unsupported segment |
| `ret imm` (stdcall/thiscall) | 76,904 | supported: calls run on the guest stack, the callee pops its arguments as on x86 |
| x87 compares: `fcom*`, `fucom*`, `fcomi*`, `ftst`, `fxam`, `fnstsw`; `sahf`/`lahf` | ~7,000 | unsupported |
| x87 functions: `fsqrt`, `frndint`, `fprem`, `fscale`, `fyl2x`, `fpatan`, `f2xm1`, `fsin`... | ~60 (CRT math) | unsupported |
| `lock xadd`, `lock cmpxchg` (inlined `Interlocked*`) | 9,188 | `xadd` supported, `cmpxchg` not |
| `pushfd`/`popfd`, `bt*`, `rcl`/`rcr`, `loop*`/`jecxz`, segment register moves, `pop m32` | a few hundred (CRT, CPUID checks) | unsupported |
| MMX/SSE2 integer word arithmetic (`pmaddwd`, `paddsw`, `pextrw`...) | ~300 (DARE's mixers) | partly |
| `movzx` byte tables before `jmp [4*r + T]` (two-level switches) | 728 jump tables in all | single-level only |

Before the PE work, 84% of functions (52% of instructions) would have translated;
`fs:` alone blocked 12,905 functions. Now every real function translates (see Status).

## Function discovery without symbols

`recomp/src/pe.cpp` reads sections, imports (ordinal imports get their documented
names), exports, the TLS directory and the RTTI vtables; `discover_functions`
(flow.cpp) then finds function starts:

- **Strong starts**: the entry point, the exports, TLS callbacks, and every direct
  call target in code reached from them.
- **Weak starts**: vtable slots, words in data sections that point into code and look
  like a start (MSVC aligns 96% of call targets to 16 bytes after `int3` padding),
  and code addresses taken by immediates (`push offset callback`).
- A function is the code reachable from its start without passing another
  function's start (a branch there is a tail call, a conditional one too, as the
  CRT's assembly does; falling into one continues there). There are no extents:
  code may be shared, and a wrong weak start costs duplicated code, not wrong code.
- Weak starts are then checked and dropped, repeatedly, when they lie inside another
  function's instruction, are the target of a conditional jump elsewhere, or do not
  decode as a function (constants that happen to look like code addresses).
- `call [IAT slot]` and `jmp [IAT slot]` are import calls; the IAT slots are also
  filled with host functions, for MSVC's `mov esi, [__imp_X]; call esi`.
- Jump tables: MSVC's single-level form, the two-level one (byte table first), and
  the CRT `memcpy`'s tables (bounded by `and $3`, one indexed from 1, one from -3).

## The pieces and what they cost

Estimates for one developer made before the work (2026-10-28), as in [PLAN.md](PLAN.md):
guesses, not measurements. Every piece below has since been done (see Status).

| Piece | Status | Estimate |
|---|---|---|
| PE front end, function discovery, coverage | done | — |
| Translator: `fs`, x87 compares and functions, flags instructions, MMX/SSE2 words, MSVC jump tables | done (decoding; most of it not yet compared against Unicorn) | difftest coverage of the new instructions: 2–3 days |
| Whole image compiling | done | 1–2 days (disk: the C++ is about 1 GB, so it is generated and compiled in parts) |
| Windows runtime: a TEB per thread (SEH chain at `fs:[0]`, TLS array at `fs:[0x2c]`), static TLS, stdcall shims, COM objects as guest vtables of host functions, `RaiseException`/C++ `throw` fatal at first | done | 3–5 days |
| KERNEL32 (157), USER32 (32), ADVAPI32, SHELL32, ole32, WININET, iphlpapi: CRT startup, files, threads and sync objects, heaps, INI files, registry answers, one window from the Cocoa shell (`games/common`) | done | 2–3 weeks |
| Direct3D 9 → Metal: `IDirect3D9`, `IDirect3DDevice9` (~120 methods), textures, surfaces, vertex and index buffers, declarations, queries, D3D9 shader bytecode → MSL (the PC data session's D3D9 → ARB translator, commit bf26934, plus the ARB → MSL one, or a direct D3D9 → MSL path), `D3DXGetShaderConstantTable` | done | 6–10 weeks: the largest piece |
| DirectInput 8 (keyboard, mouse) and XInput 1.3 (the virtual pad) | done | 1 week |
| DirectSound 8 (+ 3D buffers, listener; EAX can report itself absent) under DARE | done | 2 weeks |
| Bink (14 stdcall functions over the FFmpeg `platform/src/bink.cpp`) | done | 2 days |
| Winsock (30): fail as unavailable; Uplay: nothing | done | 1 day |
| Saves: plain files (`SHGetSpecialFolderPathA`, CRT file I/O) | done | with KERNEL32 |

Reused: the translator and runtime, the guest heap, threads model, the Metal backend's
device, pipelines and presentation, Bink through FFmpeg, the virtual pad, the Cocoa
shell, and the engine knowledge (forge format, LZO).

## Names from AC2 Mac

The AC2 Mac executable keeps its symbols, and the two games share the Scimitar
engine, so AC1's functions can borrow AC2's names where the code survived the two
years between them. Matched 2026-10-28 (scripts and the result are kept outside the
repository: the names come from Ubisoft's binary):

- **Anchors:** 1,114 classes have a vtable under the same name in both (AC1's RTTI,
  AC2's `__ZTV` symbols). Slot k pairs with slot k, with GCC's two destructor
  slots folded to MSVC's one; where the slot counts differ, the leading slots
  pair while the two functions' sizes stay plausible. 4,308 pairs. Strings,
  32-bit constants and float constants in read-only data used by exactly one
  function in each binary: 455 pairs. Where both kinds name a function they
  agree 104 times out of 106.
- **Spreading:** the call lists of matched functions, aligned around callees
  already matched; the single unmatched caller of a matched pair; functions with
  the same unique set of matched callees. A pair is taken when its votes win by
  two to one. Of half the string and constant anchors held out, spreading
  recovered 70 correctly, 2 wrongly, and did not reach 154.
- **Result:** 4,978 functions named from AC2, among them `MainCommon::Initialize`,
  `EngineLoop::Enter`, `Engine::RegisterClasses` and most classes'
  `GetClassDescriptor`/`FastLoad`. Another 19,664 functions in vtables get their
  class and slot from AC1's own RTTI (`scimitar::Object::vslot_8`). The rest,
  about 85,000, stay `f_XXXXXXXX`: most code between the games changed, MSVC and
  GCC inline differently, and only 6,068 strings are shared.

A script outside the repository shows the names in backtraces. The translation
doesn't use them.

## Status

Coverage (`scimitar-analyze AssassinsCreed_Dx9.exe --coverage`): 98,453 of 98,470
functions translate, 3,484,322 of 3,484,935 instructions. The 17 others are weak
starts that are not code (round constants such as 0x004e0000 decoding to `les`,
`in`, `aaa`). The Dx10 executable gives 92,417 of 92,428 (before the later
discovery changes).

**Running** (`build/ac1` or `build/AC1.app`, built with `-DSCIMITAR_FULL_IMAGE=ON
-DSCIMITAR_GAMES=ac1`): the C runtime starts, the engine reads its settings, creates
its threads (job, sound, loader), opens the forge archives, creates DirectSound
buffers and DirectInput devices, creates its window on an engine thread
(2560x1440 here), Direct3D 9 and a full-screen device. It plays the Ubisoft logo,
the intro and loading-screen videos (Bink), shows the **title screen**, the profile
screens (an unattended run types a name: `SCIMITAR_KEYS` reaches the window as key
messages), writes the profile (`.hdr` under the port's folder), shows the **main
menu**, and on New Game loads the present-day room and the memory corridor into the
Animus tutorial: **gameplay with the HUD** (synchronisation bar, controls, the
"Puppeteering" tutorial panel), the 3D scene drawn with shadow maps (4096x1024),
a depth pass and the bloom chain, 900-2,000 draws a frame. All of it by the game's
own D3D9 shaders on Metal.

With New Game it plays the opening in the Abstergo lab (Desmond, Lucy, Vidic), the
Animus menus and the Animus tutorial (Altaïr, the scholars), all rendered correctly:
shadow maps, fog, bloom, colour grading, the HUD (also after the 2026-11-06
Direct3D 9 fixes described in [ACR_WINDOWS.md](ACR_WINDOWS.md)). At 2560x1440 the 3D runs at about
50 fps on this Mac (one frame in flight). The window fits the screen and takes the
mouse while the game is active; the game reads the keyboard and mouse through
DirectInput and a game pad through XInput.

Performance (2026-10-28): the game runs each frame on one thread; its four
task workers get little work. In the heaviest scene reached so far (the
Animus tutorial's crowd) that thread went from 18 to 26 fps:
- the constant upload no longer walks the bool-register map per parameter,
- `com::as<T>` checks a type tag instead of `dynamic_cast`,
- the pad is read on the main thread into a snapshot,
- the x87 runtime has bit-exact fast paths for normal operands (2x).

What remains on that thread: the translated game code itself (about half),
the x87 runtime. Metal encoding has since moved to a render thread (the
crowd scene from 25 to 27 fps), and critical sections got cheaper. Benchmarks run hidden from a
copied save folder (`HOME=...`) with `SCIMITAR_FPS_LOG=path`; the scripted
keys follow frame numbers, so compare only runs that took the same path.

Still to check or do: playing by hand (only scripted runs so far, hidden and muted:
sound has not been heard), the memories past the tutorial (Solomon's Temple, Masyaf),
saving and loading a game (the profile is written), fixed-function and FVF draws
(none seen yet), sRGB, the remaining Direct3D methods (each stops the run with its
name), and performance beyond this first pass.

What it took at run time, besides the shims:
- **Code below a function's entry.** A PE function may jump back to code before
  its start; the body is emitted in address order, so it now starts with a jump to
  the entry (it used to run from the lowest address: a silent miscompile).
- **Discovery details**: code addresses after `ret`/`ret n` and the linker's
  `jmp rel32` thunks (the window procedure is one) are weak starts too; a weak start
  is not dropped for overlapping a function that is itself dropped; conditional
  tail calls in the CRT's assembly.
- **MSVC jump tables**: `memcpy`/`memmove` index some tables negatively (`neg`,
  `sub $k; jb`) and one from 1; a compare may sit several instructions before its
  branch; a two-level switch whose bound is in a register is bounded by the byte
  table that follows the word table.
- **Threads**: the window lives on an engine thread while the main thread waits, so
  the guest main thread runs on a host thread of its own and the host main thread
  runs the run loop AppKit needs (the scheme `games/acr/main.cpp` uses too); translated frames need
  large host stacks (256 MB for game threads).
- **Flags across splits**: discovery may start a function inside another (a
  data word that points there); the code before it then runs into it as a tail
  call. Such transfers (and PE tail calls) pass the flags in `Context::eflags`, and
  a function that reads flags before setting them takes them from there. Only
  strong functions (entry, exports, the call graph) count as evidence against weak
  starts; weak starts decoding to instructions a game never runs (I/O, BCD, far
  pointers) are data.
- **Render targets start black.** Metal leaves new textures undefined, the game reads
  targets it has not fully drawn (the bloom chain's edges): uninitialised memory made
  the scene washed out with magenta edges. Targets are cleared when created.
- **A draw that samples its own render target** (post-processing) reads a copy taken
  just before it; Metal has no defined result for the feedback.
- **Render targets as textures**: render-target textures are made without a
  swizzle and sampled through a swizzled view (an L8 target read as red only made the
  menus pink).
- **Compile time**: one 29,000-instruction x87 function took clang's optimizer over
  15 minutes; functions with more than 4,000 x87 instructions are left unoptimized.
  Parts of the generated code are chosen by address, and unchanged parts keep their
  files, so a translator change recompiles only what changed.

The Windows layer (`platform/win32`, shared with the Revelations and Prince of Persia ports):
`core.cpp` (handles, waits, TEB and static TLS, the import address table),
KERNEL32 (`kernel32.cpp`, `_sync`, `_file`), USER32 (`user32.mm`, Cocoa), the small
DLLs (`system.cpp`), COM objects in guest memory (`com.*`), DirectInput (keyboard
and mouse), DirectSound (buffers mixed into a 48 kHz host output; 3D buffers placed by
distance and direction), Bink (stdcall wrappers over the FFmpeg Bink), Direct3D 9 (`d3d9.mm`: the
API, resources, states, the D3DX constant table) and its Metal renderer
(`d3d9_metal.mm`: D3D9 bytecode -> ARB -> MSL through the existing translators,
pipelines and depth/stencil states from the render states, guest-memory vertex and
index buffers read in place, textures uploaded when unlocked; the game thread
records each frame as commands that a render thread encodes on Metal, one frame in
flight).

Debugging aids: `SCIMITAR_TRACE_WIN32=1` (shim log), `SCIMITAR_SCREENSHOT=dir`
(+`SCIMITAR_SCREENSHOT_EVERY`, PPM of the back buffer, at most 1280 wide),
`SCIMITAR_KEYS="frame:keycode[:hold],..."` (Mac key codes), and
`SCIMITAR_DISCOVERY_DEBUG=<hex address>` (why discovery dropped a weak start).
The generated code is built with line tables: `dsymutil` plus `atos` name the
generated line, whose comment is the guest instruction.

Game pads: the game finds pads through DirectInput and takes an Xbox 360 controller
(vendor 0x045e, product 0x028e) to its XInput path (`PadXenon`). DirectInput lists the
first GameController-framework controller as "Controller (XBOX 360 For Windows)" and
XInput reads it (`platform/win32/xinput.mm`); `SCIMITAR_PAD="frame:control[:hold],..."`
scripts one, as in the Mac ports. Menus answer the pad.

Sound: DirectSound buffers are mixed (volume and pan, 16- and 8-bit PCM) into a host
output unit at 48 kHz, which drives their play cursors; with `SCIMITAR_MUTE` a timer
plays them silently. Not yet heard in a run (the author's test runs are muted).

3D buffers are attenuated and panned from the listener (velocities and cones are not
applied); EAX is reported absent.

**Building and running**: AC1 is built with the other ports
(`-DSCIMITAR_FULL_IMAGE=ON -DSCIMITAR_GAMES=ac1 -DAC1_EXE=/path/to/AssassinsCreed_Dx9.exe`)
and makes `ac1` and `AC1.app`, which shows the launcher first. `$AC1_EXE` (the .exe
or its folder) overrides the built-in location. `SCIMITAR_FULL_SCREEN=1` starts in
full screen; `SCIMITAR_WIN32_CPUS` sets the processor count the game sees (4 by
default). Profiles and saves go to
`~/Library/Application Support/scimitar-recomp/ac1`.
