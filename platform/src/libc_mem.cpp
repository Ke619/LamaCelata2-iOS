// Memory and string imports from libSystem.
#include <cstring>

#include "scimitar/arb_store.h"
#include "scimitar/imports.h"

namespace scimitar::imports {

// void* memcpy(void* dst, const void* src, size_t n)
void imp_memcpy(Context& c) {
    const uint32_t dst = arg32(c, 0), src = arg32(c, 1), n = arg32(c, 2);
    if ((uint64_t)dst + n > GuestMemory::size() || (uint64_t)src + n > GuestMemory::size())
        fatal("memcpy: range outside guest memory (%08x <- %08x, %u)", dst, src, n);
    // memmove: identical for valid (non-overlapping) calls, defined otherwise.
    std::memmove(c.mem->ptr(dst), c.mem->ptr(src), n);
    arb_store::on_copy(c, dst, n);  // the loader copying shader program text
    shim_return(c, dst);
}

}  // namespace scimitar::imports

namespace scimitar {
namespace {
const ImportShim kMem[] = {
    {"_memcpy", imports::imp_memcpy},
    {"_memmove", imports::imp_memmove},
};
SCIMITAR_REGISTER_SHIMS(kMem);
}  // namespace
}  // namespace scimitar

namespace scimitar::imports {

// void* memmove(void* dst, const void* src, size_t n)
void imp_memmove(Context& c) {
    const uint32_t dst = arg32(c, 0), src = arg32(c, 1), n = arg32(c, 2);
    if ((uint64_t)dst + n > GuestMemory::size() || (uint64_t)src + n > GuestMemory::size())
        fatal("memmove: range outside guest memory (%08x <- %08x, %u)", dst, src, n);
    std::memmove(c.mem->ptr(dst), c.mem->ptr(src), n);
    shim_return(c, dst);
}

}  // namespace scimitar::imports
