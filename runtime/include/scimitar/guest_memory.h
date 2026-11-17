#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace scimitar {

// The guest's address space, reserved as one host block. Guest address `a`
// lives at host address `base() + a`. Pages start inaccessible; ranges are
// committed as the loader or allocator needs them.
//
// The block is the whole 32-bit space, or with the compact layout (iOS, or
// SCIMITAR_COMPACT_GUEST=1) its low 2 GB, which holds the image, the imported
// data and the heap moved down (guest_heap.h): an app may then reserve it
// without iOS's extended virtual addressing.
class GuestMemory {
public:
    static bool compact();
    static uint64_t size();  // bytes reserved: guest addresses below it are usable

    GuestMemory();
    ~GuestMemory();
    GuestMemory(const GuestMemory&) = delete;
    GuestMemory& operator=(const GuestMemory&) = delete;

    // Make [addr, addr+len) readable and writable (page-rounded).
    void commit(uint32_t addr, uint32_t len);

    // Committed ranges as (start, length), page-aligned, in commit order.
    const std::vector<std::pair<uint32_t, uint64_t>>& committed() const { return committed_; }

    uint8_t* base() const { return base_; }
    uint8_t* ptr(uint32_t addr) const { return base_ + addr; }

    template <typename T> T read(uint32_t addr) const {
        T v;
        std::memcpy(&v, base_ + addr, sizeof(T));
        return v;
    }
    template <typename T> void write(uint32_t addr, T v) {
        std::memcpy(base_ + addr, &v, sizeof(T));
    }

private:
    uint8_t* base_ = nullptr;
    std::vector<std::pair<uint32_t, uint64_t>> committed_;
};

}  // namespace scimitar
