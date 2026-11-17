// scimitar-bench on the Mac (and other POSIX hosts).
#include <sys/sysctl.h>

#include <cstdio>
#include <string>

#include "bench.h"

const char* bench_host_name() {
    static std::string name = [] {
        char buf[256] = "unknown CPU";
        size_t n = sizeof buf;
        sysctlbyname("machdep.cpu.brand_string", buf, &n, nullptr, 0);
        return std::string(buf);
    }();
    return name.c_str();
}
int bench_usable_cores() { return 4; }
void bench_thread_setup(int) {}
void bench_progress(const char*) {}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s KERNELS.exe LZO_STREAM [OUT.txt]\n", argv[0]);
        return 2;
    }
    return bench_main(argv[1], argv[2], argc > 3 ? argv[3] : nullptr);
}
