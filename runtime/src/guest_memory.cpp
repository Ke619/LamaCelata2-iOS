#include "scimitar/guest_memory.h"
#include "scimitar/guest_layout.h"

#if defined(__vita__)
#include <psp2/kernel/sysmem.h>
#if defined(SCIMITAR_VITA_KUBRIDGE)
#include <kubridge.h>
#endif
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace scimitar {

static void fail(const char* what) {
    std::fprintf(stderr, "GuestMemory: %s failed: %s\n", what, std::strerror(errno));
    std::abort();
}

namespace {
uint64_t g_window = 512ull << 20;
}

// Whole 4 MB, the steps the heap commits in.
void set_guest_window_size(uint64_t bytes) { g_window = (bytes + 0x3fffff) & ~0x3fffffull; }

GuestLayout guest_layout() {
    static const GuestLayout l = [] {
        if (const char* e = std::getenv("SCIMITAR_GUEST_WINDOW_MB")) {
            set_guest_window_size(std::strtoull(e, nullptr, 0) << 20);
            return GuestLayout::Window;
        }
        if (const char* e = std::getenv("SCIMITAR_COMPACT_GUEST"))
            return std::strcmp(e, "0") != 0 ? GuestLayout::Compact : GuestLayout::Full;
#if defined(__vita__)
        return GuestLayout::Window;
#elif defined(__APPLE__) && TARGET_OS_IPHONE
        return GuestLayout::Compact;
#else
        return GuestLayout::Full;
#endif
    }();
    return l;
}

bool GuestMemory::compact() { return guest_layout() != GuestLayout::Full; }

uint64_t GuestMemory::size() {
    switch (guest_layout()) {
    case GuestLayout::Full: return 1ull << 32;
    case GuestLayout::Compact: return 1ull << 31;
    default: return g_window;
    }
}

#if defined(__vita__)

namespace {
std::vector<uint8_t> g_pages;  // 1 per committed 4 KB page of the window
SceUID g_block = -1;
}  // namespace

// The Vita has no mmap. With kubridge (a kernel plugin) the window is reserved
// address space and pages are committed as asked; without it the whole window
// is one ordinary memory block, all of it backed from the start.
GuestMemory::GuestMemory() {
    g_pages.assign(size() >> 12, 0);
    void* p = nullptr;
#if defined(SCIMITAR_VITA_KUBRIDGE)
    g_block = kuKernelMemReserve(&p, (SceSize)size(), SCE_KERNEL_MEMBLOCK_TYPE_USER_RW);
    if (g_block < 0) {
        std::fprintf(stderr, "GuestMemory: kuKernelMemReserve(%u MB) failed: %08x\n",
                     (unsigned)(size() >> 20), (unsigned)g_block);
        std::abort();
    }
#else
    g_block = sceKernelAllocMemBlock("guest", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, (SceSize)size(), nullptr);
    if (g_block < 0 || sceKernelGetMemBlockBase(g_block, &p) < 0) {
        std::fprintf(stderr, "GuestMemory: allocating %u MB failed: %08x\n",
                     (unsigned)(size() >> 20), (unsigned)g_block);
        std::abort();
    }
#endif
    base_ = static_cast<uint8_t*>(p);
    std::fprintf(stderr, "guest: window layout, %u MB at %p\n", (unsigned)(size() >> 20), p);
}

GuestMemory::~GuestMemory() {
    if (g_block >= 0) sceKernelFreeMemBlock(g_block);
}

void GuestMemory::commit(uint32_t addr, uint32_t len) {
    const uint64_t start = addr & ~0xfffull;
    const uint64_t end = (static_cast<uint64_t>(addr) + len + 0xfff) & ~0xfffull;
    if (end > size()) {
        std::fprintf(stderr, "GuestMemory: commit %08x+%x is outside the %u MB window\n", addr, len,
                     (unsigned)(size() >> 20));
        std::abort();
    }
    for (uint64_t a = start; a < end;) {  // each run of pages not committed yet
        if (g_pages[a >> 12]) { a += 0x1000; continue; }
        uint64_t b = a;
        while (b < end && !g_pages[b >> 12]) g_pages[b >> 12] = 1, b += 0x1000;
#if defined(SCIMITAR_VITA_KUBRIDGE)
        const int r = kuKernelMemCommit(base_ + a, (SceSize)(b - a),
                                        KU_KERNEL_PROT_READ | KU_KERNEL_PROT_WRITE, nullptr);
        if (r < 0) {
            std::fprintf(stderr, "GuestMemory: kuKernelMemCommit(%08x, %x) failed: %08x\n",
                         (unsigned)a, (unsigned)(b - a), (unsigned)r);
            std::abort();
        }
#endif
        std::memset(base_ + a, 0, b - a);
        a = b;
    }
    committed_.emplace_back(static_cast<uint32_t>(start), end - start);
}

#else

GuestMemory::GuestMemory() {
    void* p = mmap(nullptr, size(), PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) fail("reserving guest memory");
    base_ = static_cast<uint8_t*>(p);
    if (guest_layout() == GuestLayout::Compact) std::fprintf(stderr, "guest: compact layout, 2 GB reserved\n");
    if (guest_layout() == GuestLayout::Window)
        std::fprintf(stderr, "guest: window layout, %u MB reserved\n", (unsigned)(size() >> 20));
}

GuestMemory::~GuestMemory() {
    if (base_) munmap(base_, size());
}

void GuestMemory::commit(uint32_t addr, uint32_t len) {
    const uint64_t page = static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
    const uint64_t start = addr & ~(page - 1);
    const uint64_t end = (static_cast<uint64_t>(addr) + len + page - 1) & ~(page - 1);
    if (end > size()) fail("commit range check");
    if (mprotect(base_ + start, end - start, PROT_READ | PROT_WRITE) != 0) fail("mprotect");
    committed_.emplace_back(static_cast<uint32_t>(start), end - start);
}

#endif

}  // namespace scimitar
