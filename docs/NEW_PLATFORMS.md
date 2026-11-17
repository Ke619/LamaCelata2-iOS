# AC2 on platforms that never had it: assessment

Assessed 2026-10-28 for four targets the author picked, in the author's order:
PS Vita, Android, Emscripten (browser) and tvOS. Only the PS Vita has builds so far
(a benchmark measured on the device and an ARMv7 build of the whole game); the
others are paper assessments, weighed against what the port needs.

## What the port needs (measured)

| | Value | Source |
|---|---|---|
| Guest memory in use, Florence | about 740 MB dirty | `footprint` on `build/ac2 /nobink /world:Firenze`, GL path, M1 Pro ("Untagged") |
| Whole process, Florence | 1.5 GB footprint on the Mac; 1,341 MB peak on the iPad (Metal) | `footprint`; iOS notes |
| Translated code | 2.4 GB of generated C++, a 155 MB arm64 executable | `build/gen/full`, `build/ac2` |
| Game data | about 8.7 GB | the Mac app |
| Shaders | 10,598 distinct ARB program translations | docs/METAL.md |
| CPU | Metal path at about 60-67 fps in Florence (2026-10-28), render thread CPU-bound | docs/METAL.md |
| Saves | about 52 KB | `~/Library/Application Support/scimitar-recomp/saves` |

What carries over to a new platform:
- The generated code is plain C++. NEON paths are behind `__aarch64__`, with
  scalar fallbacks for other hosts. x87 is emulated in software.
- Guest memory is one block at `base + guest address`. The compact layout used
  on iOS needs 2 GB of address space, of which about 0.9 GB is touched; the
  window layout (`scimitar/guest_layout.h`) fits a 32-bit host.
- The platform layer answers CoreFoundation, Objective-C, IOKit, pthread and
  POSIX imports itself, on its own objects (`bridge.h`). It is mostly portable
  C++, and file lookups already fall back to a case-insensitive match.
- Host-specific code is small:
  - the Metal backend and the ARB → MSL translator, about 4,500 lines then (about
    5,700 now, with the shader cache);
  - AudioUnit output in `audio.cpp`;
  - the AppKit and UIKit shells, about 1,800 lines;
  - Bink, which uses FFmpeg.

## Summary

| Target | Verdict | Main work | Rough effort |
|---|---|---|---|
| tvOS | Feasible, near-certain | iOS build with the tvOS SDK; storage and saves; Siri Remote excluded | days |
| Android | Feasible | a new GPU backend (Vulkan, or WebGPU through Dawn); audio; shell; packaging | 2–4 weeks |
| Emscripten | Feasible on desktop Chrome, with risks | WebGPU backend; threads and file system in the browser; a very large wasm module | 4–8 weeks |
| PS Vita | Not feasible as a playable game; a tech demo is under way | CPU: measured 52× slower than the Mac, about 1.5–2 fps in Florence; memory: the heap fits after the cuts, the code needs paging | — |

Suggested order: tvOS, then a WebGPU-style backend written once for Android and
the browser, then Android, then Emscripten. Vita only as a boot-to-title curiosity, if at all.

## PS Vita

Hardware:
- **CPU:** four Cortex-A9 cores at 333–444 MHz. Three cores are for games.
- **Architecture:** 32-bit ARMv7 with VFPv3 and NEON.
- **Memory:** 512 MB of main RAM, of which homebrew gets roughly 300–365 MB, plus 128 MB of video memory.
- **GPU:** PowerVR SGX543MP4+, driven through GXM or vitaGL.

Measured 2026-10-28 with Vita-like settings (ad hoc scripts, not kept):
- **Run:** Florence (`/nobink /world:Firenze`), Metal renderer, 960x544,
  every quality setting at 0 including `TextureQuality`, hidden and muted, on an M1 Pro.
- **Speed:** 86 fps.
- **CPU per frame, from each thread's CPU time over 30 s (2,580 frames):**

| Thread | CPU per frame |
|---|---|
| main game thread (simulation, plus our Metal encoding) | 9.2 ms |
| 7 job workers, about 71% of samples waiting | 2.25 ms each, 15.7 ms together |
| everything else (audio, loaders, OS) | about 2 ms |
| **total** | **about 27 ms**, 2.3 cores busy |

- **Memory:** the game's own memory came to 536 MB, and the process to 962 MB.
  - `/skipmips:2` changes neither, so skipping texture mips doesn't shrink what the game keeps in memory.
  - Lowering the settings saved only about 200 MB from the 740 MB at default settings.

Blockers:
1. **CPU, measured on the device:** translated code runs **52× slower** on a Vita
   at 444 MHz than on an M1 Pro core (`scimitar-bench`, below).
   - The main thread alone would need about 480 ms a frame. All threads together
     would need about 620 ms a frame over three cores.
   - **That is about 1.5–2 fps.** The estimate before the measurement was 15–35×, or 3–7 fps.
   - Translation costs 3.9× native code there. Even with no translation cost, it would be roughly 4–5 fps.
2. **Memory.** The guest heap's cuts are done and measured (Memory cuts, below):
   559 MB → about 220 MB that a Vita would have to back. What is left over is the code.
   - A Vita app gets roughly 300–365 MB plus 128 MB of video memory.
   - The ARMv7 code of the whole translated game is 134 MB (measured, ARMv7 build
     below). Loading all of it would leave too little for the game, so the code
     has to be loaded as it is needed, or made smaller.
3. **Lesser problems:**
   - SGX shaders: 10,598 ARB programs would need to be compiled offline to GXP.
     Compiling on the device is too slow.

### Heap trace (measured)

`SCIMITAR_HEAP_REPORT=90` lists every live block by the guest call stack that
allocated it. The run used the same Florence setup as above, and ad hoc scripts
(not kept) named and grouped the stacks.
- **Totals:** 558 MB live, 150 MB of it in pages that are still all zero.
- **How sizes are attributed:** Gear's allocators take memory from the heap in
  chunks and sub-allocate from them, so each chunk counts toward whatever made it
  grow. The sizes per system are therefore close, not exact.

| What | Live | Note |
|---|---|---|
| Our Metal backend's GPU buffer arena | 192 MB | three 64 MB chunks, 110 MB never touched; the game's vertex and index buffers use about 82 MB |
| Shaders | 115 MB | 13,329 ARB programs as text: 108 MB, counted directly. PC-only data |
| Textures (`CompiledTextureMap`) | 68 MB | the game's CPU copy of texture data; the GPU holds another copy |
| Meshes | 46 MB | |
| Audio (Wwise pools and banks) | 41 MB | pools sized for PC; the `/sound*` options set them |
| Object preload block | 19 MB | one block, `ObjectBlockAllocator::AllocatePreloadBlock` |
| Animation, navigation, UI, physics, streaming | 22 MB | |
| Thread stacks (ours) | 30 MB | almost all zero |
| Other | 20 MB | |

The executable image adds about 15 MB that a Vita build would still need: 5.6 MB
of `__DATA` and the constants in `__TEXT`. The x86 code and the symbol table aren't needed.

What this means:
- **The assets are about 130 MB in all:** textures, meshes, animation and navigation together.
  Even halving them with console data would save about 65 MB. The bigger savings are elsewhere:
  - the shader text: 108 MB, which an offline-translated build doesn't need;
  - our arena and stacks: about 125 MB of the 222 MB is unused or zero;
  - the audio pools.
- **Video memory:** the GPU side needs textures (about 90 MB at these settings)
  plus the buffers (about 82 MB). That is more than the 128 MB of CDRAM, so some
  of it would sit in main RAM.
  - The Vita's memory is shared between CPU and GPU, and the game keeps its own
    copy of the texture data, so a backend could perhaps sample that copy directly instead of making a second.
  - Whether GXM accepts that layout is untested.

### Toward a tech demo

Work from 2026-10-28; the author's Vita is a PS Vita 2000 with HENkaku and Ensō.

#### The CPU factor: `scimitar-bench`

[tests/bench](../tests/bench/README.md) measures how fast translated code runs on
a host, with no game code or data, so the Mac's per-frame CPU can be scaled to the Vita:
- **Workloads:** synthetic kernels shaped like an engine frame (vector math with
  x87 float returns, particles, hashing, a comparator sort, entity updates through
  function tables, a tree, text parsing, doubles).
- **How:** each is built as an i386 Windows executable with MinGW's GCC (SSE,
  frame pointers, like the game's compiler), translated by `scimitar-recomp`
  and run on the runtime, and also compiled natively. The LZO fixture's
  translation runs too, then the suite on 1–3 threads, then a memory probe.
- **Checks:** every kernel's checksum must match between the translated and native builds.

On the Mac (M1 Pro, one core):

| Kernel | Translated | Native | Translated / native |
|---|---|---|---|
| vecmath | 49 ms | 8.7 ms | 5.7× |
| particles | 51 ms | 16.4 ms | 3.1× |
| hash | 48 ms | 38 ms | 1.3× |
| sort | 53 ms | 29 ms | 1.8× |
| entities | 55 ms | 11.7 ms | 4.7× |
| tree | 55 ms | 53 ms | 1.1× |
| text | 49 ms | 42 ms | 1.2× |
| double | 47 ms | 8.2 ms | 5.8× |
| LZO (fixture) | 46 ms | – | – |
| **score** (all translated) | **455 ms** | | |

On the Vita the same app (`build-vita/scimitar-bench.vpk`) runs at 444 MHz and
writes the same report. **Its score over 455 ms is the factor** that replaces
the 15–35× estimate above. The main thread's 9.2 ms times that factor gives
the frame time.

Measured 2026-10-28 on the author's PS Vita 2000 (HENkaku + Ensō): ARM at
444 MHz, bus at 222 MHz, every checksum matching (`RESULT: ok`).

| Kernel | Vita translated | Vita native | Translated / native | Vita / Mac, translated |
|---|---|---|---|---|
| vecmath | 2,850 ms | 288 ms | 9.9× | 58× |
| particles | 5,523 ms | 1,253 ms | 4.4× | 108× |
| hash | 1,007 ms | 721 ms | 1.4× | 21× |
| sort | 2,648 ms | 444 ms | 6.0× | 50× |
| entities | 4,325 ms | 377 ms | 11.5× | 79× |
| tree | 611 ms | 544 ms | 1.1× | 11× |
| text | 2,290 ms | 1,110 ms | 2.1× | 47× |
| double | 3,229 ms | 1,089 ms | 3.0× | 69× |
| LZO (fixture) | 985 ms | – | – | 21× |
| **score** | **23,464 ms** | | 3.9× (kernels) | **52×** |

- **Threads:** 1, 2 and 3 threads give 1.00×, 1.71× and 2.25× the throughput of one thread.
- **Memory:** latency is 57 ns within 256 KB and 275 ns over 32 MB. Copying runs at 203 MB/s.
- **The factor is 52×**, outside the 15–35× estimate. It splits into two parts:
  - **The hardware:** native code is roughly 13–25× slower than on the Mac.
  - **Translation:** it costs more on ARMv7 than on the Mac (3.9× native against
    1.1–5.8× on the Mac). The worst cases are the SSE-heavy kernels: vecmath 9.9×,
    entities 11.5×. Those run on scalar VFP code here.
- **For the game:** the main thread's 9.2 ms becomes about 480 ms a frame (about 2 fps).
  All threads' 27 ms is about 1,400 ms of CPU, or about 620 ms a frame over three cores.
  - That is **about 1.5–2 fps** in Florence.
  - Even with translation as cheap as native code, it would be roughly 4–5 fps.

The one-time device setup is in [platform/vita/README.md](../platform/vita/README.md).
After that, `tools/vita_bench.sh VITA_IP` copies, starts and reads back a run
from the Mac with nobody at the Vita.

#### The game for ARMv7

The runtime builds for the Vita (`platform/vita/vita.cmake`, VitaSDK's GCC 15.2,
`-mcpu=cortex-a9 -mthumb`):
- **Guest memory:** the window layout (`scimitar/guest_layout.h`) puts guest
  addresses from 0 in one host window: the image and the imported data below
  0x08000000, and the heap from there to the window's end.
  - On the Vita it is reserved with kubridge and committed as used.
  - `SCIMITAR_GUEST_WINDOW_MB=N` runs it on the Mac. Florence runs in a 1 GB window.
- **x87:** the 128-bit significands use two 64-bit words where there is no
  `__int128`; all 45,000 Unicorn checks pass with it.
- **SSE:** `sse.h`'s scalar fallbacks run on VFP, which is IEEE like SSE.
  ARMv7's NEON flushes denormals, so it would only fit where the game sets FTZ and DAZ.

Compiling the 64 parts of `gen/full` (AC2's 1.1 GB of C++) with `-O2`:
- **Time:** 85 minutes on 6 jobs, about 1 GB of memory per job. Part 0 took 2.6 min.
- **Code:** 134 MB of Thumb-2 `.text`, about as much as arm64, whose 64
  parts hold 130 MB.
- **`-Os`:** saves 20% (measured on one part).
- **Warnings:** only five loops warn that GCC bounds their trip count by pointer
  overflow, and no guest loop comes near those counts.

Loading it:
- **As a normal eboot**, the kernel loads the whole module into memory (as far
  as is known; `ac2-loadtest` checks), so all 134 MB of it would be resident. That leaves too little of the roughly
  365 MB for the game's 220 MB.
  - `ac2-loadtest` (`SCIMITAR_VITA_GAME=ON`, title `SCIM00002`) links all of it into
    an eboot that only starts and reports the memory left. It shows whether a
    module that big loads at all.
    - It links: one 134 MB text segment at 0x81000000, with the linker's long-branch
      veneers for Thumb-2 calls beyond ±16 MB.
    - The SELF is 65 MB compressed. It has not been run yet.
- **Only the code that runs needs to be resident.** On the Mac, after
  booting into Florence, 60 MB of the executable's 121 MB of `__TEXT` was resident.
  That is an upper bound: 16 KB pages, with read-ahead.
- **The loader approach:** keep the code in an uncompressed file on the card and
  page it in on demand. kubridge reserves the address space, commits a page on
  a prefetch abort, reads it, flushes the caches and resumes.
- **A smaller set doesn't hold up.** Following calls, vtables and taken
  addresses from `main` and the static initializers reaches 48,918 of the
  image's 208,710 functions (321 MB of C++). But 91,301 functions that set
  misses are referenced from data (factories, callbacks, descriptors), so the
  game would stop when it calls one.

#### Memory cuts (measured)

Same Florence run as the heap trace, with `SCIMITAR_GUEST_WINDOW_MB=1024`, which
also turns on the stack and shader cuts:

| | Before | After | How |
|---|---|---|---|
| **Guest heap, live** | **559 MB** | **353 MB** | |
| of which whole zero pages | 148 MB | 53 MB | |
| ARB program text | 108 MB | 0 | `scimitar/arb_store.h` |
| Metal buffer arena | 192 MB | 105 MB, 26 MB of it zero | 8 MB chunks, 4 classes per octave |
| Thread stacks | 30 MB | 4.5 MB | 256 KB per thread, 1 MB main |
| Audio (Wwise) | 41 MB | 37 MB | `/sounddefaultpoolsize:7680 /soundenginepoolsize:1024` |
| Highest point (carved) | 609 MB | 393 MB | |

- **ARB program text:** the loader copies each program's text into guest memory
  with `memcpy`; that is 14,129 copies and 110 MB in Florence.
  - The ARB program store writes the text to a file and keeps only a
    reference in the guest block.
  - It shrinks the block in place through the game's own allocator (Gear's
    dlmalloc, `internal_inplace_realloc` under its lock), which gave 89 MB back.
    The other 21 MB come from blocks it can't shrink; those are zeroed instead.
  - `glProgramStringARB` reads the text back from the file.
  - The game uploads programs as it first draws with them (about 1,000 in the
    first minute in Florence) and reads the text for nothing else.
  - Florence draws the same, with no translation errors.
- **Stacks:** the most any guest thread used was 60 KB, and the main thread 20 KB.
- **Audio:** the pool sizes are in KB. The defaults are 9 MB and 3 MB, and the
  default pool used 7.2 MB in Florence. `/nosound` drops all of the 37 MB.

What a Vita would have to back:
- **GPU buffers** belong in video memory (vitaGL's, CDRAM), not the guest heap.
  That is the 105 MB arena, so 248 MB of the heap is left.
- **Untouched pages** cost nothing if the window commits a page when it is
  first touched (kubridge's exception handler). That removes another 27 MB of zero pages.
- **Result: about 220 MB in Florence,** or about 185 MB with `/nosound`. The
  highest point would be about 290 MB.
- **A plain 512 MB window** (a 384 MB heap) runs out during the Florence load
  at 377 MB, because the arena is still inside it.

### Console data (Xbox 360, PS3, Vita)

Console data could only help memory, and the heap trace shows assets aren't the main
cost. So it isn't needed now:
- **Why it can't be used directly:** both versions are big-endian. Their textures
  are tiled (Xenos) or swizzled (RSX), and their shaders are console binaries
  rather than the ARB programs the PC executable translates.
- **What a converter would take:** loading them would mean rewriting every
  serialized object type's data. One approach is the PC executable's own readers
  with a byte-swapping stream, plus converters for textures, vertex formats and
  shaders.
- **What it would save:** at most part of the 130 MB of assets, for a conversion
  project bigger than the three cuts above.
- **Liberation's Vita data and executable don't help AC2.** It is a different
  game on a later engine branch, written as native code for the Vita's budgets.
  - What Liberation's 20–30 fps shows is what a game designed for three 444 MHz cores
    can do: a smaller simulation, fewer NPCs, and hand-tuned ARM code.
  - It doesn't show that AC2's simulation would fit: AC2's main thread alone already
    needs 140–320 ms there.

### Ways to get AC2 onto a Vita anyway

- **Streaming:** run the Mac port and stream it to the Vita with Moonlight
  (`vita-moonlight`) from a Sunshine host. This plays at full speed today, with
  no porting.
- **Tech demo** (progress in Toward a tech demo):
  1. Cross-compile the generated code with VitaSDK. Done: 134 MB of code.
  2. Add the 32-bit "Vita layout". Done: the window layout.
  3. Make the three memory cuts. Done on the Mac.
  4. Measure the CPU factor on the device. Done: 52×, about 1.5–2 fps in Florence.
  5. Write a loader that pages code in as it runs (kubridge).
  6. Write the Vita shell and a vitaGL backend with an on-card shader cache.

  The likely result is the title screen, or a few frames per second in a city, after weeks to months of work.

## Android (arm64)

The closest to iOS among the new targets.
- **Code:** the translated code builds with the NDK's clang as it does for iOS:
  arm64 and NEON, a 155 MB `.so`.
- **Memory:** a 2–4 GB reservation is ordinary on arm64 Android, and 8–16 GB phones fit the 1.5 GB working set.

Work:
- **GPU backend.** This is the bulk of the work. Android has no desktop GL, and no
  ARB programs, so the Metal backend's job needs another API. There are two choices:
  - **Vulkan**: the most direct. It needs ARB → SPIR-V (through GLSL and glslang).
  - **WebGPU through Dawn or wgpu-native**: a little more overhead, but the same
    backend and the same ARB → WGSL translator would then also serve the browser
    (Emscripten). Dawn runs on Vulkan on Android, and on Metal on the Mac, which
    makes testing easy.
  - Either way, a pipeline cache matters: 10k programs at about 30 ms each is minutes on first run.
- **BC textures:** most Adreno and Mali GPUs lack them. The backend already
  decodes BC on the CPU for the iOS Simulator (`decode_bc`). That works, but
  costs memory. Transcoding to ASTC at load time is the better long-term answer.
- **Audio:** AAudio or Oboe in place of AudioUnit, about 400 lines. FFmpeg (for Bink) builds for Android.
- **Shell:** GameActivity (surface, lifecycle, pause and resume). Controllers through
  Android's input events or Paddleboat, feeding the existing virtual HID pad.
- **Data:** 8.7 GB pushed with `adb` to the app's external files directory. Saves go next to it.
- **Performance:** a recent Snapdragon or Dimensity flagship should be in the
  iPhone's range. Mid-range phones will be well below 60 fps.

## Emscripten (browser)

Feasible in desktop Chrome. Firefox is likely. Safari depends on its WebGPU and memory limits.
- **Memory:** wasm32 is enough. The guest block is an offset inside linear
  memory, which only needs to grow to about 1 GB in use. Browsers don't back
  untouched pages with physical memory, so a 2 GB guest block costs little.
- **GPU:** WebGPU, with BC textures through `texture-compression-bc` on desktop.
  This is the same backend as Android if that one is written against WebGPU.
- **Threads:** this needs SharedArrayBuffer, so the page must be served with
  COOP/COEP headers.
  - The game runs on a worker (`PROXY_TO_PTHREAD`), because blocking waits aren't allowed on the browser's main thread.
  - Gear, Wwise and the loaders create threads at run time, and Wwise one per read.
  - So a preallocated worker pool is needed.
- **Data:** the user picks their AC2 folder, and it is copied into the origin-private
  file system (OPFS). That is 8.7 GB, within Chrome's quota on most disks. Reads
  go through OPFS sync access handles in the workers.
- **Audio:** an AudioWorklet. **Bink:** FFmpeg compiled to wasm.

Risks:
- **The module:** 2.4 GB of C++ becomes a wasm module of probably 150–300 MB.
  - Download and compile time in the browser are high, though V8's baseline compiler starts it.
  - The goto-based functions, the largest about 20k guest instructions, may give
    irreducible control flow. Binaryen's fix for that inflates code. This is the
    biggest unknown.
- **Speed:** wasm runs maybe 1.5–2× slower than native. WebGPU adds a cost per
  draw call, and the engine makes a few thousand a frame. Expect 30–60 fps on a fast desktop.
- **Publishing:** the wasm module is derived from Ubisoft's executable, so it
  can't be put on a public URL. Users would build it and host it on localhost.
  That removes most of the web's appeal ("just open a link").

## tvOS

Nearly the iOS port.
- The tvOS SDK has UIKit, Metal, AudioUnit (RemoteIO) and GameController.
- `shell_ios.mm` is the base. Touch input is dropped and controllers are already supported.
- Memory: the iPad peaked at 1.34 GB in Florence. The Apple TV 4K (3rd generation,
  A15, 4 GB) should hold that, but tvOS's per-app limit is undocumented: measure it first.
- The A12-based Apple TV 4K (2nd generation, 3 GB) is a stretch.

tvOS-specific problems:
- **Storage:** tvOS apps have no persistent Documents directory. Only Caches,
  which the system may purge when the app isn't running, and NSUserDefaults
  (about 500 KB). Options:
  - Copy the 8.7 GB into Caches with `devicectl`, as the iOS port does into
    Documents, and accept a re-copy after a purge. Whether `devicectl` copies into an Apple TV
    app container is untested.
  - Saves (about 52 KB) fit in NSUserDefaults or iCloud key-value storage, so they survive purges.
- **Input:** the Siri Remote alone can't play the game. A controller is required.
- **Build:** a CMake branch for `appletvos`, a provisioning profile with the Apple TV
  registered, and a re-signed bundle. The same steps as `games/ac2/ios/README.md`.

## Shared next steps

- **tvOS:** branch the iOS build, add Caches storage and saves in NSUserDefaults, and
  measure memory on the device. This needs an Apple TV 4K that is paired with Xcode.
- **One portable GPU backend:** WebGPU-shaped, written against Dawn or wgpu, with an
  ARB → WGSL translator next to `arb_msl.cpp`. Bring it up on the Mac first, where
  it can be compared with the Metal path frame by frame, then carry it to Android and the browser.
- **PS Vita:** the guest heap now fits (about 220 MB in Florence once GPU buffers
  sit in video memory); the 134 MB of ARMv7 code needs a demand-paging loader;
  the CPU factor, measured on the device, is 52× (about 1.5–2 fps in Florence), so a
  playable game is out of reach; what is left is a tech demo (see PS Vita).
