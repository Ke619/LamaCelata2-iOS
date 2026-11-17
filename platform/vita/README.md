# PS Vita

The Vita build (VitaSDK, `-mcpu=cortex-a9 -mthumb`) of the runtime, the bench
(tests/bench) as a homebrew app, and a load test of the whole translated game
(`ac2-loadtest`). Findings and numbers are in the PS
Vita section of [docs/NEW_PLATFORMS.md](../../docs/NEW_PLATFORMS.md).

What is Vita-specific in the runtime:
- **Guest memory** uses the window layout (`scimitar/guest_layout.h`): guest
  addresses from 0 up to the window's size (512 MB unless set), with the heap
  from 0x08000000. Built with `SCIMITAR_VITA_KUBRIDGE`, the window is address
  space reserved with kubridge's `kuKernelMemReserve` and pages are committed
  as used; without it, the whole window is one ordinary memory block (the
  bench, whose window is 40 MB).
- **x87**: `x87.cpp`'s 128-bit significands use a two-word type where the
  compiler has no `__int128` (tested on the Mac with `-DSCIMITAR_PORTABLE_U128`).
- **SSE**: the scalar fallbacks of `sse.h` (VFP is IEEE like SSE; ARMv7 NEON
  flushes denormals, so it is not used for SSE lanes).

## Build

The translator runs on the Mac: the Vita build takes the translated sources
from a Mac build.

```bash
cmake --build build --target bench_sources
cmake -S . -B build-vita -DCMAKE_TOOLCHAIN_FILE=/opt/vitasdk/share/vita.toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-vita -j 10
```

Output: `build-vita/scimitar-bench.vpk` (title ID `SCIM00001`) and `eboot.bin`.

The translated game: configure with `-DSCIMITAR_VITA_GAME=ON` (the Mac build's
`gen/full`, from `SCIMITAR_HOST_BUILD`) and build `ac2-loadtest.vpk-vpk` (title
`SCIM00002`): every translated function linked into an eboot that only starts
and writes `ux0:data/scimitar-bench/loadtest.txt`. Compiling the 64 parts takes
over an hour (`-j 6`; about 1 GB of memory per job). GCC warns about five
loops whose trip count it bounds by pointer overflow; guest loops never come
near those counts.

## Setting up the Vita (once)

For a PS Vita with HENkaku and Ensō. This lets the Mac copy builds to the
Vita, start them and read their output over Wi-Fi, with nobody at the Vita.

1. **VitaShell** (most HENkaku setups have it; otherwise
   [VitaShell releases](https://github.com/TheOfficialFloW/VitaShell/releases)).
Plugins go in the folder of the taiHEN config that is in use: `ux0:tai/` when
`ux0:tai/config.txt` exists, otherwise `ur0:tai/` (the usual Ensō setup, where
`ux0:tai` doesn't exist). Use the same folder in the config lines below. Copy files
with VitaShell's FTP server (SELECT), e.g.
`curl -T FILE ftp://VITA_IP:1337/ur0:/tai/FILE`.

2. **vitacompanion** (1.08 or later), a plugin that keeps an FTP server (port
   1337) and a command server (port 1338) running whenever the Vita is on.
   From [vitacompanion releases](https://github.com/devnoname120/vitacompanion/releases),
   copy both `vitacompanion.suprx` and `vitacompanion_kernel.skprx`. The user
   module needs the kernel module, and taiHEN loads them separately.
3. **kubridge** (needed later, for the game, not for the bench; do it now to
   save a reboot): copy `kubridge.skprx` from
   [kubridge releases](https://github.com/bythos14/kubridge/releases).
   Then add to the end of the config (VitaShell edits it as text with X;
   triangle → Save):

   ```
   *KERNEL
   ur0:tai/vitacompanion_kernel.skprx
   ur0:tai/kubridge.skprx
   *main
   ur0:tai/vitacompanion.suprx
   ```
4. Reboot the Vita (hold the power button → Restart).
5. **Keep it reachable:** Settings → System → Power Save Settings: turn off
   automatic standby (or keep it on the charger with standby off), and leave
   Wi-Fi on. Note the Vita's IP address (Settings → Network → Wi-Fi Settings →
   the connected network; VitaShell also shows it when its FTP server is on).
   A fixed DHCP lease in the router keeps it the same.
6. **Install the bench once:** from the Mac,
   `curl -T build-vita/scimitar-bench.vpk ftp://VITA_IP:1337/ux0:/data/`, then on the
   Vita in VitaShell open `ux0:data/scimitar-bench.vpk` (X) and install it. A
   "scimitar bench" bubble appears.

From then on `tools/vita_bench.sh VITA_IP` copies the current `eboot.bin`,
starts the app, prints its progress and exits when the report is complete.
By hand: start the "scimitar bench" bubble; the screen stays black for about two
minutes and the app closes itself. The report is
`ux0:data/scimitar-bench/results.txt`.

Useful vitacompanion commands: `echo "quit all" | nc VITA_IP 1338` (stops the running
app), `echo launch SCIM00001 | nc VITA_IP 1338`, `echo reboot | nc VITA_IP 1338`.
