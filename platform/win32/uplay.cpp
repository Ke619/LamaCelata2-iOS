// Uplay R1 (upc_r1_loader.dll), as Revelations uses it: start, the product
// list (ownership of the downloadable content), achievements, updates and
// quit. Offline here: the game starts as owned, owns no downloadable content
// and its achievements go nowhere. The functions are cdecl.
//
// Asynchronous calls take an overlapped record { completed, reserved, result }
// that the game polls with UPLAY_GetOverlappedOperationResult between
// UPLAY_Update calls; they complete at once.
#include "scimitar/guest_heap.h"
#include "scimitar/runtime.h"

namespace scimitar::imports {
namespace {

void complete(Context& c, uint32_t overlapped, uint32_t result) {
    if (!overlapped) return;
    c.mem->write<uint32_t>(overlapped, 1);
    c.mem->write<uint32_t>(overlapped + 4, 0);
    c.mem->write<uint32_t>(overlapped + 8, result);
}

}  // namespace

// int UPLAY_Start(unsigned uplay_id, unsigned flags): 0 is success (1-3 are
// the "not installed / not running / failed" errors the game reports).
void imp_upc_r1_loader_UPLAY_Start(Context& c) { shim_return(c, 0); }
void imp_upc_r1_loader_UPLAY_Quit(Context& c) { shim_return(c, 1); }
void imp_upc_r1_loader_UPLAY_Update(Context& c) { shim_return(c, 1); }
// int UPLAY_GetLastError(const char** message)
void imp_upc_r1_loader_UPLAY_GetLastError(Context& c) {
    static uint32_t empty = 0;
    if (!empty) empty = heap::alloc(*c.mem, 4), c.mem->write<uint32_t>(empty, 0);
    if (const uint32_t out = arg32(c, 0)) c.mem->write<uint32_t>(out, empty);
    shim_return(c, 0);
}
// int UPLAY_GetOverlappedOperationResult(UPLAY_Overlapped*, int* result):
// true once the operation completed.
void imp_upc_r1_loader_UPLAY_GetOverlappedOperationResult(Context& c) {
    const uint32_t ov = arg32(c, 0), out = arg32(c, 1);
    if (!ov || !c.mem->read<uint32_t>(ov)) return shim_return(c, 0);
    if (out) c.mem->write<uint32_t>(out, c.mem->read<uint32_t>(ov + 8));
    shim_return(c, 1);
}
// int UPLAY_PRODUCT_GetProductList(void* reserved, UPLAY_ProductList** out,
// UPLAY_Overlapped*) (the order the game's call site uses): an empty list
// { count 0, products null }.
void imp_upc_r1_loader_UPLAY_PRODUCT_GetProductList(Context& c) {
    const uint32_t out = arg32(c, 1), ov = arg32(c, 2);
    if (out) {
        const uint32_t list = heap::alloc(*c.mem, 8);
        c.mem->write<uint32_t>(list, 0);
        c.mem->write<uint32_t>(list + 4, 0);
        c.mem->write<uint32_t>(out, list);
    }
    complete(c, ov, 0);
    shim_return(c, 1);
}
void imp_upc_r1_loader_UPLAY_PRODUCT_ReleaseProductList(Context& c) {
    if (const uint32_t list = arg32(c, 0)) heap::release(*c.mem, list);
    shim_return(c, 1);
}
// int UPLAY_ACH_Write(unsigned achievement, UPLAY_Overlapped*)
void imp_upc_r1_loader_UPLAY_ACH_Write(Context& c) {
    complete(c, arg32(c, 1), 0);
    shim_return(c, 1);
}

}  // namespace scimitar::imports
