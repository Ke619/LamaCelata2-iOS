// ac2-loadtest: the translated game (ac2_full) linked into an eboot that only
// starts: whether the Vita loads a module that large, how long it takes, and
// how much memory is left. Writes ux0:data/scimitar-bench/loadtest.txt.
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>

#include <cstdio>

#include "scimitar/runtime.h"

extern "C" {
unsigned int sceUserMainThreadStackSize = 1 * 1024 * 1024;
}

int main() {
    sceIoMkdir("ux0:data/scimitar-bench", 0777);
    FILE* f = std::fopen("ux0:data/scimitar-bench/loadtest.txt", "w");
    SceKernelFreeMemorySizeInfo info{};
    info.size = sizeof info;
    sceKernelGetFreeMemorySize(&info);
    // The static initializers registered every translated function; look one up.
    const scimitar::FuncEntry* e = scimitar::find_function_containing(0x00100000);
    if (f) {
        std::fprintf(f, "loaded at %u ms: %s, free main memory %d KB, CDRAM %d KB, phycont %d KB\n",
                     (unsigned)(sceKernelGetProcessTimeWide() / 1000), e ? e->name : "no functions",
                     info.size_user / 1024, info.size_cdram / 1024, info.size_phycont / 1024);
        std::fclose(f);
    }
    sceKernelExitProcess(0);
    return 0;
}
