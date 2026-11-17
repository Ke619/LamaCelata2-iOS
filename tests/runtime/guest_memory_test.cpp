#include "scimitar/context.h"
#include "scimitar/guest_layout.h"
#include "scimitar/guest_memory.h"

#include <cstdio>

using scimitar::Context;
using scimitar::GuestMemory;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, \
                         __LINE__, #cond);                              \
            return 1;                                                   \
        }                                                               \
    } while (0)

int main() {
    GuestMemory mem;

    // Low and high guest addresses map into the same reservation.
    mem.commit(0x1000, 0x2000);
    mem.write<uint32_t>(0x1ffc, 0xdeadbeef);
    CHECK(mem.read<uint32_t>(0x1ffc) == 0xdeadbeef);
    CHECK(mem.ptr(0x1ffc) == mem.base() + 0x1ffc);

    if (scimitar::guest_layout() == scimitar::GuestLayout::Full) {
        mem.commit(0xfffff000u, 0x1000);
        mem.write<uint8_t>(0xffffffffu, 0x5a);
        CHECK(mem.read<uint8_t>(0xffffffffu) == 0x5a);
    } else {
        // The last page of a smaller layout; committing twice is harmless.
        const uint32_t last = (uint32_t)(GuestMemory::size() - 0x1000);
        mem.commit(last, 0x1000);
        mem.commit(last, 0x1000);
        mem.write<uint8_t>(last + 0xfff, 0x5a);
        CHECK(mem.read<uint8_t>(last + 0xfff) == 0x5a);
        CHECK(mem.read<uint8_t>(last) == 0);
    }
    if (scimitar::guest_layout() == scimitar::GuestLayout::Window)
        CHECK(GuestMemory::size() == 64u << 20);  // SCIMITAR_GUEST_WINDOW_MB=62, rounded up

    // Unaligned access works (x86 code relies on it).
    mem.write<uint32_t>(0x1001, 0x11223344);
    CHECK(mem.read<uint32_t>(0x1001) == 0x11223344);

    Context ctx;
    ctx.mem = &mem;
    CHECK(ctx.eflags == 0x202 && ctx.mxcsr == 0x1f80);

    std::puts("guest_memory_test: OK");
    return 0;
}
