# scimitar-bench

How fast translated code runs on a host, measured the same way on every host,
with no game code or data: the number that scales the Mac's per-frame CPU
time to another machine (the PS Vita, see docs/NEW_PLATFORMS.md).

- `kernels.c`: synthetic workloads shaped like an engine frame: vector math
  with small float functions (x87 returns on i386), particles, name hashing,
  a comparator-driven sort, entity updates through function tables over
  scattered objects, a binary tree, text parsing, double-precision n-body.
- With `mingw-w64` installed (optional, Homebrew), the Mac build compiles it twice: as an i386 Windows executable with MinGW's
  GCC (SSE floating point, frame pointers, no vectorizing, like the game's
  compiler), which `scimitar-recomp` translates (`gen/bench_kernels.cpp`), and
  natively. The LZO fixture's translation (`gen/lzo1x_decompress_safe.cpp`)
  runs too, on `tests/lzo/vectors/140.lzo`.
- `bench.cpp` runs each kernel translated (on the runtime, guest memory in the
  window layout) and native, best of three, and requires equal checksums; then
  the whole translated suite on 1-4 threads at once; then a native memory
  latency and copy probe.

```bash
cmake --build build --target scimitar-bench
./build/scimitar-bench build/bench/kernels.exe tests/lzo/vectors/140.lzo
```

`bench_checksums` (ctest) runs it. The PS Vita build is in
[platform/vita/README.md](../../platform/vita/README.md); `tools/vita_bench.sh`
runs it on a Vita over Wi-Fi.
