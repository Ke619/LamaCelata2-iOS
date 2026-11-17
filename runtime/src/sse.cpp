#include "scimitar/sse.h"

#include <cfenv>

namespace scimitar::sse {

void ldmxcsr(Context& c, uint32_t v) {
    c.mxcsr = v;
    // MXCSR.RC (bits 13-14): nearest, down, up, toward zero.
    static const int kModes[4] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
    std::fesetround(kModes[v >> 13 & 3]);
#if defined(__aarch64__)
    // FTZ (bit 15) / DAZ (bit 6): ARM's FPCR.FZ flushes both inputs and outputs.
    uint64_t fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    const bool fz = (v >> 15 & 1) || (v >> 6 & 1);
    fpcr = fz ? fpcr | (1ull << 24) : fpcr & ~(1ull << 24);
    __asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
#elif defined(__arm__)
    // ARMv7 VFP: FPSCR.FZ (bit 24) likewise.
    uint32_t fpscr;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
    const bool fz = (v >> 15 & 1) || (v >> 6 & 1);
    fpscr = fz ? fpscr | (1u << 24) : fpscr & ~(1u << 24);
    __asm__ volatile("vmsr fpscr, %0" : : "r"(fpscr));
#endif
}

}  // namespace scimitar::sse
