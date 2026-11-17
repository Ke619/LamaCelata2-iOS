# Metal rendering backend (design)

Status (2026-10-27): milestones 1 and 2 are done, and Florence already renders.
- The ARB → MSL translator (`platform/src/arb_msl.cpp`, `scimitar/arb_msl.h`)
  translates every program the game has, and Metal compiles all of them.
- The backend (`platform/src/metal/`) runs the game with
  `SCIMITAR_RENDERER=metal`. The title screen matches the GL path's, and
  Florence renders completely (lighting, shadows, characters, post-processing).
- Unoptimized, it benchmarks at 60-61 fps in Florence against GL's 51
  (see Milestones).

Status now: the backend is the renderer of the launcher apps on the Mac and the only
one on iOS, for AC2 and Brotherhood, with the shader precompile and cache (below).
The plain executables still default to the GL path (`platform/src/opengl.cpp`) unless
`SCIMITAR_RENDERER=metal`. The Windows ports draw through their own Direct3D 9 layer
on Metal (`platform/win32/d3d9_metal.mm`: D3D9 bytecode → ARB
(`platform/src/d3d9_arb.cpp`) → MSL, with its own render thread), which does not use
this shader cache.

## Why

In Florence (`/nobink /world:Firenze`, `SCIMITAR_BENCH=1200-3600`, M1 Pro) the
port runs at 50-55 fps, whatever the resolution or quality. The limit is Apple's
GL driver thread (OpenGLMT with the multithreaded engine on). It spends about
20 ms a frame on the engine's ~45k GL calls. Filtering redundant state on our side
gave nothing (see PLAN "Performance"), so the cost is in each call the driver
handles: validation, GL-to-Metal state translation, and implicit hazard tracking.
The goal is 120 fps. That needs a backend that knows exactly what this one engine
does and gives Metal only prebuilt objects: pipelines, depth-stencil states,
samplers and buffers that need no copies.

## What the game uses

The game's GL usage is narrow, and the backend implements only that.

**Programs.** The game uses only ARB assembly programs written by cgc 2.2: 24,911 in the
`.forge` archives and 126 in the executable. Programs that differ only in their
comments translate alike: the forge programs make 10,598 distinct translations. All constants come
through one `PARAM c[N] = { literals, program.env[a..b], program.local[a..b] }`
array per program. Only vertex programs use relative addressing (`c[A0.x]`, in
353 of the 1,025 distinct vertex programs). Fragment programs use `TEX 2D` and `CUBE`, and `TXP SHADOW2D` on
units 9 and 10. One executable program uses `3D`, one uses `ARB_draw_buffers`
(two outputs) and one writes `result.depth`. Opcodes: ABS ADD ARL CMP COS DP3 DP4
FLR FRC KIL LG2 MAD MAX MIN MOV MUL POW RCP RSQ SGE SIN SLT TEX TXP, with `_SAT`.
The game uses no `state.*` bindings, no fixed-function fog and no position
invariance.

**One Florence frame** (`SCIMITAR_TRACE_GL_FRAME=2000`, ~44k calls):

| Calls | What |
|---:|---|
| 10,785 | `glVertexAttribPointer` (generic attributes only) |
| 6,286 | `glProgramEnvParameters4fvEXT` |
| 5,572 | `glBindBuffer` |
| 3,654 | `glDrawElements`, all `GL_TRIANGLES` with 16-bit indices from an IBO |
| 3,145 | `glBindProgramARB`: 125 vertex and 109 fragment programs, 160 pairs drawn |
| 2,649 / 1,449 | `glActiveTexture` / `glBindTexture` |
| 1,894 / 950 | `glGetQueryObjectiv` / occlusion queries (`GL_SAMPLES_PASSED`) |
| 1,438 | `glTexEnvf(GL_TEXTURE_FILTER_CONTROL, GL_TEXTURE_LOD_BIAS, 0.0)`, always 0 |
| 960 | `glProgramLocalParameters4fvEXT` |
| 918 / 916 | `glEnable` / `glDisable`: alpha test and culling (~410 each), blend, stencil, depth test, polygon offset |
| 588 | `glAlphaFunc(GL_GEQUAL, ref)` |
| 289 | `glEnable`/`glDisableVertexAttribArray` |
| 286 | framebuffer binds and attachment changes: 4 colour targets plus depth and stencil renderbuffers, re-attached ~40 times |
| 74 | `glBufferData(NULL)` + `glMapBuffer(WRITE_ONLY)` + unmap (orphan and refill) |
| 72 | `glDrawArrays`: 35 triangle draws and 37 `GL_QUADS` draws |
| 13 | `glTexSubImage2D/3D` (dynamic textures) |
| 1 | `glBlitFramebufferEXT` |

The game never calls the CGL functions it imports, either through the title
screen or through the Florence load (checked with lldb). One GL context, made
current by the shell, carries every call. There is no `glBegin`/`glEnd`,
`glPushAttrib` or `glGet*` in a gameplay frame.

## Shape of the backend

```
guest GL call ─▶ platform/src/opengl.cpp shim ─▶ GL driver            (default)
                                          └────▶ metal::Gl (state, encode) ─▶ Metal
```

- **Selection**: `SCIMITAR_RENDERER=metal`. The recompiled code links its
  imports directly to the `imp_*` shims, so the switch is inside them. Each GL
  shim in `opengl.cpp` starts with `METAL(name)`, which forwards to
  `metal::m_<name>` when the Metal renderer is on. The list of names is one
  X-macro, `SCIMITAR_METAL_GL` in `platform/src/metal/shims.h`, so every GL
  import has a Metal twin. The CGL and `gluErrorString` shims are shared.
- **Code**:
  - `platform/src/metal/shims.mm`: guest arguments to host values, plus the
    simple state setters.
  - `platform/src/metal/backend.{h,mm}`: memory, objects, passes, pipelines,
    draws, queries and presentation.
  - The shell (`games/common/shell.mm` on the Mac, GL or Metal; `shell_ios.mm` on
    iOS, Metal only) picks the presentation path from the same switch (see
    Presentation).
- **Threading**: guest GL calls come from whichever thread holds the context:
  the main thread at startup, then the render thread. The backend has no
  locks on its hot path. It asserts that calls stay serialized, as GL's one
  context guarantees.
- **One or more command buffers per frame**: early commits at pass boundaries
  (Submission, below), the last at present. Encoding happens on
  the calling thread at first, which is the simplest design. If the render
  thread becomes the limit, a later step moves encoding to a worker thread fed
  by a command ring (see Performance).

## Shaders: ARB → MSL (done)

`scimitar::arb::translate(text, options)` returns MSL source, the constant buffer
layout and the program's interface (attributes read, varyings, textures per
unit, outputs, kill). It is plain C++, with no Metal.

**Contract with the backend.**
- Vertex: `vertex.attrib[N]` is `[[attribute(N)]]` (stage_in). NVIDIA's aliases
  of the conventional attributes map onto those slots (position 0, normal 2,
  colour 3, texcoord 8+n). Constants are `float4 P[]` at `[[buffer(0)]]`.
  Outputs are the position plus varyings `user(tc0..tc7, col0, col1, fog)`.
- Fragment: the varyings it reads, `P[]` at `[[buffer(0)]]`, `arb_fixed` (alpha
  reference and per-unit LOD bias) at `[[buffer(1)]]`, and `texture(N)`/`sampler(N)`
  for unit N (`texture2d`, `texturecube`, `texture3d`, `texture1d`, `depth2d` for
  `SHADOW2D`). Function constant 0 selects the alpha test. Fragment functions
  must always be fetched with `newFunctionWithName:constantValues:` (empty
  values = no alpha test).
- Vertex buffers therefore start at index 1 (attribute buffers 1-30).

**Constants.** Each program gets a compact constant block instead of GL's 256
environment registers. cgc pads its arrays: a fragment program typically declares
`program.env[1..127]` so that its locals land at `c[128]`, but reads only five of
them. The translator inlines literals and assigns one `P` slot to each
environment or local register the program actually reads (`Program::params`
lists them). A relatively addressed array gets a contiguous block, with its
literals in it. At draw time the backend gathers those registers into the frame's
constant ring. That is typically 5-20 float4s, compared with 2-3.4 KB for whole
`env` ranges. The largest block in the corpus is 290 float4s, a skinning
palette.

**Semantics**, as NVIDIA ran cgc's output:
- Temporaries start at 0, and outputs at (0, 0, 0, 1).
- `RSQ` and `LG2` use |x|, `POW` is `pow(|x|, y)` and `ARL` floors.
- Vertex colours are clamped to [0, 1].
- Relative addressing is clamped to the array.
- `vertex.attrib[N].secondary` is accepted (a cgc 2.2 quirk).
- `TXP SHADOW2D` divides by q and compares r. The result is (s, s, s, 1), which is
  `GL_LUMINANCE` depth mode.
- `KIL` discards.
- Alpha test is a specialization constant, so pipelines without it contain no
  `discard`. On Apple GPUs, any `discard` in a shader turns off hidden-surface
  removal for that draw.

**GL clip space → Metal.** Every vertex program ends with
`pos = (x, -y, (z + w) / 2, w)`. Negating y keeps every image in GL's row order:
memory row 0 is GL's bottom row, in textures and in render targets alike. So:
- texture uploads, render-to-texture and sampling need no flips;
- GL viewport and scissor rectangles are used unchanged as Metal rectangles,
  because GL's y from the bottom is Metal's row from row 0;
- `fragment.position` equals Metal's `[[position]]` (x, y, z, 1/w);
- winding is mirrored, so GL's `glFrontFace(GL_CCW)` becomes Metal's clockwise;
- the default framebuffer is drawn flipped once, when it is presented.

The tests check the row order ("rows bottom-up") and the depth mapping ("clip
z to depth").

**Position invariance**: `[[position, invariant]]`, compiled with
`preserveInvariance`. The engine draws the same geometry with different programs
(depth pre-pass, then `GL_EQUAL`/`GL_LEQUAL`), so positions must be bitwise
identical.

**Shader variants** (each a cheap re-translation):
- vertex: `extra_varyings`, outputs a fragment program reads but the vertex
  program never writes, as (0, 0, 0, 1). Metal refuses a pipeline whose
  fragment inputs are not all vertex outputs; GL would read undefined values.
- vertex: integer attribute masks, for non-normalized integer vertex formats
  (Metal does not convert those to float).
- fragment: the alpha function, as a specialization only, not a re-translation.

**Cost and caching.**
- Translating takes about 180 µs per forge program (comments included).
- Compiling the 10,598 distinct translations took 71 s on 10 cores, cold.
- Building all of them into render pipelines took 38.5 s, about 30 ms of CPU each.

So:
1. `glProgramStringARB` translates at once and starts an asynchronous library
   compile (`newLibraryWithSource:completionHandler:`), keyed by a hash of the
   MSL. Programs differing only in comments share it.
2. Pipelines are built on first use and cached in memory, keyed as below.
3. Built pipelines are kept on disk and compiled ahead of play: see "Shader
   precompile and cache" (`~/Library/Caches/scimitar-recomp/<game>/shaders`).

## State → cached Metal objects

The backend shadows the GL state it needs in plain structs with dirty bits. At
a draw it resolves only what changed:

- **Render pipeline** (`MTLRenderPipelineState`), keyed by:
  - the vertex and fragment translations, plus the vertex variant;
  - the vertex layout: format, offset relative to the binding and binding
    index per attribute (strides are dynamic, `MTLBufferLayoutStrideDynamic` with
    `setVertexBuffer:offset:attributeStride:atIndex:`, macOS 14);
  - colour attachment formats and sample count;
  - blend enable, factors and equations, and colour write mask (per attachment
    in Metal, from GL's single state);
  - the alpha function.

  The key is hashed into a flat table. In the Florence frame, 160 program pairs
  times a handful of layouts and blend states is likely a few hundred pipelines.
- **Depth-stencil state**, keyed by depth test, function and write, plus stencil
  enable, functions, ops, read and write masks (front and back). A few dozen.
- **Encoder state**, set only on change: cull mode, winding, fill mode
  (`glPolygonMode`), depth bias (`glPolygonOffset` with
  `GL_POLYGON_OFFSET_FILL`), viewport with depth range, scissor, stencil
  reference, blend colour.
- **Samplers**, keyed by the texture's parameters: wrap, filter, mip filter,
  anisotropy, LOD clamp, compare mode and function. Built and cached when the
  parameters change. The games never ask for anisotropic filtering (their
  `OGLState` starts at 1 and no option raises it); `set_forced_anisotropy`
  (`SCIMITAR_ANISO=2..16` at start, the F4 menu and the launcher's Graphics
  section) raises the linearly filtered, mipmapped textures' to at least that,
  dropping the textures' cached samplers when it changes. `SCIMITAR_ANISO_FROM=F`
  applies it from frame F on, for comparisons within one run.
- **Fixed state for programs**: alpha reference and per-unit LOD bias go into
  `arb_fixed`. The engine sends LOD bias 0 every time, but it is honoured anyway.

GL → Metal mappings are mechanical: blend factors and equations, compare
functions, stencil ops and cull faces. Notes:
- `GL_ALPHA_TEST` with `glAlphaFunc` becomes a pipeline specialization plus the
  reference in `arb_fixed`. The engine toggles it about 410 times a frame, but
  only between "off" and `GL_GEQUAL`, so at most two variants per program.
- Enabling `GL_POLYGON_OFFSET_FILL` sets depth bias; disabling it sets 0. Units
  need calibrating against GL on `Depth32Float` (no 24-bit depth on Apple
  silicon).
- Polygon mode `GL_LINE` becomes `MTLTriangleFillModeLines`.

**Constants per draw.**
- `glProgramEnv/LocalParameters4fvEXT` write into CPU arrays: 256 vertex and
  256 fragment environment registers, and locals per program.
- At a draw whose program or registers changed since that stage's last draw,
  the backend gathers `params` into a bump-allocated slice of the frame's
  constant ring and calls `setVertexBufferOffset`/`setFragmentBufferOffset`. No
  per-draw allocation and no `setBytes`.
- A generation counter per register file makes the "unchanged" check one
  comparison.

## Shader precompile and cache (done)

First-time shader compiles were the stutter on a first view of a scene: a
cold ACB run of 6,000 frames (title, intro cutscenes, first gameplay) spent
36.4 s blocked compiling, worst frame 13.2 s; the same run with the system's
Metal cache warm spent 18 ms. `platform/src/metal/shader_cache.mm` removes it
like a PC game's "compiling shaders" step:

- **Disk cache**, per game, in `~/Library/Caches/scimitar-recomp/<game>/shaders`
  (`SCIMITAR_SHADER_CACHE=DIR` overrides): `programs.bin` (the ARB texts the
  game used), `pipelines.bin` (each pipeline drawn with: its two programs, the
  vertex variant, the alpha test and `PipeDesc`: formats, blending, vertex
  layout), `forge.bin` (the programs of the game's archives), and per
  translator version `v<hash>/archive.bin` (an `MTLBinaryArchive`) and
  `done.bin`. The version hashes `arb::prelude()` and two sample
  translations, so a translator change compiles everything again.
- **The game's programs, from its data**: the `.forge` archives hold every ARB
  program as text in their LZO1X sections (codec 0 in AC2, 1 in ACB; one
  decoder). A one-time scan (about 11 s) finds 28,645 distinct programs in
  ACB's 56 archives. That is the complete program list, without playing; the
  pairs and fixed-function state of whole pipelines are known only from draws,
  hence the recording.
- **Startup precompile** (`ShaderCache::startup`): everything not yet compiled for this translator
  version (recorded pipelines into the binary archive, archive programs into
  the system's Metal cache) on all cores. From the launcher, Play runs it
  first with counts and a percentage in the launcher's window
  (`scimitar_shader_precompile`, games/common/launcher/README.md); without
  the launcher the game runs it at its first frame behind a plain bar.
  Progress is saved as it goes. Cold, ACB's 28,645 programs take about 8.5
  minutes once; later launches compile only what is new.
- **Recording**: each new pipeline is appended to `pipelines.bin` at once, and
  goes into the archive at the next launch.
- **Background fallback**: a function or pipeline still missing at a draw
  compiles on another thread. The frame waits at most
  `SCIMITAR_SHADER_WAIT_MS` (12) in total for such compiles, and a draw whose
  pipeline is still compiling is skipped (an object can be missing for a
  frame or two the first time). `SCIMITAR_SHADER_ASYNC=0` waits instead (the
  old behaviour).

Switches: `SCIMITAR_PRECOMPILE=0` (no startup step), `=recorded` (no archive
programs), `=background` (archive programs after the first frame on
`SCIMITAR_PRECOMPILE_THREADS` (2) low-priority threads: measured, it still
costs the game frames, so it is not the default); `SCIMITAR_PRECOMPILE_FORGE=0`
skips the scan. On iOS the default is `recorded` with no scan (a phone's
launch would block for many minutes); setting `SCIMITAR_PRECOMPILE` turns them on.

Measured (6,000 frames from the title through the opening cutscenes, hidden,
`SCIMITAR_HITCH_MS=20`; "cold" = the system's Metal cache removed):

| Run | Frames over 20 ms | Their time | Compiling in them | Worst compile |
|---|---|---|---|---|
| ACB cold, before | 49 | 37.8 s | 36.4 s | 13.2 s |
| ACB warm system cache, before | 31 | 1.0 s | 18 ms | 6.8 ms |
| ACB cold, background fallback only | 60 | 4.9 s | 320 ms | — |
| ACB cold, archive programs compiled during play on all cores | 1,173 | 111 s | 1.9 s | 219 ms |
| ACB cold, the same on 2 low-priority threads | 190 | 16.5 s | 416 ms | 112 ms |
| **ACB cold, after the startup precompile** | 33 | 1.3 s | 47 ms | 3.5 ms |
| AC2 cold, before | 379 | 27.6 s | 19.0 s | 8.3 s |
| **AC2 cold, after the startup precompile** (24,911 programs, 445 s once) | 222 | 5.8 s | 63 ms | 7.5 ms |
| AC2, next launch (159 recorded pipelines into the archive in 0.6 s) | 218 | 5.4 s | 11 ms | 2.5 ms |

Ideas checked and dropped (whole pipelines before their first draw):
- **Pairs from the archives**: a material object stores a block of vertex
  programs and then a block of fragment programs (140 and 20, 280 and 200 in
  `DataMAC_LGS302_Catacombs.forge`), not pairs, and the material programs'
  headers carry no feature switches to match them by. Every combination would
  be thousands of guesses per object.
- **Vertex fetch in the shader** (pipelines independent of the vertex
  layout): of 897 pipelines recorded in ACB play, no program pair was ever
  drawn with two layouts; removing the layout from the key leaves 897. New
  pipelines are new program pairs (671 pairs, 1.3 pipelines each).
What is left: the recording (a pipeline seen once is in the archive from the
next launch on) and the wait budget.

The frames still over 20 ms after the precompile are not compiles: they are
the same ones a warm-cache run has (loading, streaming, video). The compiler
uses every core during the precompile (one `MTLCompilerService` at about
860% CPU on 10 cores, none idle), so its length is the compile work itself.

## Buffers without copies

Guest memory is host memory, and Metal can wrap host memory:
`newBufferWithBytesNoCopy`, page-aligned, on Apple silicon's unified memory.

- **GL buffer storage lives in guest memory.** A GPU arena of 64 MB chunks is
  allocated from the guest heap with 16 KB alignment. Each chunk is wrapped once
  as an `MTLBuffer` and sub-allocated for buffer objects. A GL buffer is then an
  (arena chunk, offset, size) triple with a guest address.
- **`glBufferData(data)`** copies once into the arena, like any driver.
- **`glMapBuffer` returns the guest address of the storage itself**: no staging
  copy, no upload at unmap, no driver round trip. This replaces today's staged
  write-only maps.
- **Orphaning**: `glBufferData(NULL)`, or a map of storage the GPU may still read,
  renames: the buffer gets fresh storage from the arena, and the old storage is
  freed when the frame that used it completes (frame serial numbers).
- A write-only map without orphaning on in-flight storage also renames, and
  copies the old contents first, because GL preserves bytes not written. That is
  rare.
- **Client-side arrays** (attribute pointers without a VBO) and immediate data
  are copied into the frame ring at the draw. Index ranges are found by scanning
  the indices, as GL drivers do. The Florence frame has none.
- **Vertex bindings**: attributes that share a buffer and stride become one
  Metal vertex buffer binding. The smallest attribute offset becomes the binding
  offset (`setVertexBuffer:offset:` or `setVertexBufferOffset:`), and the rest
  go into the pipeline's vertex descriptor as relative offsets. The pipeline key
  therefore does not change when the engine draws from another part of a big VBO.
- **Disabled arrays** read the attribute's current value (GL's
  `glVertexAttrib*`, default (0, 0, 0, 1)). It is bound as a 16-byte buffer with
  `MTLVertexStepFunctionConstant`.
- **Index buffers**: `drawIndexedPrimitives` with the IBO's chunk and offset.
  Metal wants the offset aligned to the index size. The engine's 16-bit offsets
  should be; a misaligned one would be copied to the ring.
- **`GL_QUADS`** (37 small draws a frame) goes through a static index buffer
  (0 1 2 0 2 3, ...), with client data copied to the ring. Fans and polygons can
  use the same path if they show up.

## Textures

- **Formats** (to confirm with a census of the `glTexImage*` formats at load):
  - DXT1/3/5 → BC1/2/3 (Apple silicon Macs support BC; so do iPhones and iPads
    from the A17 Pro and M1 on). Where the GPU has no BC formats the blocks are
    decoded to RGBA8 on upload (`decode_bc`; `SCIMITAR_NO_BC=1` forces it). RGB DXT1 gets an
    alpha-one swizzle, because Metal's BC1 decodes the 3-colour block's black as
    transparent.
  - BGRA8 and RGBA8 → `BGRA8Unorm`/`RGBA8Unorm`.
  - Luminance, alpha and luminance-alpha → R8/RG8 with texture swizzles.
  - Half and float formats → as is.
  - `DEPTH_COMPONENT24` → `Depth32Float`, and `DEPTH24_STENCIL8` →
    `Depth32Float_Stencil8`.
- **Uploads**: `replaceRegion` when no command buffer in flight uses the texture
  (per-texture last-use serial). Otherwise the texture is renamed: new storage,
  with a blit copy of the old contents when only a sub-rectangle changes. That
  covers the Bink video textures and the 13 dynamic updates a frame without
  stalls. Mip levels arrive one by one, as in GL, so storage is created for the
  full chain from level 0's size. A missing level is generated if the engine
  asks for mipmaps.
- **Depth textures sampled as colour** (`TEX 2D` on a depth texture) need GL's
  (d, d, d, 1). They get a texture view swizzled RRR1.
- **Shadow units without a depth texture**: the engine sometimes leaves a colour
  texture on units 9 and 10. Metal cannot bind it to a `depth2d`, so the same
  lit 1x1 depth texture as the GL path's fallback is bound.
- Cube maps and 3D textures map directly; 1D textures have no mipmaps in Metal
  (none are used).

## Framebuffers and render passes

- **The render pass is derived, not explicit.** GL framebuffer binds and
  attachment changes only mark the attachments dirty. The next draw or clear
  compares the wanted attachments with the open pass. If they are the same, the
  pass continues. The engine re-attaches the same renderbuffers ~40 times a
  frame, and most of those must not split passes.
- **Load and store**: an attachment cleared by the first `glClear` in a pass
  gets `MTLLoadActionClear`, and no clear is drawn. Other attachments load,
  because GL keeps contents. Everything stores, except the default framebuffer's
  depth after the last pass of the frame.
- **A `glClear` in the middle of a pass**, or with a scissor or masks, becomes a
  full-viewport clear draw with a built-in pipeline.
- **`glBlitFramebufferEXT`**: a blit-encoder copy when sizes and formats match,
  an MSAA resolve when the source is multisampled, otherwise a scaled draw.
- **`glReadPixels`** and any other read-back commit the frame so far and wait.
  This is rare: screenshots, and perhaps save thumbnails.
- **The default framebuffer** is an offscreen colour texture plus depth-stencil
  at the game's mode size. It replaces `kCGLCPSurfaceBackingSize`; see
  Presentation.

## Occlusion queries

- Each frame has a visibility result buffer in counting mode
  (`MTLVisibilityResultModeCounting`), attached to every render pass of the
  frame.
- `glBeginQuery` assigns an 8-byte slot and sets
  `setVisibilityResultMode:offset:`. `glEndQuery` sets it back to disabled. A
  query that spans a pass boundary continues in a new slot, and the results are
  summed.
- A query's result is ready when its frame's command buffer completes (the
  completion handler advances a "completed frame" serial).
- The game reads the previous frame's queries, about 950 a frame. Availability
  is "has that frame completed". `GL_QUERY_RESULT` on an incomplete frame waits
  for that command buffer, as GL does.
- Brotherhood also reads queries of the frame it is drawing
  (`OGLOcclusionQuery::GetVisiblePixels` spins on availability with
  `pthread_yield_np`), so the render thread waits for the GPU there: in Rome at
  the highest settings that was ~15% of it. The early commits below cut that
  wait.
- Tried and dropped: answering an unresolved query with the last result of the
  same query object (`SCIMITAR_QUERY_LAG`). It ran Rome at 62 fps instead of
  47, but the game reuses query objects for other nodes, so a camera pan showed
  sky through holes in the ground and buildings.

## Submission, pacing and memory

- The frame's last command buffer is committed at `MacUpdateOpenGL` after the
  present pass (earlier ones at pass boundaries, below).
- At most 2 frames in flight, with a semaphore signalled by the completion
  handler.
- Per-frame rings (constants, client arrays, upload staging) grow on demand and
  are reused once their frame completes.
- Deletes are deferred by frame serial: buffers, textures, renderbuffers and
  programs.
- Early commits: at a render pass boundary, once 300 draws have been encoded
  since the last commit (`SCIMITAR_COMMIT_DRAWS`, 0 turns it off), the command
  buffer is committed and a new one started. A boundary costs no extra
  attachment loads or stores, and the GPU works while the rest of the frame is
  encoded, so a wait on a query (above) or on the frame ends sooner. Rome,
  highest settings, frames 1500-3600: 48.6 fps without, 60-75 with 200-500
  draws (300: 60.0, 65.9, 74.9 over three runs); 100 draws gave 55.8, 1000 gave
  52.1. AC2's opening is unchanged (112 fps either way).
- Guest locks (`platform/include/scimitar/guest_mutex.h`) are an owner word and
  a count, for the Mac ports' pthread mutexes and the Windows ports' critical
  sections alike: a held lock fails a trylock with one load. Brotherhood's
  `Gear::AdaptiveLock` spins 4096 trylocks before it blocks, and the host
  `std::recursive_mutex` slow path was ~10% of its main thread. Critical
  sections are also found without the global lock every Enter and Leave took.
- `update_shader_constants` (vertex and pixel), called for every draw over the
  whole constant range, is native in AC2 and Brotherhood
  (`games/common/native_constants.h`, `games/*/native_gfx.cpp`).

## Presentation (the shell's side)

`games/common/shell.mm` owns an `NSOpenGLContext` on the GL path. With Metal:

- `MacInitializeOpenGL` makes the view layer-backed with a `CAMetalLayer`
  (BGRA8, `framebufferOnly`, `maximumDrawableCount` 3) and creates the device
  and queue. No GL context is created.
- `MacSetContextOpenGL` no longer needs the context handover lock:
  `[NSOpenGLContext update]` and its frame-boundary handshake go away. The main
  thread only publishes the view's new size, and the render thread applies it to
  `drawableSize` at the next present.
- `MacUpdateOpenGL` encodes the present pass: it takes the next drawable as
  late as possible, draws the offscreen default framebuffer flipped, scaled and
  letterboxed to the window's aspect (what the GL path gets from the backing
  size), then `presentDrawable` and commit. The F6 statistics overlay and
  `SCIMITAR_SCREENSHOT` read from the offscreen framebuffer.
- `MacVSyncOpenGL` sets `displaySyncEnabled`.
- `MacSwitchModeIfNeeded` resizes the offscreen default framebuffer instead of
  setting the CGL backing size.

## GL calls outside the draw path

- `glGet*` is answered from tracked state and fixed limits.
- `glGetError` returns tracked errors, normally none.
- `glCheckFramebufferStatusEXT` is "complete" for valid attachment sets.
- `glPushAttrib`/`glPopAttrib` save and restore the tracked state groups their
  masks name.
- Immediate mode (`glBegin`/`glVertex3f`/`glTexCoord2f` with the matrix stack
  and `gluPerspective`) gets a small built-in fixed-function pipeline fed from
  the ring. It never appears in gameplay frames; it may appear at startup.
- The CGL entry points remain for completeness but stay unused.

## Performance expectations

GL's per-call driver cost is replaced by:
- a few hash lookups and dirty-bit checks per draw;
- about 4-8 Metal encoder calls per draw (pipeline when changed, depth-stencil
  when changed, 1-3 buffer offsets, textures when changed, draw);
- one gather of 100-300 bytes of constants per draw.

On M1-class CPUs that should be roughly 1-2 µs per draw, so 4-8 ms of encoding
for 4,000 draws. Today that work happens on GL's thread at about 20 ms. If
encoding on the render thread then limits the frame, the next step is GL's own
trick, done cheaply: the render thread records compact draw packets (resolved
pipeline, buffers, offsets, gathered constants already in the ring) into a
lock-free ring, and a worker thread replays them into the encoder.

## Milestones

1. **Translator** (done). `tests/metal/arb_msl_test.mm`:
   - Without the game: 66 synthetic programs covering every opcode, modifier,
     binding form and texture target. Each is run on the GPU and its pixels are
     compared with values worked out from the ARB specs. Also checked: the
     varying contract, integer attributes, the row order and depth mapping, MRT,
     `result.depth`, and the alpha-test specialization. 12 malformed programs
     must be refused.
   - With the game (`ctest`: `arb_msl_forge`, `arb_msl_ac2`): all 24,911 forge
     programs and the 126 in the executable translate. All 10,598 distinct
     translations compile, and all of them also build as render pipelines
     (`--pipelines`, 38.5 s).
2. **Title screen on Metal** (done). Everything in this document is in place,
   except the encoding thread.
   - Title screen: it goes through the same sequence as on GL (gray loading
     clear, white fade-in, the logo assembling), and the settled screen
     matches.
   - Frame numbers differ between runs, because loading runs on other threads
     and the animation follows the wall clock. Compare states, not frame
     numbers.
   - Florence (`/nobink /world:Firenze`, `SCIMITAR_BENCH=1200-3600`, M1 Pro,
     2560x1440) also renders completely:

     | | fps | p50 | p95 |
     |---|---:|---:|---:|
     | Metal | 59.6-61.3 | 16.2-16.7 ms | 17.5-18.7 ms |
     | GL | 51.1 | 19.4 ms | 21.3 ms |

   - A Florence frame: ~3,400 draws, ~32 render passes, 6-8 clears, ~30 buffer
     renames, ~1 command-buffer split (a query polled twice within its frame).
     About 220 pipelines are built for the whole run.
   - Simplified for now:
     - multisampled renderbuffers are single-sampled;
     - immediate mode and draws without programs are skipped (none seen);
     - depth texture uploads with data are ignored;
     - `glReadPixels` reads only 8-bit colour targets;
     - render targets use shared storage;
     - depth bias is not calibrated.
3. **Florence correct**: compare screenshots against the GL path in the same
   game states (menus, videos, a save loaded, night, interiors), check the
   shadow bias, and look at a Metal GPU capture for wrong passes.
4. **Florence fast**: the render thread is now the limit; it is busy all the
   time.
   - In a sample, about 54% of it is the game's own recompiled code. The rest
     is the backend and the Metal driver: draw, constants, encoder calls and
     Objective-C messaging.
   - Next steps:
     - move encoding to a worker thread (see Performance);
     - set only the state that changed, using dirty bits instead of per-draw
       pipeline keys;
     - keep colour render targets private (depth and stencil already are).
   - Metal is the apps' default; the plain executables still start on GL.
   - `SCIMITAR_VSYNC=0` keeps the layer from waiting for the display. The game
     turns vsync off itself, so it makes no difference in the benchmark.

A useful tool from milestone 2 on is a **frame capture and replay**: dump one
frame's GL calls plus the buffer, texture and program data they reference. The
dump goes to a file outside the repo, because it is game data. Replaying it
offline through both backends allows image diffs without running the game.
Metal's GPU frame capture (`MTLCaptureManager`) then shows each pass and draw.

## Debugging tools

- `MTL_DEBUG_LAYER=1 MTL_DEBUG_LAYER_ERROR_MODE=nslog`: Metal API validation,
  logged instead of aborting. The title screen and Florence report no errors.
- F13 (Metal), or `SCIMITAR_METAL_DUMP_FRAME=N`, dumps a frame to
  `/tmp/ac2dump/<time>/` (`platform/src/metal/dump.mm`), with a notice on
  screen:
  - every pass's colour and depth as images;
  - `frame.txt`: each pass's attachments (with the GL framebuffer's own) and
    each draw's shaders, textures (with their GL names), constants and state;
  - `programs/`: the shaders' text.
- `SCIMITAR_TRACE_GL_FRAME=A-B` logs every GL call of those frames, on either
  renderer. `SCIMITAR_TRACE_NAMES=1` logs object names and texture uploads
  (Metal); `SCIMITAR_TRACE_GL_THREADS=1` logs the threads making GL calls.

## Backlog

- **Stutter in play** (reported 2026-10-27): shader and pipeline compiles on first
  use. Resolved: see "Shader precompile and cache".
- **Render-thread game code**: native replacements of the hot math
  (`games/ac2/native_math.cpp`) took Florence on Metal from about 60.5 to
  65.4-67.4 fps. GL barely changes (51 to 53), because its driver thread is its
  limit.

  The game code is still ~51% of the render thread. Its next hot spots are
  bigger engine functions (in profile samples):
  - `SkinnedPrimitive::Setup` (93), 2.5 KB of engine logic;
  - `update_shader_constants` (60), now native (`games/*/native_gfx.cpp`);
  - `ColorBalance::FillVolumeTexture` (57), which refills the 16³ grading LUT
    every frame;
  - `LODSelectorInstance::AddHWGraphicObjectInstances` (42).

  The backend and the Metal driver are another ~36%; the encoding thread and
  dirty-bit state are the levers there.
- **MetalFX** (the macOS 27 SDK has all of it). The GPU spends ~7.4 ms of a
  ~15 ms Florence frame at 2560x1440 (`SCIMITAR_TRACE_GL` prints GPU time a
  frame). The game is CPU-bound, so upscaling alone does not raise fps yet.
  - *Spatial upscaler*: small and low-risk. Upscale the default framebuffer
    into the drawable in the present pass (instead of bilinear), behind a
    switch. It is a quality option now. Once the CPU work is cut and 120 fps is
    near (8.3 ms budget), rendering at a lower internal size becomes the GPU
    lever.
  - *Temporal upscaler*: needs a jittered projection and motion vectors, which
    the engine does not render.
  - *Frame interpolation* (`MTLFXFrameInterpolator`): would show ~2x the
    rendered frames, with more latency. It needs, each frame:
    - current and previous colour, and depth;
    - motion vectors;
    - near/far planes, field of view and the camera matrices;
    - the HUD in its own texture.

    A prototype could take camera-motion vectors from depth and the camera
    matrices found among the uploaded constants, and treat the draws after
    post-processing as the HUD. Animated characters, cloth and crowds would
    smear without per-object motion.
- **Cutscene depth of field without its depth input** (2026-10-27,
  intermittent). Cutscenes come out blurred and darker.
  - The engine packs depth into an RGBA8 target (`ShadePixel`, `(z/w+1)/2` in
    R, G, B), and `depthoffield.hlsl` reads it at unit 2.
  - In bad sessions that GL texture had no storage, so the backend
    substituted its 1x1 stand-in.
  - The engine deletes and regenerates its render targets (at loading) and
    keeps using the old texture number, which Apple's GL hands out again. The
    backend now allocates names the same way (per kind, lowest free first),
    which fixed a session where texture 17785 was left empty.
  - One later session still had the regenerated texture 5 empty, before
    uploads were logged. A later session with the same delete/regenerate
    sequence was correct.
  - If it returns:
    - run with `SCIMITAR_TRACE_NAMES=1` (every generated and deleted name and
      every `glTexImage*`, with the unit's bindings);
    - press F13 in the bad scene; the dump names the empty texture;
    - `SCIMITAR_TRACE_GL_THREADS=1` showed a single GL thread.

## Risks and open points

- **Depth precision and bias**: `Depth32Float` instead of D24 changes the
  polygon-offset scale and may change z-fighting. Calibrate against the GL
  path's images.
- **Pipeline compile hitches** on first sight of a program pair (~30 ms each,
  cold): handled by the startup precompile and background fallback (see
  "Shader precompile and cache").
- **Query waits** if the GPU runs more than a frame behind; see the fallback
  above.
- Formats and read-backs not seen in Florence (menus, videos, saves) need the
  census from milestone 2.
