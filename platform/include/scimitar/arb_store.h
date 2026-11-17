#pragma once
// ARB program text kept out of guest memory (docs/NEW_PLATFORMS.md, PS Vita).
//
// AC2 loads the text of all of its shader programs with the materials (14,000
// copies, 110 MB in Florence) and uploads only the ones it draws with, when it
// first does. With the store on (SCIMITAR_ARB_STORE=FILE; on by default in
// the window layout), each program text the loader copies into guest memory
// goes to FILE instead: the guest copy keeps its first line and a reference,
// and the rest of its block is given back to the game's allocator (set_shrink)
// or, failing that, zeroed and its whole pages released. glProgramStringARB
// reads such a program back from the file. The game reads the text only to
// upload it.
#include <cstdint>
#include <string>

#include "scimitar/context.h"
#include "scimitar/guest_memory.h"

namespace scimitar::arb_store {

bool enabled();

// Shrinks the guest heap block at ptr to `size` bytes where it is, through the
// game's own allocator; false if it cannot. The game provides it.
using ShrinkFn = bool (*)(Context& c, uint32_t ptr, uint32_t size);
void set_shrink(ShrinkFn fn);

// After the guest copied n bytes to dst (memcpy): the game's loader copies a
// program's text in one piece, or in consecutive pieces at a stream boundary
// (with the stream's own refills between them), ending with its terminating zero.
void on_copy(Context& c, uint32_t dst, uint32_t n);

// The program text glProgramStringARB(…, len, ptr) means: the stored text
// when ptr holds a reference, else the guest's bytes.
std::string text(GuestMemory& m, uint32_t ptr, uint32_t len);

struct Stats {
    uint64_t programs = 0, bytes = 0;  // texts moved to the store
    uint64_t shrunk = 0;               // bytes given back to the game's allocator
    uint64_t released = 0;             // else, bytes of whole 4 KB pages zeroed in guest memory
};
Stats stats();

}  // namespace scimitar::arb_store
