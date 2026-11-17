# Forge test (needs the game locally)

`forge_test` reads the real `DataMAC.forge` with the engine's own code, translated
from the local AC2 `scimitar_sf` at build time (output in `build/gen/`, never
committed). CMake skips it when `AC2_APP` does not point at the game or Unicorn is
not installed.

```bash
build/forge_test "/path/to/Assassin's Creed 2.app" [--dump]
build/forge_test "…/Assassin's Creed 2.app" --only-emulated   # original code only
```

## What it runs

1. **Startup**: `_GLOBAL__I__ZN8scimitar27CreateXDecompressionContextEv`, the static
   initializer that fills `CompressBinStream`'s codec table at `0x2cf8a40`
   (`LZO1X_1`, `LZO1X_999`, `LZO2A`, `LZX`). Without it the decompressor pointer is 0.
2. **Tables**, in the engine's order (`BigFile::ReadAllFats` / `BigFileFat::ReadAllFats`):
   `BigFile::HeaderFile::Read`, `BigFileFat::ReadHeader`, `File::Seek`,
   `BigFileFatDescription::Read` per table, `FatFile::Read` per entry.
3. **File data**: for the smallest and the largest compressed entries up to 2 MB,
   `BigFile::ReadFile` (seeks past the 440-byte `FILEDATA` header, allocates through the
   engine's memory manager, reads the data), then `CompressBinStream::DecompressBuffer`
   once per section (see below).

93 functions are translated (57 reached through vtables, 2 left as stubs because they
use SSE and are never called here); imports `read`, `lseek`, `memcpy`, `memmove`, `free`.

The engine reaches its global memory manager through pointer slot `0x278f944`
(`*(*(*slot))`). The test installs a native one there (host functions in
`kHostFnBase`): vtable `+0x8` Alloc, `+0xc` AllocAligned, `+0x18` Free; every other slot
traps with its caller and arguments.

## Checks

- **Recompiled vs original**: the scenario runs twice in separate guest memories —
  recompiled, and the original x86 in Unicorn with the same native functions. Return
  values, the traces of `read`/`lseek`/allocations, every byte of the test's objects
  and the whole heap (file data and decompressed output) must be identical.
- **Against the raw archive**: header magic and version; every entry lies inside the
  file, ids are unique, each starts with `FILEDATA`; `ReadFile` returns exactly the
  stored bytes; each section's decompressed output equals an independent `liblzo2`
  decode of the raw bytes, and `DecompressBuffer` consumes all of its input.

Result on AC2 1.0.1 `DataMAC.forge`: 96 calls, 284 traced native calls, 4 sections and
3,210,611 decompressed bytes, all identical; about 1 second for both runs.

## Formats learned from the code

| Structure | Fields (offset: type) |
|---|---|
| `HeaderFile` (at `BigFile+0x8`) | 0: char[9] magic, 0xc: u32 version, 0x10: u64 table-header offset, 0x18: u32, 0x1c: u8 (version > 23) |
| `HeaderFat` | 8 × u32 (0x0 file count, 0x18 max files per table, 0x1c table count), 0x20: u64 first description offset |
| `BigFileFatDescription` (0x30 bytes) | 0x0: u32 entry count, 0x4: u32, 0x8: u64 entry table offset, 0x10/0x18: u64, 0x20: u64 next (-1 = none), 0x28/0x2c: u32 |
| `FatFile` (entry) | 0x0: u64 record offset, 0x8: u32 id, 0xc: u32 data size (version > 24) |
| `FILEDATA` record | 440 bytes (`0x1b8`): u64 magic `FILEDATA`, char[128] name, 255 bytes, then u32/u64 fields (`FatFileDataHeader::Read`); the file's data follows |
| File data | u32 (0 in AC2), then sections |
| Section | u64 marker `33 aa fb 57 99 fa 04 10`, u16 version (1), u8 codec (0 = `LZO1X_1`), u16 (0x8000), u16 (0x8000), u16 chunk count, chunk count × (u16 decompressed, u16 stored), then per chunk u32 checksum + stored bytes; a chunk is LZO-compressed when stored < decompressed |

`DecompressBuffer` takes `[u32 decompressed size][one section]` and allocates the output.
73 of the 75 entries are compressed, each as two sections: a 16-byte descriptor (count,
the entry's own id, a type, …) and the data. How the shipped game consumes data whose
leading word is 0 (it calls `DecompressBuffer` with it from `CreateBigFileBinStream`
and `BigFileBinStream::SwitchStream`, which would yield nothing) was not worked out (the
recompiled game loads its data, so it no longer matters); the test
passes each section with its size, as `DecompressBuffer` expects.

`BigFile` holds its `scimitar::File` at `+0x22c` and its table object at `+0x28` (whose
`+0x40` is the entries array); a `File` is a file descriptor at offset 0.
