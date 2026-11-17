// The guest heap (scimitar/guest_heap.h) and the malloc-family imports.
//
// Small blocks (up to 4 KB) come from per-size free lists (16-byte classes);
// larger ones from a best-fit map of free extents that merges neighbours. New
// memory is carved from the top of the heap and committed in 4 MB steps.
#include "scimitar/guest_heap.h"

#include <cxxabi.h>
#include <dlfcn.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "scimitar/arb_store.h"
#include "scimitar/imports.h"
#include "scimitar/runtime.h"

namespace scimitar {

namespace imports {
void dump_alloc_trace();
}

namespace heap {

namespace {

constexpr uint32_t kHeader = 16, kMagic = 0x5ca1ab1e, kFreeMagic = 0xf4eeb10c;
constexpr uint32_t kSmallMax = 4096, kCommitStep = 4u << 20;

struct State {
    std::mutex mutex;
    uint32_t top = base(), committed = base();
    uint64_t in_use = 0;  // bytes of live blocks, headers included
    std::vector<std::vector<uint32_t>> small = std::vector<std::vector<uint32_t>>(kSmallMax / 16 + 1);
    std::multimap<uint32_t, uint32_t> large_by_size;  // block size -> header address
    std::map<uint32_t, uint32_t> large_by_addr;       // header address -> block size
};

State& state() {
    static State s;
    return s;
}

// Header: [0] size of the block's payload, [4] magic, [8] alignment padding
// before the header (for aligned blocks), [12] unused.
uint32_t round16(uint32_t n) { return (n + 15) & ~15u; }

uint32_t carve(GuestMemory& m, State& s, uint32_t total) {
    if ((uint64_t)s.top + total > (uint64_t)base() + size()) return 0;
    const uint32_t h = s.top;
    s.top += total;
    while (s.committed < s.top) {
        m.commit(s.committed, kCommitStep);
        s.committed += kCommitStep;
    }
    return h;
}

void remove_large(State& s, uint32_t h, uint32_t size) {
    s.large_by_addr.erase(h);
    auto range = s.large_by_size.equal_range(size);
    for (auto it = range.first; it != range.second; ++it)
        if (it->second == h) {
            s.large_by_size.erase(it);
            return;
        }
}

void add_large(GuestMemory& m, State& s, uint32_t h, uint32_t size) {
    // Merge with the free extents right after and right before.
    auto next = s.large_by_addr.find(h + kHeader + size);
    if (next != s.large_by_addr.end()) {
        const uint32_t nsize = next->second;
        remove_large(s, next->first, nsize);
        size += kHeader + nsize;
    }
    auto prev = s.large_by_addr.lower_bound(h);
    if (prev != s.large_by_addr.begin()) {
        --prev;
        if (prev->first + kHeader + prev->second == h) {
            const uint32_t ph = prev->first, psize = prev->second;
            remove_large(s, ph, psize);
            h = ph;
            size += kHeader + psize;
        }
    }
    m.write<uint32_t>(h, size);
    m.write<uint32_t>(h + 4, kFreeMagic);
    s.large_by_addr[h] = size;
    s.large_by_size.emplace(size, h);
}

}  // namespace

bool contains(uint32_t p) { return p >= base() + kHeader && p < base() + size(); }

namespace {
uint64_t g_bucket_bytes[33], g_bucket_count[33];  // by power of two of the size
}

uint32_t alloc_block(GuestMemory& m, uint32_t size, uint32_t align) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    {
        const int b = size ? 32 - __builtin_clz(size) : 0;
        g_bucket_bytes[b] += size, g_bucket_count[b]++;
    }
    if (align < 16 || (align & (align - 1))) align = 16;
    uint32_t want = round16(size ? size : 1);
    if (want < size) return 0;
    const uint32_t slack = align > 16 ? align : 0;  // room to move the payload up
    uint32_t h = 0, got = want + slack;
    if (got <= kSmallMax && !s.small[got / 16].empty()) {
        h = s.small[got / 16].back();
        s.small[got / 16].pop_back();
    } else if (got > kSmallMax) {
        auto it = s.large_by_size.lower_bound(got);
        if (it != s.large_by_size.end()) {
            h = it->second;
            const uint32_t have = it->first;
            remove_large(s, h, have);
            if (have >= got + kHeader + kSmallMax + 16) {  // split off the rest
                add_large(m, s, h + kHeader + got, have - got - kHeader);
            } else {
                got = have;
            }
        }
    }
    if (!h) {
        h = carve(m, s, kHeader + got);
        if (!h) {
            std::fprintf(stderr, "guest heap: out of memory allocating %u bytes (%u MB in use of %u MB)\n",
                         size, (s.top - base()) >> 20, heap::size() >> 20);
            static bool dumped = false;
            if (!dumped) {
                dumped = true;
                imports::dump_alloc_trace();
                for (int b = 0; b <= 32; b++)
                    if (g_bucket_bytes[b] >> 20)
                        std::fprintf(stderr, "  sizes < 2^%d: %llu MB in %llu blocks\n", b,
                                     (unsigned long long)(g_bucket_bytes[b] >> 20),
                                     (unsigned long long)g_bucket_count[b]);
            }
            return 0;
        }
    }
    m.write<uint32_t>(h, got);
    m.write<uint32_t>(h + 4, kMagic);
    m.write<uint32_t>(h + 8, 0);
    s.in_use += kHeader + got;
    uint32_t p = h + kHeader;
    if (align > 16 && (p & (align - 1))) {
        // Aligned payload: a second header right before it points back.
        const uint32_t q = (p + kHeader + align - 1) & ~(align - 1);
        m.write<uint32_t>(q - kHeader, got - (q - p));
        m.write<uint32_t>(q - kHeader + 4, kMagic);
        m.write<uint32_t>(q - kHeader + 8, q - p);  // offset back to the real header
        p = q;
    }
    return p;
}

// ---- SCIMITAR_HEAP_REPORT ----
namespace {

thread_local const Context* t_site = nullptr;

int report_seconds() {
    static const int secs = std::getenv("SCIMITAR_HEAP_REPORT") ? std::atoi(std::getenv("SCIMITAR_HEAP_REPORT")) : 0;
    return secs;
}

// Up to eight guest return addresses, innermost first, or one host address
// with the top bit set.
using Origin = std::array<uint64_t, 8>;

struct Tracker {
    std::mutex mutex;
    std::map<Origin, uint32_t> ids;
    std::vector<Origin> origins;
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> live;  // payload -> size, origin
};

Tracker& tracker() {
    static Tracker t;
    return t;
}

Origin origin_of(GuestMemory& m, const void* host_caller) {
    Origin o{};
    const Context* c = t_site;
    if (!c) {
        o[0] = reinterpret_cast<uintptr_t>(host_caller) | (1ull << 63);
        return o;
    }
    o[0] = m.read<uint32_t>(c->esp);
    // The game keeps frame pointers: walk the ebp chain while it stays on
    // this thread's stack, just above esp.
    uint32_t f = c->ebp;
    for (size_t k = 1; k < o.size(); k++) {
        if (f <= c->esp || f - c->esp > (1u << 20)) break;
        o[k] = m.read<uint32_t>(f + 4);
        const uint32_t next = m.read<uint32_t>(f);
        if (next <= f) break;
        f = next;
    }
    return o;
}

std::string host_name(uint64_t a) {
    Dl_info info{};
    const void* p = reinterpret_cast<const void*>(a & ~(1ull << 63));
    if (!dladdr(p, &info) || !info.dli_sname) return "host?";
    int status = 0;
    char* d = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
    std::string name = status == 0 && d ? d : info.dli_sname;
    std::free(d);
    return "host " + name.substr(0, 90);
}

bool zero_page(const uint8_t* p) {
    const uint64_t* q = reinterpret_cast<const uint64_t*>(p);
    for (int i = 0; i < 512; i++)
        if (q[i]) return false;
    return true;
}

void report(GuestMemory& m) {
    Tracker& t = tracker();
    std::lock_guard<std::mutex> lock(t.mutex);
    struct Sum {
        uint64_t bytes = 0, zero = 0, blocks = 0;
        uint64_t most_used = 0;  // of one block: its bytes outside whole zero pages
    };
    std::vector<Sum> sums(t.origins.size());
    uint64_t total = 0, total_zero = 0;
    for (const auto& [p, e] : t.live) {
        Sum& s = sums[e.second];
        s.bytes += e.first, s.blocks++;
        total += e.first;
        uint64_t zero = 0;
        for (uint32_t a = (p + 4095) & ~4095u; a + 4096 <= p + e.first; a += 4096)
            if (zero_page(m.ptr(a))) zero += 4096;
        s.zero += zero, total_zero += zero;
        s.most_used = std::max<uint64_t>(s.most_used, e.first - zero);
    }
    const Usage u = usage();
    std::fprintf(stderr, "heap-report: %llu MB live in %zu blocks (%llu MB of whole zero pages), %llu MB carved\n",
                 (unsigned long long)(total >> 20), t.live.size(), (unsigned long long)(total_zero >> 20),
                 (unsigned long long)(u.carved >> 20));
    if (arb_store::enabled()) {
        const arb_store::Stats a = arb_store::stats();
        std::fprintf(stderr, "heap-report: ARB program store: %llu programs, %llu MB of text moved out: "
                             "%llu MB given back to the game's allocator, %llu MB of whole pages zeroed\n",
                     (unsigned long long)a.programs, (unsigned long long)(a.bytes >> 20),
                     (unsigned long long)(a.shrunk >> 20), (unsigned long long)(a.released >> 20));
    }
    std::vector<uint32_t> order(sums.size());
    for (uint32_t k = 0; k < order.size(); k++) order[k] = k;
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return sums[a].bytes > sums[b].bytes; });
    for (uint32_t k : order) {
        if (sums[k].bytes < (64u << 10)) break;
        std::string where;
        const Origin& o = t.origins[k];
        if (o[0] >> 63) {
            where = host_name(o[0]);
        } else {
            char buf[16];
            for (uint64_t a : o) {
                if (!a) break;
                std::snprintf(buf, sizeof buf, " %08x", (uint32_t)a);
                where += buf;
            }
        }
        std::fprintf(stderr, "heap-report: %9llu KB %8llu blocks %9llu KB zero %8llu KB most used in one |%s\n",
                     (unsigned long long)(sums[k].bytes >> 10), (unsigned long long)sums[k].blocks,
                     (unsigned long long)(sums[k].zero >> 10), (unsigned long long)(sums[k].most_used >> 10),
                     where.c_str());
    }
}

void record(GuestMemory& m, uint32_t p, uint32_t size, const void* host_caller) {
    const Origin o = origin_of(m, host_caller);
    Tracker& t = tracker();
    std::lock_guard<std::mutex> lock(t.mutex);
    static std::once_flag once;
    std::call_once(once, [&m] {
        std::thread([&m] {
            std::this_thread::sleep_for(std::chrono::seconds(report_seconds()));
            report(m);
        }).detach();
    });
    auto [it, added] = t.ids.emplace(o, (uint32_t)t.origins.size());
    if (added) t.origins.push_back(o);
    t.live[p] = {size, it->second};
}

}  // namespace

GuestSite::GuestSite(const Context& c) : previous_(t_site) { t_site = &c; }
GuestSite::~GuestSite() { t_site = previous_; }

uint32_t alloc(GuestMemory& m, uint32_t size, uint32_t align) {
    const uint32_t p = alloc_block(m, size, align);
    if (p && report_seconds()) record(m, p, size, __builtin_return_address(0));
    return p;
}

Usage usage() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return {s.in_use, (uint64_t)(s.top - base()), (uint64_t)size()};
}

uint32_t size_of(GuestMemory& m, uint32_t p) {
    if (!contains(p) || m.read<uint32_t>(p - kHeader + 4) != kMagic) return 0;
    return m.read<uint32_t>(p - kHeader);
}

void release(GuestMemory& m, uint32_t p) {
    if (!contains(p)) return;
    if (report_seconds()) {
        Tracker& t = tracker();
        std::lock_guard<std::mutex> lock(t.mutex);
        t.live.erase(p);
    }
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    uint32_t h = p - kHeader;
    if (m.read<uint32_t>(h + 4) != kMagic) fatal("free(%08x): not an allocated block", p);
    if (const uint32_t back = m.read<uint32_t>(h + 8)) h = p - back - kHeader;  // aligned block
    const uint32_t size = m.read<uint32_t>(h);
    if (m.read<uint32_t>(h + 4) != kMagic) fatal("free(%08x): corrupt header", p);
    m.write<uint32_t>(h + 4, kFreeMagic);
    s.in_use -= kHeader + size;
    if (size <= kSmallMax) s.small[size / 16].push_back(h);
    else add_large(m, s, h, size);
}

}  // namespace heap

namespace imports {

// With SCIMITAR_TRACE_ALLOCS, bytes are counted per guest caller and the top
// callers printed when the heap runs out.
namespace {
std::mutex g_trace_lock;
std::map<uint32_t, std::pair<uint64_t, uint64_t>> g_by_caller;  // caller -> bytes, count
void count(Context& c, uint32_t n) {
    static const bool on = std::getenv("SCIMITAR_TRACE_ALLOCS") != nullptr;
    if (!on) return;
    std::lock_guard<std::mutex> l(g_trace_lock);
    auto& e = g_by_caller[c.mem->read<uint32_t>(c.esp)];
    e.first += n, e.second++;
}
}  // namespace

void dump_alloc_trace() {
    std::lock_guard<std::mutex> l(g_trace_lock);
    std::vector<std::pair<uint64_t, uint32_t>> v;
    for (const auto& [a, e] : g_by_caller) v.emplace_back(e.first, a);
    std::sort(v.rbegin(), v.rend());
    for (size_t k = 0; k < v.size() && k < 12; k++)
        std::fprintf(stderr, "  %8llu KB in %8llu blocks from %08x\n", (unsigned long long)(v[k].first >> 10),
                     (unsigned long long)g_by_caller[v[k].second].second, v[k].second);
}

void imp_malloc(Context& c) {
    count(c, arg32(c, 0));
    heap::GuestSite site(c);
    shim_return(c, heap::alloc(*c.mem, arg32(c, 0)));
}

void imp_calloc(Context& c) {
    const uint64_t n = (uint64_t)arg32(c, 0) * arg32(c, 1);
    count(c, (uint32_t)n);
    if (n >> 32) return shim_return(c, 0);
    heap::GuestSite site(c);
    const uint32_t p = heap::alloc(*c.mem, (uint32_t)n);
    if (p) std::memset(c.mem->ptr(p), 0, (size_t)n);
    shim_return(c, p);
}

void imp_realloc(Context& c) {
    const uint32_t old = arg32(c, 0), n = arg32(c, 1);
    heap::GuestSite site(c);
    if (!old) return shim_return(c, heap::alloc(*c.mem, n));
    const uint32_t have = heap::size_of(*c.mem, old);
    if (!have) fatal("realloc(%08x): not an allocated block", old);
    if (n <= have) return shim_return(c, old);
    count(c, n);
    const uint32_t p = heap::alloc(*c.mem, n);
    if (p) {
        std::memcpy(c.mem->ptr(p), c.mem->ptr(old), have);
        heap::release(*c.mem, old);
    }
    shim_return(c, p);
}

// void free(void* p). Pointers outside the heap (test harnesses' own
// allocators) are recorded and otherwise ignored.
void imp_free(Context& c) {
    const uint32_t p = arg32(c, 0);
    if (import_trace_enabled()) import_trace().push_back("free(" + std::to_string(p) + ")");
    heap::release(*c.mem, p);
    shim_return(c, 0);
}

void imp_malloc_size(Context& c) { shim_return(c, heap::size_of(*c.mem, arg32(c, 0))); }

}  // namespace imports

namespace {
const ImportShim kHeap[] = {
    {"_malloc", imports::imp_malloc},   {"_calloc", imports::imp_calloc},
    {"_realloc", imports::imp_realloc}, {"_free", imports::imp_free},
    {"_malloc_size", imports::imp_malloc_size},
};
SCIMITAR_REGISTER_SHIMS(kHeap);
}  // namespace

}  // namespace scimitar
