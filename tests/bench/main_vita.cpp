// scimitar-bench on the PS Vita: the files come with the app (app0:), the
// report goes to ux0:data/scimitar-bench/results.txt, and the app exits when
// done. The ARM cores run at 444 MHz, the most a game may ask for.
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>

#include <cstdio>

#include "bench.h"

#ifdef BENCH_SCREEN
extern "C" {
#include "debugScreen.h"
}
#endif

// Native memory for the probes and scratch (newlib's heap), and the main
// thread's stack.
extern "C" {
int _newlib_heap_size_user = 96 * 1024 * 1024;
unsigned int sceUserMainThreadStackSize = 4 * 1024 * 1024;
}

namespace {
constexpr const char* kDir = "ux0:data/scimitar-bench";
}

// With what memory is left for the app (after newlib's 96 MB heap).
const char* bench_host_name() {
    static char name[192];
    SceKernelFreeMemorySizeInfo m{};
    m.size = sizeof m;
    sceKernelGetFreeMemorySize(&m);
    std::snprintf(name, sizeof name, "PS Vita, ARM %d MHz, bus %d MHz; free: main %d MB, CDRAM %d MB, phycont %d MB",
                  scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency(), m.size_user >> 20,
                  m.size_cdram >> 20, m.size_phycont >> 20);
    return name;
}

// Games get cores 0-2.
int bench_usable_cores() { return 3; }

void bench_thread_setup(int index) {
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0 << index);
}

// A progress file, so a run can be watched over FTP, and the same lines on
// the screen.
void bench_progress(const char* line) {
    sceClibPrintf("%s", line);
#ifdef BENCH_SCREEN
    psvDebugScreenPuts(line);
#endif
    char path[96];
    std::snprintf(path, sizeof path, "%s/progress.txt", kDir);
    if (FILE* f = std::fopen(path, "a")) {
        std::fputs(line, f);
        std::fclose(f);
    }
}

int main() {
#ifdef BENCH_SCREEN
    psvDebugScreenInit();
    psvDebugScreenPuts("scimitar bench: about two minutes; the app closes itself when done.\n\n");
#endif
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    sceIoMkdir(kDir, 0777);
    char path[96];
    std::snprintf(path, sizeof path, "%s/progress.txt", kDir);
    std::remove(path);
    std::snprintf(path, sizeof path, "%s/results.txt", kDir);
    std::remove(path);
    bench_main("app0:kernels.exe", "app0:140.lzo", path);
#ifdef BENCH_SCREEN
    psvDebugScreenPuts("\nDone. Closing in 20 seconds.\n");
    sceKernelDelayThread(20 * 1000 * 1000);
#endif
    sceKernelExitProcess(0);
    return 0;
}
