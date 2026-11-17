# AC2 with the Windows release's data

Status (2026-10-28): the recompiled Mac executable (`build/ac2`) runs on the
Windows release's archives and videos. The title screen and Florence
(`/nobink /world:Firenze`) render on both renderers (GL, and Metal at about
60 fps, as with Mac data), with characters, shadows and post-processing. Sound
works with the Mac sound packs; the PC packs load but stay silent (below).

```bash
tools/pc_bundle.sh "/path/to/Assassin's Creed 2.app" \
    "/path/to/Assassin's Creed II" /tmp/ac2pc     # the Mac app, the Windows install
build/ac2 "/tmp/ac2pc/Assassin's Creed 2.app" /nobink /world:Firenze
```

`tools/pc_bundle.sh` builds a bundle of symlinks only (no game data is copied):
the Mac executable, `Info.plist` and `media/`, the PC archives, videos and
(with `--pc-sound`) sound packs. The engine builds archive names as
`Data%s.%sforge` with its platform name, so `DataPC_X.forge` is linked as
`DataMAC_X.forge`; nothing else in the names matters.

The conversion is `games/ac2/pc_data.cpp` (records converted as they load) and
`platform/src/d3d9_arb.cpp` (D3D9 shader bytecode to ARB assembly). With Mac
data both are idle: every hook passes Mac records to the original code.

## What differs between the archives

Found by decompressing the same entries from both sets (the archives index the
same file IDs) and comparing them byte by byte, with the classes named by their
type hashes (CRC-32 of the class name, e.g. `ShaderPermutation` = 0x9467f2bb).

| | Mac | PC | Handling |
|---|---|---|---|
| Container, index, block layout | version 25 | same | none |
| Compressed blocks | LZO1X_1 (type 0) | LZO2A (type 2) | none: the Mac engine has LZO2A |
| Textures (DXT etc.), meshes, animations | | byte-identical (checked: a terrain texture, a world cell, a character) | none |
| Object IDs | | differ throughout | none: only references between objects of the same set |
| `ShaderPermutation`, `LightingShaderPermutation` | ARB text + constant map | D3D9 bytecode, no constant map | translated (below) |
| `HavokChunk` | packfile in the 4th of 4 arrays, GCC layout rules | in the 1st of 3, MSVC layout rules (`reusePaddingOptimization` 0) | moved; the one header byte set (the packfiles are otherwise identical) |
| `CompiledMeshInstance` +0x2c | 2 | 0x20 | set to 2 (use not traced) |
| `MeshPrimitive` 2nd word | 2 | 0x20 | none: only its low bit is kept |
| `CompiledRealTreeMesh` (trees) | | a few bytes per record differ | **not handled** |
| Wwise sound banks (`.pck`) | bank version 48 | bank version 44 | **not handled**: use the Mac packs |
| Bink videos | | same codec | none (FFmpeg plays both) |
| Text locales | Russian | Chinese, Korean, Polish instead | untested |

### Shader records

`ShaderPermutation::FastLoad` reads 15 flag bytes and a u32, the program
(u32 length, bytes), the material parameters (u32 count, 30-byte
`ShaderParameter`s), then on the Mac an array of `ShaderConstantInfo`
(u16 register, u16 index into `scimitar::g_OGLShaderConstantNames`, the
engine's 325 shader constant names), then 8 bytes. `LightingShaderPermutation`
has two u32 before the program and no material parameters. PC records lack the
`ShaderConstantInfo` array: the PC engine builds the same map at load from the
bytecode's constant table with `D3DXGetShaderConstantTable` (one of the two
d3dx9 functions `AssassinsCreedIIGame.exe` imports).

`pc_data.cpp` reads a PC record whole (through the serializer's buffer,
refilling it with `FastLoadSerializer::ReadFromStream` as the engine does),
translates the bytecode, turns each constant-table entry whose name is in
`g_OGLShaderConstantNames` into a `ShaderConstantInfo` (arrays and structures
get register 0, as in every Mac record), and runs the original FastLoad on the
record rebuilt in Mac form from a guest buffer of its own, then puts the
serializer back where the PC record ended. Registers agree between the builds:
the HLSL pins them, and every constant the Mac programs name sits in the same
register in the PC constant tables (checked over the 2,130 shaders of the
bootstrap archive). Two booleans are bool registers on PC and float constants
on the Mac (`g_DeconstructionEnabled` c79, `g_HasSun` c77): the translator reads
those floats.

The engine reaches these FastLoads only through member-function pointers and
vtables (no direct calls in the binary), so `install_pc_data_support` (run by
`main.cpp` through `game::Info::start`) swaps their entries in the runtime's
function table; no re-translation is involved.

## The shader translator

`scimitar/d3d9_arb.h`: vs_3_0/ps_3_0 bytecode to `!!ARBvp1.0`/`!!ARBfp1.0`, in
the Mac programs' conventions, so the engine binds them as its own:

- Float constants are `program.env[n]` at their D3D register; fragment registers
  from 128 (the material's) are `program.local[n]`, as in the Mac programs.
  `def` literals take their slots in the `c[]` array.
- Vertex inputs are generic attributes by usage: position 0, blend weight 1,
  normal 2, colour 3-4, blend indices 7, texture coordinate n 8 + n, tangent 14,
  binormal 15 (Cg's bindings in the Mac programs). Vertex outputs and fragment
  inputs pair by usage (`result.texcoord[n]`, `result.color`).
- Matrices the constant table marks column-major are read transposed: the engine
  uploads a row per register (the Mac programs compute `x*c0 + y*c1 + ...`),
  the PC code takes `dp4` with columns. Single matrices are transposed into
  temporaries once; arrays of them (grass instancing) are gathered per read.
- `if`/`else` (bool and comparison) are flattened: both sides run, and each write
  inside a block is merged into its register under the block's mask (`CMP` in
  fragment programs; `x + m * (new - x)` in vertex programs, whose `RCP`, `RSQ`,
  `LG2`, `EX2` and `POW` get a harmless operand where the mask is 0, so an
  infinity from the side not taken cannot become NaN; without that guard most
  of Florence vanished). `rep` loops with `defi` counts are unrolled.
- Expanded: `lrp`, `cmp` and `sincos` in vertex programs, `nrm`, `dp2add`,
  `sgn`, `m4x4`-style macros, `mova` (rounds, ARL floors), large relative offsets
  (ARB stops at 63), abs source modifiers, saturation in vertex programs, and
  ARB's one-parameter/one-attribute-per-instruction rule.
- Approximated: `vFace` is +1 (ARB fragment programs cannot tell faces apart,
  nor could the Mac build's); `texldl` samples with implicit LOD (ARB has no
  `TXL`). `g_ProjectorCookies` samplers are `SHADOW2D`, as on the Mac.

MojoShader was the obvious prior art, but its ARB profile needs NVIDIA's flow
control extensions for shader model 3, which neither Apple's GL nor our ARB →
MSL translator has; the game's shaders need flattening instead. DXMT translates
Direct3D 10/11, not 9.

Coverage (`build/d3d9_arb_check FILES...` compiles each translation with this
Mac's GL and the ARB → MSL translator; `-m` also with Metal): the 10,762
distinct shaders of all 26 PC archives (1,147 vertex, 9,615 pixel) translate
and compile on both except 24 tree vertex shaders (below); Metal compiled all of
a random sample of 300.
`ctest -R d3d9_arb` checks hand-assembled shaders on the GPU (transposed
matrices, flattened bool and comparison blocks with an infinity in the side not
taken, unrolled loops, `cmp`, `lrp`).

## Backlog

What is missing, and what it would take. Sound uses the Mac packs for now
(`tools/pc_bundle.sh` links them unless `--pc-sound`).

| Item | Effect | Cost |
|---|---|---|
| Wwise banks version 44 → 48 | the PC sound packs are silent (the Mac packs work: same project, same event IDs). `CAkBankMgr::ProcessBankHeader` accepts version 48 only (`AK_WrongBankVersion` otherwise); with the version rewritten to 48 as the packs were read (an experiment, not kept) the engine crashed parsing the first bus in the HIRC section (`CAkBus::SetInitialFxParams`), so the object layouts differ, not just the number | high: a bank converter for the HIRC object changes between the two Wwise versions (several days, with the community's bank documentation). Workaround in place: the Mac packs |
| Trees (`RealTree`) | 24 tree vertex shaders read texture coordinates 9-10, which ARB cannot address; they fall back to drawing nothing. The Mac's tree shaders use other inputs, and `CompiledRealTreeMesh` records differ by a few bytes each | medium (about a day): find which attributes the Mac engine binds for tree vertex formats, map 9-10 there, check the record differences |
| `texldl` with an explicit LOD | sampled with the implicit LOD | small for Metal (`TXL` in the ARB → MSL translator, about two hours); not possible in ARB for GL |
| `vFace` | back faces of two-sided materials lit as front faces | small for Metal; not possible in ARB |
| Other worlds | Venice, Tuscany, Romagna, the DLC and the rest untested; their shaders are in the coverage above | a play-through, fixing what shows up |
| Locales | PC text locales untested in the Mac engine | small, if the engine's language list accepts them |
| Small artifacts | a white quad and dark marks seen once in a GL frame of Florence | unknown |

The PC executable itself was only used for evidence (its imports, its PDB path);
nothing of it runs.
