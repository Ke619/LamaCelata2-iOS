# Working rules for this repository

## Publishing
- The repository is meant to be public. Push, add remotes or publish only when
  asked.
- Nothing personal or machine-specific goes in: no home paths, device IDs, team IDs
  or email addresses. Use placeholders (`<udid>`, `<team>`) in docs.

## Game files never enter the repo
- Never commit, upload or paste game executables, `.forge`/`.pck`/`.bik` data, or
  large disassembly dumps of Ubisoft code. `.gitignore` blocks the usual names.
- The only code bytes allowed in `fixtures/` are small, self-contained functions
  from **open-source libraries the game links statically** (e.g. LZO), extracted
  with `tools/extract_fixture.py`, which enforces a size cap and records provenance.
- Everything under `tests/` must run from fixtures and synthetic data alone, so
  the test suite works without the game installed. Work that needs the real binary
  (`scimitar_sf`) reads it from outside this repo
  (`../steam_depot/33362_5/Assassin's Creed 2.app`).

## Target binaries (reference facts)
- AC2 `scimitar_sf`: i386 Mach-O, SHA-1 `afcbbbf8046c47561b2a5a0574a11703071f15e5`,
  308,449 symbols, `__text` 0x1f3cb40 bytes, 593 imports, fixed load address (not PIE).
  Not the executable Steam ships today (see docs/MAC_RELEASES.md); the data files are.
- ACB `scimitar_sf`: i386 Mach-O, SHA-1 `380d23b7d0bea1e71ed9d65a1ff230e0894bb310`,
  407,423 symbols, `__text` 0x2204358 bytes. Matches Steam's only ACB Mac manifest.
- Compiler: Apple GCC 4.0/4.2 era, frame pointers kept, mostly SSE floating point.
- Windows targets (AC1 `AssassinsCreed_Dx9.exe`, Revelations `ACRSP.exe`, Prince of
  Persia `Prince of Persia.exe`): PE32, MSVC, no symbols. Facts in
  docs/AC1_WINDOWS.md, docs/ACR_WINDOWS.md, docs/POP_WINDOWS.md.

## Conventions
- C++20, CMake. Generated code goes to the build directory, never into git.
- Guest addresses are `uint32_t`; never store host pointers in guest memory.
- Calling convention for guest functions: i386 cdecl (args on the guest stack,
  result in `eax`/`edx`, callee-saved `ebx`, `esi`, `edi`, `ebp`). The Windows
  ports also use MSVC's thiscall, stdcall and fastcall.

## Build and test
```bash
cmake -S . -B build && cmake --build build -j 10 && ctest --test-dir build --output-on-failure
```
The Makefiles generator builds one job at a time without `-j`. The ports need
`-DSCIMITAR_FULL_IMAGE=ON` and the games' locations (README, Building).
