// scimitar-bench: how fast translated code runs on this host (README.md).
// Runs the synthetic kernels of kernels.c translated from i386 (on our
// runtime, as the game's code runs) and compiled natively, plus the LZO
// fixture translated, and a native memory latency and bandwidth probe; then
// the translated kernels on several threads at once. Times are the best of a
// few runs. Every kernel's checksum must match between the two builds.
//
//   scimitar-bench KERNELS.exe LZO_STREAM [OUT.txt]
//
// On the PS Vita the files come from the app's own folder and the report is
// also written to ux0:data/scimitar-bench/results.txt (vita/main.cpp).
#include "bench.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "scimitar/guest_layout.h"
#include "scimitar/image_loader.h"
#include "scimitar/runtime.h"

extern "C" {
uint32_t bench_vecmath(uint8_t*, uint32_t);
uint32_t bench_particles(uint8_t*, uint32_t);
uint32_t bench_hash(uint8_t*, uint32_t);
uint32_t bench_sort(uint8_t*, uint32_t);
uint32_t bench_entities(uint8_t*, uint32_t);
uint32_t bench_tree(uint8_t*, uint32_t);
uint32_t bench_text(uint8_t*, uint32_t);
uint32_t bench_double(uint8_t*, uint32_t);
}
namespace generated {
void lzo1x_decompress_safe(scimitar::Context& c);
}

using namespace scimitar;

namespace {

// Guest memory of the bench: the kernels' image at 0x00400000; per thread 4 MB
// of scratch and 1 MB of stack.
constexpr uint32_t kScratch = 0x01000000, kScratchSize = 0x00400000;
constexpr uint32_t kStacks = 0x02000000, kStackSize = 0x00100000;
constexpr int kMaxThreads = 4;
constexpr uint64_t kWindow = 40ull << 20;

struct Kernel {
    const char* name;
    uint32_t (*native)(uint8_t*, uint32_t);
    uint32_t reps;  // about 50 ms translated on an M1 Pro core
};
const Kernel kKernels[] = {
    {"bench_vecmath", bench_vecmath, 280},    {"bench_particles", bench_particles, 1200},
    {"bench_hash", bench_hash, 30},           {"bench_sort", bench_sort, 54},
    {"bench_entities", bench_entities, 300},  {"bench_tree", bench_tree, 4},
    {"bench_text", bench_text, 560},          {"bench_double", bench_double, 1200},
};
constexpr int kKernelCount = sizeof kKernels / sizeof kKernels[0];
constexpr uint32_t kLzoReps = 125;

std::string g_report;
void out(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::fputs(buf, stdout);
    std::fflush(stdout);
    g_report += buf;
    bench_progress(buf);
}

double now_ms() { return (double)rdtsc() * 1e-6; }

struct Bench {
    GuestMemory& mem;
    uint32_t addr[kKernelCount];
    std::vector<uint8_t> lzo;

    // One guest thread's state: its own stack and scratch.
    Context context(int t) {
        Context c;
        c.mem = &mem;
        c.esp = kStacks + (uint32_t)(t + 1) * kStackSize - 64;
        return c;
    }
    uint32_t translated(int k, int t) {
        Context c = context(t);
        const uint32_t scratch = kScratch + (uint32_t)t * kScratchSize;
        std::memset(mem.ptr(scratch), 0, kScratchSize);
        return call_guest(c, addr[k], {scratch, kKernels[k].reps});
    }
    // LZO: the fixture decompresses a 256 KB stream kLzoReps times.
    uint32_t lzo_run(int t) {
        const uint32_t src = kScratch + (uint32_t)t * kScratchSize, dst = src + 0x100000,
                       len = dst + 0x100000;
        std::memcpy(mem.ptr(src), lzo.data(), lzo.size());
        uint32_t h = 0;
        for (uint32_t r = 0; r < kLzoReps; r++) {
            Context c = context(t);
            mem.write<uint32_t>(len, 0x40000);
            c.esp -= 24;
            const uint32_t frame[6] = {0xfffffff0u, src, (uint32_t)lzo.size(), dst, len, 0};
            std::memcpy(mem.ptr(c.esp), frame, sizeof frame);
            generated::lzo1x_decompress_safe(c);
            const uint8_t* p = mem.ptr(dst);
            uint32_t f = 2166136261u ^ c.eax;
            for (uint32_t i = 0, n = mem.read<uint32_t>(len); i < n; i += 64) f = (f ^ p[i]) * 16777619u;
            h = (h ^ f) * 16777619u;
        }
        return h;
    }
};

template <typename F> double best_ms(int runs, F f) {
    double best = 1e30;
    for (int r = 0; r < runs; r++) {
        const double t0 = now_ms();
        f();
        best = std::min(best, now_ms() - t0);
    }
    return best;
}

// Native: nanoseconds per dependent load over `bytes` (a random cycle), and
// sequential copy bandwidth in MB/s.
double latency_ns(uint32_t bytes) {
    const uint32_t n = bytes / 64;
    std::vector<uint32_t> next(n * 16);
    std::vector<uint32_t> order(n);
    for (uint32_t i = 0; i < n; i++) order[i] = i;
    uint32_t s = 1;
    for (uint32_t i = n - 1; i > 0; i--) {
        s = s * 1664525u + 1013904223u;
        std::swap(order[i], order[s % (i + 1)]);
    }
    for (uint32_t i = 0; i < n; i++) next[order[i] * 16] = order[(i + 1) % n] * 16;
    const uint32_t steps = 2000000;
    volatile uint32_t sink;
    const double ms = best_ms(3, [&] {
        uint32_t at = 0;
        for (uint32_t i = 0; i < steps; i++) at = next[at];
        sink = at;
    });
    (void)sink;
    return ms * 1e6 / steps;
}
double copy_mb_s(uint32_t bytes) {
    std::vector<uint8_t> a(bytes, 1), b(bytes, 2);
    volatile uint8_t sink = 0;
    const double ms = best_ms(3, [&] {
        for (int r = 0; r < 4; r++) {
            uint8_t* d = r & 1 ? a.data() : b.data();
            std::memcpy(d, r & 1 ? b.data() : a.data(), bytes);
            sink = sink + d[r * 4099 % bytes];
        }
    });
    return 4.0 * bytes / (1 << 20) / (ms * 1e-3);
}

}  // namespace

int bench_main(const char* kernels_exe, const char* lzo_path, const char* report_path) {
    set_guest_window_size(kWindow);
    GuestMemory mem;
    LoadedImage image = load_image(mem, kernels_exe);
    mem.commit(kScratch, kScratchSize * kMaxThreads);
    mem.commit(kStacks, kStackSize * kMaxThreads);

    Bench b{mem, {}, {}};
    for (int k = 0; k < kKernelCount; k++) {
        for (const recomp::Export& e : image.image->exports())
            if (e.name == kKernels[k].name) b.addr[k] = e.addr;
        if (!b.addr[k]) fatal("%s exports no %s", kernels_exe, kKernels[k].name);
    }
    {
        std::ifstream in(lzo_path, std::ios::binary);
        b.lzo.assign(std::istreambuf_iterator<char>(in), {});
        if (b.lzo.empty() || b.lzo.size() > 0x100000) fatal("cannot read %s", lzo_path);
    }

    out("scimitar-bench on %s\n", bench_host_name());
    out("%-16s %10s %10s %8s  %s\n", "kernel", "translated", "native", "ratio", "checksum");
    std::vector<uint8_t> native_scratch(kScratchSize);
    double total_tr = 0, total_nat = 0;
    int mismatches = 0;
    for (int k = 0; k < kKernelCount; k++) {
        uint32_t rt = 0, rn = 0;
        const double tr = best_ms(3, [&] { rt = b.translated(k, 0); });
        const double nat = best_ms(3, [&] {
            std::memset(native_scratch.data(), 0, native_scratch.size());
            rn = kKernels[k].native(native_scratch.data(), kKernels[k].reps);
        });
        total_tr += tr, total_nat += nat;
        mismatches += rt != rn;
        out("%-16s %8.1fms %8.1fms %7.2fx  %08x%s\n", kKernels[k].name + 6, tr, nat, tr / nat, rt,
            rt == rn ? "" : " MISMATCH (native differs)");
    }
    uint32_t rl = 0;
    const double lzo_ms = best_ms(3, [&] { rl = b.lzo_run(0); });
    out("%-16s %8.1fms %10s %8s  %08x\n", "lzo (fixture)", lzo_ms, "-", "-", rl);
    out("%-16s %8.1fms %8.1fms %7.2fx\n", "kernels total", total_tr, total_nat, total_tr / total_nat);
    out("score (translated, all, ms): %.1f\n", total_tr + lzo_ms);

    // The translated suite on 1..kMaxThreads threads at once, each with its
    // own scratch: wall time of one round, and throughput against one thread.
    const int threads = std::min(kMaxThreads, bench_usable_cores());
    double one = 0;
    for (int n = 1; n <= threads; n++) {
        std::atomic<int> go{0};
        double ms = best_ms(2, [&] {
            std::vector<std::thread> pool;
            for (int t = 0; t < n; t++)
                pool.emplace_back([&, t] {
                    bench_thread_setup(t);
                    go++;
                    while (go < n) {}
                    for (int k = 0; k < kKernelCount; k++) b.translated(k, t);
                    b.lzo_run(t);
                });
            for (auto& th : pool) th.join();
            go = 0;
        });
        if (n == 1) one = ms;
        out("threads %d: %8.1fms per round, throughput %.2fx one thread\n", n, ms, n * one / ms);
    }

    out("memory: latency %.1f ns (256 KB), %.1f ns (32 MB); copy %.0f MB/s (24 MB)\n",
        latency_ns(256 << 10), latency_ns(32 << 20), copy_mb_s(24 << 20));
    out(mismatches ? "RESULT: %d checksum mismatches\n" : "RESULT: ok\n", mismatches);
    if (report_path) {
        if (FILE* f = std::fopen(report_path, "w")) {
            std::fputs(g_report.c_str(), f);
            std::fclose(f);
        }
    }
    return mismatches ? 1 : 0;
}
