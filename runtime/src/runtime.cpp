#include "scimitar/runtime.h"

#if defined(__APPLE__)
#include <mach/mach_time.h>
#else
#include <time.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace scimitar {

namespace {

// Translated functions register at static initialisation (before any guest
// thread runs); the table is sorted once, on the first lookup, and only read
// afterwards. Host functions may be registered at any time from any thread,
// so they live in a separate fixed table indexed by address.
std::vector<FuncEntry>& registry() {
    static std::vector<FuncEntry> r;
    return r;
}
std::once_flag g_sorted;
std::vector<const FuncEntry*> g_lookup;  // address hash -> entry, built with the sort
uint32_t g_lookup_bits = 0;
inline uint32_t lookup_hash(uint32_t addr) { return (addr * 0x9E3779B1u) >> (32 - g_lookup_bits); }

constexpr uint32_t kHostSlots = kHostFnSpan / kHostFnStride;
FuncEntry g_host[kHostSlots];
std::atomic<uint32_t> g_host_count{0};
std::atomic<uint32_t> g_host_published{0};  // entries [0, published) are complete
std::mutex g_host_lock;

}  // namespace

void register_functions(const FuncEntry* entries, size_t count) {
    registry().insert(registry().end(), entries, entries + count);
}

const FuncEntry* find_function(uint32_t addr) {
    if (addr >= kHostFnBase && addr < kHostFnBase + kHostFnSpan) {
        const uint32_t k = (addr - kHostFnBase) / kHostFnStride;
        if ((addr - kHostFnBase) % kHostFnStride || k >= g_host_published.load(std::memory_order_acquire))
            return nullptr;
        return &g_host[k];
    }
    auto& r = registry();
    std::call_once(g_sorted, [&] {
        std::sort(r.begin(), r.end(),
                  [](const FuncEntry& a, const FuncEntry& b) { return a.addr < b.addr; });
        // Every virtual and indirect call looks its target up here: an
        // open-addressing table (at most half full) makes that one or two probes.
        uint32_t bits = 10;
        while ((1u << bits) < 2 * r.size()) bits++;
        g_lookup_bits = bits;
        g_lookup.assign(1u << bits, nullptr);
        for (const FuncEntry& e : r) {
            uint32_t k = lookup_hash(e.addr);
            while (g_lookup[k]) k = (k + 1) & ((1u << bits) - 1);
            g_lookup[k] = &e;
        }
    });
    const uint32_t mask = (1u << g_lookup_bits) - 1;
    for (uint32_t k = lookup_hash(addr);; k = (k + 1) & mask) {
        const FuncEntry* e = g_lookup[k];
        if (!e || e->addr == addr) return e;
    }
}

const FuncEntry* find_function_containing(uint32_t addr) {
    if (addr >= kHostFnBase && addr < kHostFnBase + kHostFnSpan)
        return find_function(addr - (addr - kHostFnBase) % kHostFnStride);
    find_function(0);  // sorts the table once
    auto& r = registry();
    auto it = std::upper_bound(r.begin(), r.end(), addr,
                               [](uint32_t a, const FuncEntry& e) { return a < e.addr; });
    return it == r.begin() ? nullptr : &*(it - 1);
}

namespace {
std::vector<ImportEntry>& import_registry() {
    static std::vector<ImportEntry> r;
    return r;
}
}  // namespace

void register_imports(const ImportEntry* entries, size_t count) {
    import_registry().insert(import_registry().end(), entries, entries + count);
}

const ImportEntry* find_import(const char* name) {
    for (const ImportEntry& e : import_registry())
        if (std::strcmp(e.name, name) == 0) return &e;
    return nullptr;
}

namespace {
thread_local Context* t_context = nullptr;
}
void set_thread_context(Context* c) { t_context = c; }
Context* thread_context() { return t_context; }

uint32_t register_host_function(GuestFn fn, const char* name) {
    std::lock_guard<std::mutex> l(g_host_lock);
    const uint32_t k = g_host_count.load();
    if (k >= kHostSlots) fatal("too many host functions");
    const uint32_t addr = kHostFnBase + k * kHostFnStride;
    g_host[k] = FuncEntry{addr, fn, name};
    g_host_count.store(k + 1);
    g_host_published.store(k + 1, std::memory_order_release);
    return addr;
}

uint32_t host_function_count() { return g_host_count; }

namespace {

uint32_t call_with(Context& c, uint32_t fn, const uint32_t* args, size_t n) {
    constexpr uint32_t kReturnToHost = 0xfffffff0;  // never executed: the callee returns to us
    const FuncEntry* f = find_function(fn);
    if (!f) fatal("call_guest: %08x is not a recompiled function", fn);
    const uint32_t esp = c.esp;
    c.esp -= 4 * (uint32_t)n;
    for (size_t k = 0; k < n; k++) c.mem->write<uint32_t>(c.esp + 4 * (uint32_t)k, args[k]);
    c.esp -= 4;
    c.mem->write<uint32_t>(c.esp, kReturnToHost);
    f->fn(c);
    c.esp = esp;  // cdecl: the caller pops the arguments (also covers callee-pops)
    return c.eax;
}

}  // namespace

uint32_t call_guest(Context& c, uint32_t fn, std::initializer_list<uint32_t> args) {
    return call_with(c, fn, args.begin(), args.size());
}

uint32_t call_virtual(Context& c, uint32_t object, int slot, std::initializer_list<uint32_t> args) {
    const uint32_t vtable = c.mem->read<uint32_t>(object);
    const uint32_t fn = c.mem->read<uint32_t>(vtable + 4 * (uint32_t)slot);
    std::vector<uint32_t> all{object};
    all.insert(all.end(), args.begin(), args.end());
    return call_with(c, fn, all.data(), all.size());
}

void call_indirect(Context& c, uint32_t target) {
    const FuncEntry* f = find_function(target);
    if (!f) fatal("indirect call to %08x: not a recompiled function", target);
    f->fn(c);
}

namespace {
FatalHook g_fatal_hook = nullptr;
}

uint64_t rdtsc() {
#if defined(__APPLE__)
    static mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t t;
        mach_timebase_info(&t);
        return t;
    }();
    return mach_absolute_time() * tb.numer / tb.denom;
#else
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
#endif
}

void cpuid(Context& c) {
    // A Core 2 class CPU: the game needs no more than SSE3 (it uses fisttp
    // and haddpd but no SSSE3/SSE4).
    switch (c.eax) {
    case 0: {
        c.eax = 1;
        const char v[] = "GenuineIntel";
        std::memcpy(&c.ebx, v, 4), std::memcpy(&c.edx, v + 4, 4), std::memcpy(&c.ecx, v + 8, 4);
        return;
    }
    case 1:
        c.eax = 0x000006fb;  // family 6, model 15, stepping 11
        c.ebx = 0x00020800;  // 2 logical processors, CLFLUSH line 64 bytes
        c.ecx = 0x00000001;  // SSE3
        c.edx = 0x0789fbff;  // FPU..CMOV, MMX, FXSR, SSE, SSE2, TSC, CX8
        return;
    default:
        c.eax = c.ebx = c.ecx = c.edx = 0;
        return;
    }
}

void set_fatal_hook(FatalHook hook) { g_fatal_hook = hook; }

void print_guest_backtrace(Context& c) {
    // [esp] is the return address when called from a shim; then the ebp chain:
    // [ebp] is the caller's ebp, [ebp + 4] the return address.
    auto name = [](uint32_t a) {
        const FuncEntry* f = a >= kHostFnBase ? nullptr : find_function_containing(a);
        return a >= kHostFnBase ? "(host code)" : f ? f->name : "?";
    };
    uint32_t top;
    std::memcpy(&top, c.mem->ptr(c.esp), 4);
    std::fprintf(stderr, "guest backtrace (esp %08x, ebp %08x, [esp] %08x %s):\n", c.esp, c.ebp, top, name(top));
    uint32_t ebp = c.ebp;
    for (int k = 0; k < 40 && ebp && ebp < 0xfffffff0u; k++) {
        uint32_t ret, next;
        std::memcpy(&ret, c.mem->ptr(ebp + 4), 4);
        std::memcpy(&next, c.mem->ptr(ebp), 4);
        std::fprintf(stderr, "  #%-2d %08x %s\n", k, ret, name(ret));
        if (next <= ebp) break;
        ebp = next;
    }
}

void fatal(const char* fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (g_fatal_hook) g_fatal_hook(msg);
    std::fprintf(stderr, "scimitar runtime: %s\n", msg);
    if (Context* c = t_context) {
        print_guest_backtrace(*c);
        std::fprintf(stderr, "registers: eax %08x ebx %08x ecx %08x edx %08x esi %08x edi %08x ebp %08x esp %08x\n",
                     c->eax, c->ebx, c->ecx, c->edx, c->esi, c->edi, c->ebp, c->esp);
        // SCIMITAR_FATAL_DUMP="ADDR[:WORDS],...": guest memory to show (hex addresses).
        if (const char* d = std::getenv("SCIMITAR_FATAL_DUMP")) {
            while (*d) {
                char* end = nullptr;
                const uint32_t a = (uint32_t)std::strtoul(d, &end, 16);
                uint32_t n = 8;
                if (*end == ':') n = (uint32_t)std::strtoul(end + 1, &end, 0);
                std::fprintf(stderr, "%08x:", a);
                for (uint32_t k = 0; k < n; k++) std::fprintf(stderr, " %08x", c->mem->read<uint32_t>(a + 4 * k));
                std::fprintf(stderr, "\n");
                d = *end == ',' ? end + 1 : end;
                if (end == d && *d) break;
            }
        }
    }
    std::abort();
}

}  // namespace scimitar
