#pragma once
// The loop of scimitar::update_shader_constants<Vector4<float>, SetVSCEnv or
// SetPSCEnv>(unsigned first, Vector4* shadow, const Vector4* src, unsigned
// count), native (AC2's and Brotherhood's native_gfx.cpp): for each run of
// constants that differ from the shadow copy (compared as bits, as
// IsConstantEqual does), copies the run into the shadow and hands it on with
// `emit(index, run, n)`. The game calls it for every draw over the whole
// constant range; the original made one guest call per constant. eax, ecx,
// edx and xmm0 are scratch in the originals and are left as they are (their
// callers do not read them).
#include <cstdint>
#include <cstring>

#include "scimitar/runtime.h"

namespace scimitar::native {

template <typename Emit> void update_shader_constants(Context& c, Emit emit) {
    uint8_t* const M = c.mem->base();
    uint32_t index = arg32(c, 0);
    uint32_t dst = arg32(c, 1) + index * 16, src = arg32(c, 2);
    const uint32_t end = dst + arg32(c, 3) * 16;
    while (dst < end) {
        if (std::memcmp(M + dst, M + src, 16) == 0) {
            dst += 16, src += 16, index++;
            continue;
        }
        const uint32_t start = dst;
        do {
            std::memcpy(M + dst, M + src, 16);
            dst += 16, src += 16;
        } while (dst < end && std::memcmp(M + dst, M + src, 16) != 0);
        const uint32_t n = (dst - start) / 16;
        emit(index, start, n);
        index += n;
    }
    shim_return(c, 0);
}

void imp_glProgramEnvParameters4fvEXT_call(Context& c, uint32_t target, uint32_t index, uint32_t n, uint32_t data);

}  // namespace scimitar::native
