# Plan

Rough estimates assume one experienced developer; they are guesses, not measurements.

## Phases

0. **Groundwork** — repo, Ghidra project, code inventory (functions from symbols and
   `eh_frame`, imports, vtables, jump tables, function pointers in data).
1. **Pilot: LZO** (done: 286/286 identical; on the real `DataMAC.forge` the engine's table readers, `BigFile::ReadFile` and `CompressBinStream::DecompressBuffer` run identically to the original x86 and match `liblzo2`, see `tests/forge`) — recompile `lzo1x_decompress_safe` (fixture in
   `fixtures/ac2-1.0.1/`), verify byte-identical output against Unicorn running the
   original x86 and against `liblzo2` on `tests/lzo/vectors/`. Then grow to the
   engine's `.forge` reader (first file-system shims) and an SSE math cluster.
2. **Full translation** of `scimitar_sf` (done): everything compiles, per-function Unicorn
   comparisons pass on a large sample, boots into `main()`. This cannot be done
   piecemeal: the direct call closure of `_main` is only 4,600 of 208,730 functions,
   and the rest is reached through vtables (see [Assessment](#assessment-2026-10-27)).
3. **macOS platform layer → main menu** (done): libSystem/CoreFoundation/threads/files,
   C++ exceptions (fatal on throw at first, see risk check 2), OpenGL passthrough
   (legacy GL on Apple Silicon accepts the game's ARB assembly shaders, see risk
   check 1), native replacements for the game's Cocoa classes, local saves in
   place of Orbit (risk check 6).
4. **Playable on macOS** (done): audio, Bink videos, controllers via GameController
   (replacing `PadProxyMac`), fix translation bugs.
5. **Metal renderer**: ~100 GL calls on Metal; ARB assembly → Metal Shading Language
   (done: the launcher apps' renderer and the only one on iOS, with a shader
   precompile and cache; design and status in [METAL.md](METAL.md)).
6. **iOS** (done for AC2 and Brotherhood on an iPhone 17 and an iPad Air M3): UIKit
   shell (`games/common/shell_ios.mm`), data delivery, and a compact 2 GB guest that
   needs no paid-only entitlements; [games/ac2/ios/README.md](../games/ac2/ios/README.md).
7. **Brotherhood** ([games/acb/README.md](../games/acb/README.md)):
   `build/acb` plays from the logo video through Story Mode to the arrival in Rome
   (on the Mac and on the iPhone) and loads saves, on GL and Metal, with keyboard, mouse and game pad. The port's entry
   point, shells and overrides are shared with AC2 (`games/common`, overrides and game
   functions found by symbol); Brotherhood added external relocations, new jump-table
   forms, the Orbit C++ client as imports, VAOs and draw buffers.
8. **Other Anvil games (Windows builds)**: a PE front end, MSVC support in the
   translator and a Win32/Direct3D 9 platform layer. Done for AC1 (the opening and the
   Animus tutorial), Revelations (menus, videos, Animus Island) and Prince of Persia
   (2008, to the first canyon): [AC1_WINDOWS.md](AC1_WINDOWS.md),
   [ACR_WINDOWS.md](ACR_WINDOWS.md), [POP_WINDOWS.md](POP_WINDOWS.md). The AC2 PC
   executable was not ported: the Mac one runs the PC data ([PC_DATA.md](PC_DATA.md)).
   Assessment in [ANVIL_FAMILY.md](ANVIL_FAMILY.md).

## Early risk checks
1. Does Apple Silicon legacy OpenGL accept `ARB_vertex_program`/`ARB_fragment_program`?
   **Yes** (`tests/gl/arb_probe.cpp`, M1 Pro, `2.1 Metal - 91.7`): every extension
   behind the game's GL imports is present; a synthetic env/local-parameter, TEX/KIL
   render gives the expected pixels; all programs embedded in the binaries compile
   under native limits (AC2 30 vp + 96 fp, ACB 38 vp + 110 fp; up to 246 vertex
   env parameters, limit 256). **Forge data too** (2026-10-28, `tests/gl/forge_shaders`):
   the 25 AC2 archives hold 24,911 distinct ARB programs (13,241 vertex, 11,670
   fragment; 70,033 sections, 8.5 GB decompressed); all compile and stay under native
   limits (up to 616 instructions, 21 temps). 70 vertex programs read
   `vertex.attrib[N].secondary` (cgc 2.2 output the drivers of the time accepted);
   Apple's parser needs the meaningless `.secondary` dropped, which the GL layer will do.
2. Exception strategy prototype (guest unwinder + host exception to landing pad).
   **Mostly defused** (AC2): 69 `__cxa_throw`/`__cxa_rethrow` call sites, all in the
   statically linked libstdc++ (`std::__throw_*`), `std::vector` catch-and-rethrow and
   `google::sparsetable`. No Scimitar, Gear, Havok, Wwise or Quazal code throws. The
   ~2 MB `__DATA,__gcc_except_tab` is mostly cleanup landing pads. Start with "throw
   = fatal error with a guest backtrace"; build the unwinder only if a throw is seen
   at runtime. Objective-C `@try` (setjmp-based, `objc_exception_try_enter`) appears
   only in the Cocoa shell, which is replaced natively.
3. Indirect-call target coverage (function pointers in data vs known function starts).
   **Good** (AC2): 141,345 of 142,285 text-range words in `__DATA,__const` (vtables)
   are function starts from the symbol table; the 940 others look like constants
   (`0x400000`, `0x900000`). All 3,647 `__mod_init_func` entries are function starts.
   `__eh_frame` has 127,232 FDEs against 208,730 function symbols, so symbols remain
   the primary boundary source. Indirect jumps: ~5,800 are virtual tail calls
   (`leave; jmp ecx`), translated as tail `call_indirect` (done); ~420 are real jump
   tables (`jmp [4*reg + table]`) with their entries in `__TEXT,__const` (~30k words
   pointing into function bodies), since translated as `switch`es (done).
4. Generated code size and full build time.
   **Acceptable** (AC2, extrapolated from `build/gen/forge_readers.cpp`: 3,519
   instructions → 435 KB C++, 70 KB object, 0.7 s at `-O3`): full image ≈ 1.1 GB C++,
   ≈ 180 MB objects, ≈ 30 CPU-minutes. Largest functions are ~20k instructions
   (`hkSimplexSolverSolve`, `hkGeomConvexHullBuilder::generateConvexHull`); only 22
   exceed 5k. Watch for superlinear optimizer time on those.
5. **New: x86 memory ordering on arm64.** Recompiled loads/stores run under ARM's
   weak ordering, not x86 TSO. Most synchronization goes through imports (13,386
   `OSAtomicAdd32` calls, 43 `OSAtomicCompareAndSwap32Barrier`, pthread mutex/cond,
   Mach semaphores), and those shims are real host barriers. Only 58 inline `lock`
   instructions exist (Havok, libstdc++ locale/ios). Code that publishes data with
   plain stores (lock-free queues in Havok or Wwise) cannot be ruled out statically.
   Keep an emitter mode that makes every guest access acquire/release (`ldar`/`stlr`)
   to bisect ordering bugs.
6. **New: savegames go through Orbit.** AC2 `dlopen`s `ubiorbitapi_r2.dylib` (shipped
   with the launcher, depot 48172) and resolves 22 `MgOrbitdll*` functions: session,
   login and launcher calls, plus savegame list/read/write/remove/info. Without a
   replacement the game cannot save. The boundary is narrow (22 C functions), and
   `MgOrbitdllGetFakeSession` suggests an offline path worth tracing.
   **Decided:** Orbit is not ported; saves are local (see Decisions).
   **Done** (2026-10-28): `platform/src/orbit.cpp` implements the 22 functions
   (offline session, saves as files), installed by an override of
   `LibraryLoader::PrivLoadLibrary`. `tests/saves` runs the game's own
   `scimitar::OrbitClient` code (Initialize, IsOrbitReady, Setup, SaveGameWrite/Read/
   Delete) recompiled against it: the game starts offline instead of quitting with
   "launcher not found", and what it saves reads back identically.

### Revised risk order
Highest first, by how much each blocks the path to the main menu:
1. ~~Translator coverage for the full image (switch tables from risk 3, then x87 float
   returns and SSE, see the census below)~~: done, every function translates.
2. ~~Local saves in place of Orbit (risk 6)~~: done, the game's own save code works
   offline against local files.
3. Memory ordering (risk 5): the game now runs threaded into gameplay (Wwise, task
   threads, loaders) with OSAtomic as sequentially consistent host atomics and no
   ordering failure seen yet; longer play will tell.
4. ~~Shaders from `.forge` data (rest of risk 1)~~: all 24,911 compile.
5. Exceptions (risk 2) and build size (risk 4): low.

## Assessment (2026-10-27)
Static only (Mach-O inspection, `objdump` census, `scimitar-analyze`),
AC2 1.0.1 `scimitar_sf` only; nothing was executed.

**Instruction census** (8,856,134 instructions, 208,730 functions, 278 distinct
mnemonics):

| Class | Share | Notes |
|---|---|---|
| Supported by `recomp/` when measured | 92.5% | `mov` alone is 48%; with `mul`/`div`/`cmov` (rows below, since added) 92.7% |
| SSE/SSE2 floating point | ~7% | mostly scalar (`movss`, `mulss`, `addss`, `ucomiss`), plus `movaps`/`*ps`; SSE3 at most (`fisttp`, `haddpd`), no SSSE3/SSE4, so NEON or sse2neon-style mappings cover it |
| x87 | 0.13% (11,269) | mostly `fstp`/`fld`: i386 returns float/double in `ST0`, so nearly every float-returning call needs a small x87 stack model; 32 `fldcw`, 37 80-bit `tbyte` operands, `strtold` import |
| SSE2 integer | 0.13% | `pslld`, `por`, `paddd`, `pslldq` |
| `imul`/`mul`/`div`/`idiv` | 0.10% | supported now |
| `cmov*` | 0.07% | supported now |
| Other | <0.05% | 1,404 `rdtsc` (map to a monotonic counter consistent with `mach_absolute_time`), 2 `cpuid` (report a fixed SSE3 CPU), 607 rotates/`shld`/`shrd`/`bswap`, ~1.3k real string ops, 251 `xchg` |

`rcpps`/`rsqrtps`/`rcpss` approximations differ bit-for-bit between x86 and NEON; Unicorn
comparisons must either emulate the x86 tables or allow a tolerance for them.

**Imports** (593): libSystem 314, OpenGL 103 (fixed function + ARB programs + EXT
FBOs), CoreFoundation 63, IOKit HID 24, CoreGraphics display 18 (several removed from
current SDKs, e.g. `CGDisplaySwitchToMode`), Bink 14, AudioToolbox/AudioUnit/CoreAudio
17, Foundation 13, AppKit 11, libobjc 9, CoreServices 7 (Carbon `FSOpenFork`,
`PBReadForkAsync`). `libssl.0.9.8`, `libcrypto.0.9.8`, `libsqlite3`, Security, AGL and
`libgcc_s` are linked but have **no imports**: no shims needed. The C++ runtime is
statically linked (only `_Unwind_*` is imported), so libstdc++ is recompiled with the
game.

**Cocoa**: the game defines two Objective-C (ObjC 1) classes, `ACApplicationDelegate`
(14 methods) and `CustomOpenGLView` (6 methods), both replaced natively.

**Threads**: created by Gear (`pthread_create_suspended_np`, `thread_suspend`), Wwise
(5 sites) and Quazal; each needs its own guest stack and `Context`.

Everything above was done without a decompiler.

## Runtime progress (2026-10-27)

`build/ac2` (`cmake -DSCIMITAR_FULL_IMAGE=ON`, target `ac2`) is the whole game
recompiled and linked with the platform layer and a native Cocoa shell
(`games/common/shell.mm`). Run from the repo root, it loads the original image, binds
imported data, runs all 3,652 static initializers and the game's `main`, and reaches
**gameplay**: the title screen, Story Mode, name entry (a local save is written), then
the opening sequence in the Abstergo lab, rendered through the host's legacy OpenGL 2.1
with the game's ARB programs, at about 100 fps at 800x600 from an `-O1` build on an
M1 Pro, steady past 15,000 frames. Keyboard input works (virtual HID keyboard), and so
do game pads (a virtual HID pad fed from the GameController framework); audio runs
through Wwise into a host AUGraph.

What it took, beyond the translator: the Intel 1/1 Mach timebase (Apple Silicon's
125/3 made the engine's clock 0); guest thread stacks freed on exit (Wwise starts a
thread per read); display mode dictionaries with `kCGDisplayModeIsSafeForHardware`
(without it the view surface is 0x0 and frames are black); `MacSetApplicationActive`
from the shell (the pad manager is off until then); keyboard through IOKit HID, not the
Cocoa key handlers (`MacHandleKeyDown` only feeds the debug console); rwlocks that
another thread may unlock.

Bink videos play through FFmpeg (`platform/src/bink.cpp`: the Ubisoft logo at startup,
with its audio track on a host output unit). The mouse is captured
while the game is active (hidden, decoupled cursor), as the original did. Shadow
samplers (units 9 and 10 in 9,282 forge programs) left with a colour texture bound
get a lit 1x1 depth texture per draw, where Apple's GL would sample zero. A 45,000-frame
idle soak (about 7 minutes, mostly at 107 fps) ran clean.

Checked by scripted runs: the Ubisoft logo video, title, main menu (arrow keys),
Exit to Desktop (clean exit), new session with name entry, the whole opening (security
camera, Abstergo, Altaïr's memory, Lucy, the Animus) into the lab escape with Desmond
walking, a save written at the first checkpoint, and loading it from a second run: the
Story Mode list shows the session with its thumbnail and resumes there. Options →
Graphics lists the display's modes (the game keeps its settings in
`~/Library/Application Support/Ubisoft/Assassin's Creed 2/Assassin2.ini`, as the
original, and defaults to the desktop mode); switching resolution at runtime goes
through the game's "keep these settings?" prompt and resizes the GL surface.

Game pads, as the Mac build finds them: `PadManagerNative` looks once, at startup, for
IOHIDDevice services with PrimaryUsagePage 1 / PrimaryUsage 5 and makes each an
`io_kit::Joystick` (HID manager matching DeviceUsagePage 1 / DeviceUsage 0, queue of
X/Y/Z/Rx/Ry/Rz and buttons 1-128, no hat) behind `PadMac`, in slots 5-7 after the five
keyboard and mouse profiles; only the selected slot drives the game (Options, or
`[Input] SelectedInput=<pad name>` at startup). `PadIoKit`'s constructor picks a
built-in layout from vendor/product (0x045e/0x028e Xbox 360 = type 6, also 0x045e/0x0289
Xbox, 0x0738/0x4516 Mad Catz, 0x0e8f/0x0001, 0x0c12/0x0005, 0x0925/0x8866,
0x6666/0x0667), else type 8, which maps through `[VendorID/ProductID]` ini bindings
that no shipped file provides. But the Mac `io_kit::Device` never reads vendor, product
or name (no registry property call), so on a real Mac every pad was type 8 with
nothing bound. The port's virtual pad (`platform/src/iokit.cpp`) answers the service
lookup when a controller is connected, and overrides of `Device::vendorId`/`productId`/
`product` (`games/common/overrides.cpp`) make it the Xbox 360 controller with the
controller's name. Type 6 reads the 360Controller driver's layout (buttons 1 A, 2 B,
3 X, 4 Y, 5 LB, 6 RB, 7/8 stick clicks, 9 Start, 10 Back, 11 Guide, 12-15 d-pad up,
down, left, right; sticks on X/Y and Rx/Ry, +-32768; triggers on Z/Rz, pressed when
above 0) into a fresh state every frame, so the virtual queue hands out every element
on each drain. The shell selects the pad (`PadManagerNative::SelectPad`) from the first
frame when there is one, and whenever it is used; a key or click returns to the keyboard
profile. Button prompts: the HUD movies are shared with the consoles and pick their art
from the UI variable `PLATFORM`, which `FireItem::GetVariable` answers with
`GetPlatformName(1)` = "PC" (generic action icons). Overriding `GetPlatformName` for
that caller gives the HUD (`MultiComponentShowingFireItem`, asks once per load) and
`MissionActivatorMenu` "360" (Xbox buttons) or "PS3" (PlayStation buttons) while the pad
is selected; `TitleMenu` and `PauseMenu` keep "PC", because on console names they drop
Exit to Desktop. Checked by scripted pad runs: the title, the main menu by d-pad and
stick (the game switches its prompts to controller buttons), Story Mode, name entry and
the opening; in Florence (`/nobink /world:Firenze`) the right stick turns the camera,
the left stick walks Ezio, Start opens the Animus menu, the d-pad moves in it and B
closes it; loading a save, the HUD shows Y/X/B/A, or triangle/square/circle/cross with
`SCIMITAR_PAD_KIND=playstation`. The author played with an Xbox controller.

The startup videos' "ko" directory is the original Mac build's own behaviour, not a
port bug: `StartupVideoManager::DetectStartupVideosForLanguage` checks
`videos\en/Warning_Disclaimer.bik` (a Windows separator) with a plain `open`, which
fails on any Mac, so it falls back to language 12, whose video directory is "ko".

Unattended runs: `SCIMITAR_KEYS="2000:36,2600:36,3200:36,3800:36"` (frame:keycode[:hold])
presses Return through the menus; `SCIMITAR_PAD="2600:a,3200:down,7000:lup:600"`
(frame:control[:hold], controls a b x y lb rb lt rt ls rs start back guide up down
left right, and lup ldown lleft lright rup rdown rleft rright for the sticks; any value,
e.g. `1`, gives the game a pad without a controller) does the same with the pad;
scripted runs do not take the keyboard focus. `SCIMITAR_SAVE_DIR` pointing at a copy of
a save lets a run load straight into gameplay with the HUD (`/world:` starts have none);
`SCIMITAR_EXIT_FRAME`, `SCIMITAR_SCREENSHOT=dir`
(+`SCIMITAR_SCREENSHOT_EVERY`, `_FROM`, `_COUNT`); traces: `SCIMITAR_TRACE_GL`,
`SCIMITAR_TRACE_GL_FRAME=N`, `SCIMITAR_TRACE_INPUT`, `SCIMITAR_TRACE_FILES`, `SCIMITAR_TRACE_THREADS`,
`SCIMITAR_TRACE_ALLOCS`, `SCIMITAR_TRACE_OBJC`. More switches are described in
[games/ac2/README.md](../games/ac2/README.md), the launcher's
[README](../games/common/launcher/README.md) and [METAL.md](METAL.md).

## Performance (2026-10-27)

Florence (`/nobink /world:Firenze`, `SCIMITAR_BENCH=1200-3600`, M1 Pro): about
30 fps at first, now about 50-55 fps, with steady frame times (p95 within 2 ms of
p50). The work that got there: guest registers in locals in the generated code
(`cache_registers` in `emit.cpp`), `-O2` for the generated code, NEON for packed
SSE, a hash table for indirect calls, a lock-free mutex cache, Apple's
multithreaded GL engine, and no driver waits for buffer maps, occlusion queries
or flushes.

The limit now is Apple's OpenGL-over-Metal driver: its thread spends about 20 ms
a frame on the engine's ~45k GL calls (3,600-4,200 draws). Resolution and quality
settings barely change that (1280x720 to 2560x1440, low to max: 44-57 fps; shadows
off helps most). Going much higher (120 fps) needs a different graphics backend: a
translation of the game's GL usage (ARB programs, FBOs, VBOs, fixed state) to Metal
with cached pipelines, instead of Apple's general GL driver.

That backend is designed in [METAL.md](METAL.md), and it runs the game with
`SCIMITAR_RENDERER=metal`:
- The ARB → MSL translator handles all 24,911 forge programs and the
  executable's 126 (`tests/metal/arb_msl_test.mm`).
- The title screen matches the GL path's, and Florence renders completely.
- Unoptimized, Florence benchmarks at about 60 fps against GL's 51. The render
  thread itself is now the limit, more than half of it the game's own code.

Metal is now the default in the apps (the launcher sets it) and the only renderer on
iOS; the plain executables still default to GL unless `SCIMITAR_RENDERER=metal`.

## Open questions

- Uplay reward unlocks: whether AC2 asks for them through the Orbit calls the port
  already answers.

## Decisions so far
- **AC2 executable**: 1.0.1 (`afcbbbf8…`). Steam's current manifest ships the older,
  mostly stripped 1.0.0; 1.0.1 still comes from the Steam console
  ([MAC_RELEASES.md](MAC_RELEASES.md)).
- **Bink**: replaced natively with an FFmpeg-based implementation of the 14 functions
  the games import. All 86 `.bik` files (`BIKi`, Bink 1) decode cleanly with FFmpeg.
  Some carry several audio tracks (`BinkSetSoundTrack` must select them).
- **LZO / zlib / libjpeg**: recompiled with the game (the pilot verified LZO bit for
  bit); native swaps were considered but are not needed.
- **Wwise, Havok, Scaleform-style UI, Quazal**: no public source; stay recompiled.
- **Ubisoft launcher / Orbit (`ubiorbitapi_r2.dylib`)**: **not ported.** It talks to
  old Ubisoft services that no longer exist, and the author holds a licensed copy with
  its serial. Saves are local instead: the platform layer answers the game's
  `dlopen`/`dlsym` of `ubiorbitapi_r2.dylib` with our own implementation of the 22
  `MgOrbitdll*` functions. Savegame list/read/write/remove/info map to files in the
  user's Application Support folder; session, login and launcher calls report a
  local offline session. Ubisoft online services are not contacted. Nothing from the
  original dylib is used or shipped.
- **Distribution**: tools only; users recompile from their own copy.
