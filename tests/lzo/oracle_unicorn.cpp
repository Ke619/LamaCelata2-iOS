// Runs the original x86 lzo1x_decompress_safe fixture in Unicorn on every LZO
// vector, compares with the liblzo2 2.10 expectations, and writes the original
// code's own results (the authoritative answers for the recompiled version).
//
//   lzo_oracle FIXTURE.bin LOAD_ADDR VECTORS_DIR OUT_RESULTS.tsv

#include <unicorn/unicorn.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "lzo_cases.h"

using namespace lzo_cases;

static void check(uc_err e, const char* what) {
    if (e != UC_ERR_OK) {
        std::fprintf(stderr, "unicorn: %s: %s\n", what, uc_strerror(e));
        std::exit(1);
    }
}

static uint32_t reg(uc_engine* uc, int r) {
    uint32_t v = 0;
    uc_reg_read(uc, r, &v);
    return v;
}

static void set_reg(uc_engine* uc, int r, uint32_t v) { uc_reg_write(uc, r, &v); }

int main(int argc, char** argv) {
    if (argc != 5) {
        std::fprintf(stderr, "usage: %s FIXTURE.bin LOAD_ADDR VECTORS_DIR OUT.tsv\n", argv[0]);
        return 2;
    }
    const std::vector<uint8_t> code = read_file(argv[1]);
    const uint32_t entry = static_cast<uint32_t>(std::stoul(argv[2], nullptr, 0));
    const std::vector<Case> cases = load(argv[3]);

    uc_engine* uc;
    check(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), "open");
    const uint32_t code_page = entry & ~0xfffu;
    const uint32_t code_size = ((entry + code.size() + 0xfff) & ~0xfffu) - code_page;
    check(uc_mem_map(uc, code_page, code_size, UC_PROT_READ | UC_PROT_EXEC), "map code");
    check(uc_mem_write(uc, entry, code.data(), code.size()), "write code");
    check(uc_mem_map(uc, kSrc, kRegion, UC_PROT_READ | UC_PROT_WRITE), "map src");
    check(uc_mem_map(uc, kDst, kRegion, UC_PROT_READ | UC_PROT_WRITE), "map dst");
    check(uc_mem_map(uc, kDstLen, 0x1000, UC_PROT_READ | UC_PROT_WRITE), "map dst_len");
    check(uc_mem_map(uc, kStackLo, kStackTop - kStackLo, UC_PROT_READ | UC_PROT_WRITE),
          "map stack");

    std::vector<uint8_t> fill(kRegion, kGuardByte);
    std::vector<Result> results;
    int valid_ok = 0, valid_total = 0, damaged_same = 0, damaged_total = 0;
    for (const Case& c : cases) {
        check(uc_mem_write(uc, kSrc, c.input.data(), c.input.size()), "write src");
        check(uc_mem_write(uc, kDst, fill.data(), fill.size()), "fill dst");
        check(uc_mem_write(uc, kDstLen, &c.dst_capacity, 4), "write dst_len");

        const uint32_t esp = kStackTop - 0x100;
        const uint32_t frame[6] = {kReturnSentinel, kSrc, (uint32_t)c.input.size(), kDst,
                                   kDstLen, 0};
        check(uc_mem_write(uc, esp, frame, sizeof frame), "write frame");
        set_reg(uc, UC_X86_REG_ESP, esp);
        set_reg(uc, UC_X86_REG_EAX, 0xaaaaaaaa);
        set_reg(uc, UC_X86_REG_ECX, 0xcccccccc);
        set_reg(uc, UC_X86_REG_EDX, 0xdddddddd);
        set_reg(uc, UC_X86_REG_EBX, kEbx);
        set_reg(uc, UC_X86_REG_ESI, kEsi);
        set_reg(uc, UC_X86_REG_EDI, kEdi);
        set_reg(uc, UC_X86_REG_EBP, kEbp);
        set_reg(uc, UC_X86_REG_EFLAGS, 0x202);

        check(uc_emu_start(uc, entry, kReturnSentinel, 0, 500'000'000), "run");
        if (reg(uc, UC_X86_REG_EIP) != kReturnSentinel) {
            std::fprintf(stderr, "case %d: did not return (eip=%08x)\n", c.id,
                         reg(uc, UC_X86_REG_EIP));
            return 1;
        }

        Result r;
        r.ret = static_cast<int32_t>(reg(uc, UC_X86_REG_EAX));
        uc_mem_read(uc, kDstLen, &r.out_len, 4);
        std::vector<uint8_t> dst(kRegion);
        uc_mem_read(uc, kDst, dst.data(), dst.size());
        r.out_fnv = r.out_len <= c.dst_capacity ? fnv64(dst.data(), r.out_len) : 0;
        for (uint32_t i = c.dst_capacity; i < c.dst_capacity + kGuard; i++)
            if (dst[i] != kGuardByte) r.guard_ok = false;
        r.regs_ok = reg(uc, UC_X86_REG_EBX) == kEbx && reg(uc, UC_X86_REG_ESI) == kEsi &&
                    reg(uc, UC_X86_REG_EDI) == kEdi && reg(uc, UC_X86_REG_EBP) == kEbp &&
                    reg(uc, UC_X86_REG_ESP) == esp + 4;
        results.push_back(r);

        const bool same = r.ret == c.expect_ret && r.out_len == c.expect_out_len &&
                          r.out_fnv == c.expect_fnv;
        if (c.valid) {
            valid_total++;
            if (same && r.guard_ok && r.regs_ok) valid_ok++;
            else
                std::printf("VALID MISMATCH case %d: ret %d (want %d) len %u (want %u)%s%s\n",
                            c.id, r.ret, c.expect_ret, r.out_len, c.expect_out_len,
                            r.guard_ok ? "" : " guard-overwritten",
                            r.regs_ok ? "" : " regs-not-restored");
        } else {
            damaged_total++;
            if (same) damaged_same++;
            else
                std::printf("damaged case %d differs from liblzo2 2.10: ret %d (2.10: %d), "
                            "len %u (2.10: %u)\n",
                            c.id, r.ret, c.expect_ret, r.out_len, c.expect_out_len);
            if (!r.guard_ok || !r.regs_ok)
                std::printf("  case %d:%s%s\n", c.id, r.guard_ok ? "" : " guard-overwritten",
                            r.regs_ok ? "" : " regs-not-restored");
        }
    }
    uc_close(uc);

    write_results(argv[4], cases, results,
                  "original x86 lzo1x_decompress_safe (LZO 2.01) from AC2 scimitar_sf, run in Unicorn");
    std::printf("valid: %d/%d match liblzo2 exactly; damaged: %d/%d same as liblzo2 2.10\n",
                valid_ok, valid_total, damaged_same, damaged_total);
    return valid_ok == valid_total ? 0 : 1;
}
