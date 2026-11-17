// The ARB program store (scimitar/arb_store.h).
#include "scimitar/arb_store.h"

#include <unistd.h>
#if !defined(__vita__)
#include <sys/mman.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include "scimitar/guest_layout.h"
#include "scimitar/runtime.h"

namespace scimitar::arb_store {

namespace {

// The reference that replaces a program's text after its first line.
constexpr char kTag[] = "\n#scimitar-arb-store ";
constexpr uint32_t kTagLen = sizeof kTag - 1;
constexpr uint32_t kHeader = 10;              // "!!ARBvp1.0" / "!!ARBfp1.0"
constexpr uint32_t kStub = kHeader + kTagLen + 16 + 1;  // ... 16 hex digits, '\n'

struct Store {
    std::mutex m;
    FILE* f = nullptr;
    uint64_t end = 0;
    struct Entry {
        uint64_t offset;
        uint32_t len;
    };
    std::unordered_map<uint64_t, Entry> index;
    Stats stats;
};

Store& store() {
    static Store s;
    return s;
}

const char* path() {
    static const std::string p = [] {
        if (const char* e = std::getenv("SCIMITAR_ARB_STORE")) return std::string(e);
        if (guest_layout() != GuestLayout::Window) return std::string();
#if defined(__vita__)
        return std::string("ux0:data/ac2/cache/arb_store.bin");
#else
        return "/tmp/scimitar-arb-store-" + std::to_string(getpid()) + ".bin";
#endif
    }();
    return p.empty() ? nullptr : p.c_str();
}

uint64_t fnv64(const uint8_t* p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

// Zeroes guest [a, a+n) and gives the whole host pages in it back to the host.
uint64_t release(GuestMemory& m, uint32_t a, uint32_t n) {
    std::memset(m.ptr(a), 0, n);
    const uint32_t lo = (a + 4095) & ~4095u, hi = (a + n) & ~4095u;
#if !defined(__vita__)
    const uintptr_t page = (uintptr_t)sysconf(_SC_PAGESIZE);
    const uintptr_t hlo = ((uintptr_t)m.ptr(a) + page - 1) & ~(page - 1), hhi = (uintptr_t)m.ptr(a + n) & ~(page - 1);
    if (hhi > hlo)
        mmap((void*)hlo, hhi - hlo, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
#endif
    return hi > lo ? hi - lo : 0;
}

ShrinkFn g_shrink = nullptr;

void finish(Context& c, uint32_t a, uint32_t len) {
    if (len <= kStub + 16) return;
    GuestMemory& m = *c.mem;
    const uint8_t* p = m.ptr(a);
    const uint64_t h = fnv64(p, len);
    Store& s = store();
    std::unique_lock<std::mutex> l(s.m);
    if (!s.f) {
        s.f = std::fopen(path(), "w+b");
        if (!s.f) fatal("arb store: cannot create %s", path());
    }
    if (!s.index.count(h)) {
        if (std::fseek(s.f, (long)s.end, SEEK_SET) != 0 || std::fwrite(p, 1, len, s.f) != len)
            fatal("arb store: cannot write %s", path());
        s.index[h] = {s.end, len};
        s.end += len;
    }
    char stub[kStub + 1];
    std::snprintf(stub, sizeof stub, "%.10s%s%016llx\n", (const char*)p, kTag, (unsigned long long)h);
    std::memcpy(m.ptr(a), stub, kStub);
    s.stats.programs++, s.stats.bytes += len;
    l.unlock();
    // The shrunk block ends with the reference's '\n': nothing past it is read.
    const bool shrunk = g_shrink && g_shrink(c, a, kStub);
    l.lock();
    if (shrunk) s.stats.shrunk += len - kStub;
    else s.stats.released += release(m, a + kStub, len - kStub);
}

// A program being copied in pieces on this thread.
thread_local uint32_t t_start = 0, t_next = 0;

}  // namespace

bool enabled() { return path() != nullptr; }

void set_shrink(ShrinkFn fn) { g_shrink = fn; }

void on_copy(Context& c, uint32_t dst, uint32_t n) {
    if (!n || !enabled()) return;
    GuestMemory& m = *c.mem;
    // Other copies come between the pieces (the stream refilling its buffer).
    // A copy of a program already moved out is a reference already.
    if (n >= kHeader && std::memcmp(m.ptr(dst), "!!ARB", 5) == 0)
        t_start = n >= kStub && std::memcmp(m.ptr(dst + kHeader), kTag, kTagLen) == 0 ? 0 : dst, t_next = dst;
    if (!t_start || dst != t_next) return;
    t_next += n;
    if (m.read<uint8_t>(t_next - 1) == 0) {  // the terminating zero: the whole text is in
        finish(c, t_start, t_next - t_start);
        t_start = 0;
    } else if (t_next - t_start > (1u << 20)) {
        t_start = 0;  // not a program after all
    }
}

std::string text(GuestMemory& m, uint32_t ptr, uint32_t len) {
    const char* p = (const char*)m.ptr(ptr);
    if (len < kStub || std::memcmp(p + kHeader, kTag, kTagLen) != 0) return std::string(p, len);
    const uint64_t h = std::strtoull(p + kHeader + kTagLen, nullptr, 16);
    Store& s = store();
    std::lock_guard<std::mutex> l(s.m);
    auto it = s.index.find(h);
    if (it == s.index.end()) fatal("arb store: no program %016llx", (unsigned long long)h);
    std::string t(it->second.len, '\0');
    if (std::fseek(s.f, (long)it->second.offset, SEEK_SET) != 0 || std::fread(t.data(), 1, t.size(), s.f) != t.size())
        fatal("arb store: cannot read %s", path());
    // The game passes its array's length (the stored text's) or strlen's, which
    // now ends wherever the bytes after the reference do: the whole text then,
    // up to its terminating zero.
    const size_t zero = t.find('\0');
    if (zero != std::string::npos) t.resize(zero);
    return t;
}

Stats stats() {
    Store& s = store();
    std::lock_guard<std::mutex> l(s.m);
    return s.stats;
}

}  // namespace scimitar::arb_store
