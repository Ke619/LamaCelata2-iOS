#pragma once
// The guest's malloc heap: blocks in guest memory [base(), base() + size()),
// committed as the heap grows. Thread-safe. Every block is 16-byte aligned and
// preceded by a 16-byte header holding its size.
#include <cstdint>

#include "scimitar/guest_layout.h"
#include "scimitar/guest_memory.h"

namespace scimitar {
struct Context;
}

namespace scimitar::heap {

// 0x80000000 (1.5 GB); with the compact layout (GuestMemory::compact)
// 0x08000000, above the game's image (AC2's ends at 0x058ec000, Brotherhood's
// at 0x06aee000) and below the imported data (0x7f000000), all within the low
// 2 GB; with the window layout the same base, and the heap fills the window.
inline uint32_t base() { return GuestMemory::compact() ? 0x08000000u : 0x80000000u; }
inline uint32_t size() {
    return guest_layout() == GuestLayout::Window ? (uint32_t)(GuestMemory::size() - base()) : 0x60000000u;
}

uint32_t alloc(GuestMemory& m, uint32_t size, uint32_t align = 16);  // 0 when exhausted
void release(GuestMemory& m, uint32_t p);  // ignores 0 and pointers outside the heap
uint32_t size_of(GuestMemory& m, uint32_t p);  // usable size of a live block
bool contains(uint32_t p);

// Bytes in live blocks (headers included), bytes ever carved from the heap
// (the high-water mark; freed blocks are reused, not returned), and the size.
struct Usage {
    uint64_t in_use, carved, size;
};
Usage usage();

// SCIMITAR_HEAP_REPORT=<seconds>: every live block remembers where it came
// from, and that many seconds after the first allocation the live bytes are
// printed by origin to stderr ("heap-report:" lines), with how much of each
// origin's blocks is still all zero. The origin is the guest call stack for
// blocks a guest import allocates while a GuestSite names its caller, and the
// host function that called alloc otherwise.
class GuestSite {
public:
    explicit GuestSite(const Context& c);
    ~GuestSite();
    GuestSite(const GuestSite&) = delete;
    GuestSite& operator=(const GuestSite&) = delete;

private:
    const Context* previous_;
};

}  // namespace scimitar::heap
