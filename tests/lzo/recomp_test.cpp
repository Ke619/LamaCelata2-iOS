// Runs the recompiled lzo1x_decompress_safe on every LZO vector and requires
// exactly the results the original x86 code produced in Unicorn
// (tests/lzo/vectors/oracle-lzo2.01.tsv).
//
//   lzo_recomp_test VECTORS_DIR ORACLE.tsv

#include <cstdio>
#include <vector>

#include "lzo_cases.h"
#include "scimitar/context.h"
#include "scimitar/guest_memory.h"

namespace generated {
void lzo1x_decompress_safe(scimitar::Context& c);
}

using namespace lzo_cases;

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s VECTORS_DIR ORACLE.tsv\n", argv[0]);
        return 2;
    }
    const std::vector<Case> cases = load(argv[1]);
    const std::vector<Result> oracle = read_results(argv[2]);
    if (oracle.size() != cases.size()) {
        std::fprintf(stderr, "oracle has %zu results for %zu cases\n", oracle.size(),
                     cases.size());
        return 2;
    }

    scimitar::GuestMemory mem;
    mem.commit(kSrc, kRegion);
    mem.commit(kDst, kRegion);
    mem.commit(kDstLen, 0x1000);
    mem.commit(kStackLo, kStackTop - kStackLo);

    int failures = 0;
    for (size_t k = 0; k < cases.size(); k++) {
        const Case& c = cases[k];
        std::memcpy(mem.ptr(kSrc), c.input.data(), c.input.size());
        std::memset(mem.ptr(kDst), kGuardByte, kRegion);
        mem.write<uint32_t>(kDstLen, c.dst_capacity);

        scimitar::Context ctx;
        ctx.mem = &mem;
        const uint32_t esp = kStackTop - 0x100;
        const uint32_t frame[6] = {kReturnSentinel, kSrc, (uint32_t)c.input.size(), kDst,
                                   kDstLen, 0};
        std::memcpy(mem.ptr(esp), frame, sizeof frame);
        ctx.esp = esp;
        ctx.eax = 0xaaaaaaaa;
        ctx.ecx = 0xcccccccc;
        ctx.edx = 0xdddddddd;
        ctx.ebx = kEbx;
        ctx.esi = kEsi;
        ctx.edi = kEdi;
        ctx.ebp = kEbp;

        generated::lzo1x_decompress_safe(ctx);

        Result r;
        r.ret = static_cast<int32_t>(ctx.eax);
        r.out_len = mem.read<uint32_t>(kDstLen);
        r.out_fnv = r.out_len <= c.dst_capacity ? fnv64(mem.ptr(kDst), r.out_len) : 0;
        for (uint32_t i = c.dst_capacity; i < c.dst_capacity + kGuard; i++)
            if (mem.read<uint8_t>(kDst + i) != kGuardByte) r.guard_ok = false;
        r.regs_ok = ctx.ebx == kEbx && ctx.esi == kEsi && ctx.edi == kEdi && ctx.ebp == kEbp &&
                    ctx.esp == esp + 4;

        const Result& o = oracle[k];
        if (r.ret != o.ret || r.out_len != o.out_len || r.out_fnv != o.out_fnv ||
            r.guard_ok != o.guard_ok || r.regs_ok != o.regs_ok) {
            if (failures++ < 20)
                std::printf("case %d (%s): ret %d/%d len %u/%u fnv %s guard %d/%d regs %d/%d "
                            "(recompiled/original)\n",
                            c.id, c.valid ? "valid" : "damaged", r.ret, o.ret, r.out_len,
                            o.out_len, r.out_fnv == o.out_fnv ? "same" : "DIFF", r.guard_ok,
                            o.guard_ok, r.regs_ok, o.regs_ok);
        }
    }
    std::printf("recompiled lzo1x_decompress_safe: %zu/%zu cases identical to the original\n",
                cases.size() - failures, cases.size());
    return failures ? 1 : 0;
}
