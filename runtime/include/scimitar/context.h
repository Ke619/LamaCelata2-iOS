#pragma once
#include <cstdint>

#include "scimitar/x87.h"

namespace scimitar {

class GuestMemory;

// i386 register state for one guest thread. Generated code reads and writes
// these fields directly; condition flags are materialised only when needed.
struct Context {
    uint32_t eax = 0, ecx = 0, edx = 0, ebx = 0;
    uint32_t esp = 0, ebp = 0, esi = 0, edi = 0;
    uint32_t eflags = 0x202;  // IF set, reserved bit 1 set
    uint8_t df = 0;           // direction flag (cld/std), used by string instructions

    struct alignas(16) Xmm { uint8_t b[16]; };
    Xmm xmm[8] = {};
    uint32_t mxcsr = 0x1f80;
    X87 fpu;

    GuestMemory* mem = nullptr;
    // Windows code addresses its thread environment block through fs (SEH
    // frames at fs:[0], TLS at fs:[0x2c]): the guest address fs points to.
    uint32_t fs_base = 0;
};

}  // namespace scimitar
