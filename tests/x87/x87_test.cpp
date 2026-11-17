// Checks the runtime's x87 arithmetic (scimitar/x87.h) against Unicorn running
// the real instructions: conversions in and out, integer stores, add, sub, mul
// and div, for random and special operands under every rounding mode and
// precision setting. Uses tiny hand-assembled code; no game files needed.
//
// Two places where Unicorn (its QEMU base) differs from real x86 and we follow
// the hardware (Intel SDM) are counted separately, not as failures:
// - fisttp of an out-of-range value: QEMU saturates (0x7fff...), x86 stores the
//   integer indefinite (0x8000...);
// - fld m32/m64 of a signalling NaN: x86 quiets it, QEMU keeps it signalling.
//
//   x87_test [CASES_PER_OP]

#include <unicorn/unicorn.h>

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "scimitar/x87.h"

using namespace scimitar;

namespace {

constexpr uint32_t kCode = 0x1000, kData = 0x2000;
constexpr uint32_t kA = kData, kB = kData + 0x10, kOut = kData + 0x20, kCw = kData + 0x40;

struct Rng {
    uint64_t s;
    uint64_t next() {
        uint64_t z = (s += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    uint32_t below(uint32_t n) { return (uint32_t)(next() % n); }
};

// Random extended values: mostly normal numbers near each other in exponent
// (so additions interact), plus zeros, denormals, infinities, NaNs and
// values converted from float/double.
F80 gen(Rng& r, int base_exp) {
    const uint32_t k = r.below(100);
    const uint16_t sign = r.below(2) ? 0x8000 : 0;
    if (k < 4) return {0, sign};
    if (k < 6) return {0x8000000000000000ull, (uint16_t)(sign | 0x7fff)};
    if (k < 8) return {0xc000000000000000ull | (r.next() >> 2), (uint16_t)(sign | 0x7fff)};
    if (k < 11) return {r.next() >> 1, sign};  // denormal
    if (k < 25) return x87::from_f32((uint32_t)r.next());
    if (k < 35) return x87::from_f64(r.next());
    if (k < 40) return x87::from_int((int64_t)r.next() >> r.below(64));
    int e = base_exp + (int)r.below(80) - 40;
    if (k < 45) e = 1 + (int)r.below(0x7ffd);
    if (e < 1) e = 1;
    if (e > 0x7ffe) e = 0x7ffe;
    uint64_t m = r.next() | 0x8000000000000000ull;
    if (r.below(4) == 0) m &= ~0ull << r.below(64);  // short significands round exactly
    return {m, (uint16_t)(sign | e)};
}

class Cpu {
public:
    Cpu() {
        check(uc_open(UC_ARCH_X86, UC_MODE_32, &uc_));
        uint64_t cr0 = 0;
        uc_reg_read(uc_, UC_X86_REG_CR0, &cr0);
        cr0 = (cr0 & ~4ull) | 2ull;
        uc_reg_write(uc_, UC_X86_REG_CR0, &cr0);
        check(uc_mem_map(uc_, 0, 0x10000, UC_PROT_ALL));
    }
    ~Cpu() { uc_close(uc_); }

    // Runs `code` with the FPU reset and control word `cw` loaded first.
    void run(const std::vector<uint8_t>& code, uint16_t cw) {
        std::vector<uint8_t> all = {0xdb, 0xe3,  // fninit
                                    0xd9, 0x2d}; // fldcw [kCw]
        put32(all, kCw);
        all.insert(all.end(), code.begin(), code.end());
        check(uc_mem_write(uc_, kCode, all.data(), all.size()));
        check(uc_mem_write(uc_, kCw, &cw, 2));
        check(uc_emu_start(uc_, kCode, kCode + all.size(), 0, 0));
    }
    void write(uint32_t a, const void* p, size_t n) { check(uc_mem_write(uc_, a, p, n)); }
    void read(uint32_t a, void* p, size_t n) { check(uc_mem_read(uc_, a, p, n)); }

    static void put32(std::vector<uint8_t>& v, uint32_t x) {
        for (int k = 0; k < 4; k++) v.push_back((uint8_t)(x >> (8 * k)));
    }
    // opcode, modrm /reg with disp32 addressing, address
    static void mem(std::vector<uint8_t>& v, uint8_t op, int reg, uint32_t addr) {
        v.push_back(op);
        v.push_back((uint8_t)(reg << 3 | 5));
        put32(v, addr);
    }

private:
    static void check(uc_err e) {
        if (e != UC_ERR_OK) {
            std::fprintf(stderr, "unicorn: %s\n", uc_strerror(e));
            std::exit(2);
        }
    }
    uc_engine* uc_ = nullptr;
};

void wr80(Cpu& cpu, uint32_t a, F80 v) {
    uint8_t b[10];
    std::memcpy(b, &v.mant, 8);
    std::memcpy(b + 8, &v.se, 2);
    cpu.write(a, b, 10);
}
F80 rd80(Cpu& cpu, uint32_t a) {
    uint8_t b[10];
    cpu.read(a, b, 10);
    F80 v;
    std::memcpy(&v.mant, b, 8);
    std::memcpy(&v.se, b + 8, 2);
    return v;
}

int failures = 0, known = 0;

void fail(const std::string& what) {
    if (failures++ < 30) std::printf("MISMATCH %s\n", what.c_str());
}

std::string hex80(F80 v) {
    char b[32];
    std::snprintf(b, sizeof b, "%04x:%016" PRIx64, v.se, v.mant);
    return b;
}

}  // namespace

int run(int cases, bool qemu);

int main(int argc, char** argv) {
    const int cases = argc > 1 ? std::atoi(argv[1]) : 3000;
    // Hardware behaviour (the default), then QEMU-compatible mode, which must
    // match Unicorn exactly.
    const int a = run(cases, false);
    const int b = run(cases, true);
    return a || b ? 1 : 0;
}

int run(int cases, bool qemu) {
    x87::set_qemu_compat(qemu);
    failures = known = 0;
    Cpu cpu;
    Rng rng{42};
    // Control words: every rounding control, with extended, double and single precision.
    std::vector<uint16_t> cws;
    for (uint16_t rc = 0; rc < 4; rc++)
        for (uint16_t pc : {3, 2, 0}) cws.push_back((uint16_t)(0x037f & ~0x0f00) | pc << 8 | rc << 10);

    struct Op {
        const char* name;
        uint8_t b0, b1;  // the popping register form: Fxxp st(1), st (st1 = st1 op st0)
        F80 (*fn)(const X87&, F80, F80);
    };
    const Op ops[] = {
        {"add", 0xde, 0xc1, x87::add},
        {"sub", 0xde, 0xe9, x87::sub},
        {"mul", 0xde, 0xc9, x87::mul},
        {"div", 0xde, 0xf9, x87::div},
    };
    int checked = 0;
    for (int n = 0; n < cases; n++) {
        const uint16_t cw = cws[n % cws.size()];
        X87 s;
        s.cw = cw;
        const int base = 1 + (int)rng.below(0x7ffd);
        const F80 a = gen(rng, base), b = gen(rng, base);

        // Arithmetic: fld a; fld b; fop st(1), st; fstp tbyte [out]  ->  a op b
        for (const Op& op : ops) {
            std::vector<uint8_t> code;
            Cpu::mem(code, 0xdb, 5, kA);  // fld tbyte [a]
            Cpu::mem(code, 0xdb, 5, kB);  // fld tbyte [b]
            code.push_back(op.b0), code.push_back(op.b1);
            Cpu::mem(code, 0xdb, 7, kOut);  // fstp tbyte [out]
            wr80(cpu, kA, a), wr80(cpu, kB, b);
            cpu.run(code, cw);
            const F80 want = rd80(cpu, kOut), got = op.fn(s, a, b);
            checked++;
            if (want.mant != got.mant || want.se != got.se)
                fail(std::string(op.name) + " cw " + std::to_string(cw) + ": " + hex80(a) + ", " +
                     hex80(b) + " -> unicorn " + hex80(want) + ", ours " + hex80(got));
        }

        // Stores: fld a; fst dword [out]; fst qword [out+4]; fistp/fisttp to integers.
        {
            std::vector<uint8_t> code;
            Cpu::mem(code, 0xdb, 5, kA);           // fld tbyte [a]
            Cpu::mem(code, 0xd9, 2, kOut);         // fst dword [out]
            Cpu::mem(code, 0xdd, 2, kOut + 4);     // fst qword [out+4]
            Cpu::mem(code, 0xdf, 2, kOut + 12);    // fist word [out+12]
            Cpu::mem(code, 0xdb, 2, kOut + 14);    // fist dword [out+14]
            code.push_back(0xd9), code.push_back(0xc0);  // fld st(0)
            Cpu::mem(code, 0xdf, 7, kOut + 18);    // fistp qword [out+18]
            code.push_back(0xd9), code.push_back(0xc0);  // fld st(0)
            Cpu::mem(code, 0xdd, 1, kOut + 26);    // fisttp qword [out+26]
            code.push_back(0xd9), code.push_back(0xc0);  // fld st(0)
            Cpu::mem(code, 0xdb, 1, kOut + 34);    // fisttp dword [out+34]
            Cpu::mem(code, 0xdf, 1, kOut + 38);    // fisttp word [out+38] (pops a)
            wr80(cpu, kA, a);
            cpu.run(code, cw);
            uint8_t out[40];
            cpu.read(kOut, out, 40);
            uint32_t f32;
            uint64_t f64, i64, t64;
            uint16_t i16, t16;
            uint32_t i32, t32;
            std::memcpy(&f32, out, 4), std::memcpy(&f64, out + 4, 8);
            std::memcpy(&i16, out + 12, 2), std::memcpy(&i32, out + 14, 4);
            std::memcpy(&i64, out + 18, 8), std::memcpy(&t64, out + 26, 8);
            std::memcpy(&t32, out + 34, 4), std::memcpy(&t16, out + 38, 2);
            checked += 8;
            auto expect = [&](const char* what, uint64_t want, uint64_t got, int bits = 0,
                              bool truncating = false) {
                if (want != got && truncating) {
                    // QEMU saturation instead of the integer indefinite.
                    const uint64_t mask = bits == 64 ? ~0ull : (1ull << bits) - 1;
                    const uint64_t indefinite = 1ull << (bits - 1), max = indefinite - 1;
                    if ((got & mask) == indefinite && (want & mask) == max) {
                        known++;
                        return;
                    }
                }
                if (want != got) {
                    char b[160];
                    std::snprintf(b, sizeof b, "%s cw %04x: %s -> unicorn %016" PRIx64 ", ours %016" PRIx64,
                                  what, cw, hex80(a).c_str(), want, got);
                    fail(b);
                }
            };
            expect("fst m32", f32, x87::to_f32(s, a));
            expect("fst m64", f64, x87::to_f64(s, a));
            expect("fist m16", i16, (uint16_t)x87::to_int(s, a, 16, false));
            expect("fist m32", i32, (uint32_t)x87::to_int(s, a, 32, false));
            expect("fistp m64", i64, (uint64_t)x87::to_int(s, a, 64, false));
            expect("fisttp m64", t64, (uint64_t)x87::to_int(s, a, 64, true), 64, true);
            expect("fisttp m32", t32, (uint32_t)x87::to_int(s, a, 32, true), 32, true);
            expect("fisttp m16", t16, (uint16_t)x87::to_int(s, a, 16, true), 16, true);
        }

        // Loads: fld m32 / m64, fild m64 -> fstp tbyte.
        {
            const uint32_t f = (uint32_t)rng.next();
            const uint64_t d = rng.next();
            const int64_t iv = (int64_t)rng.next() >> rng.below(64);
            cpu.write(kA, &f, 4), cpu.write(kA + 4, &d, 8), cpu.write(kB, &iv, 8);
            std::vector<uint8_t> code;
            Cpu::mem(code, 0xd9, 0, kA);       // fld dword [a]
            Cpu::mem(code, 0xdb, 7, kOut);     // fstp tbyte [out]
            Cpu::mem(code, 0xdd, 0, kA + 4);   // fld qword [a+4]
            Cpu::mem(code, 0xdb, 7, kOut + 10);
            Cpu::mem(code, 0xdf, 5, kB);       // fild qword [b]
            Cpu::mem(code, 0xdb, 7, kOut + 20);
            cpu.run(code, cw);
            F80 w1 = rd80(cpu, kOut), w2 = rd80(cpu, kOut + 10), w3 = rd80(cpu, kOut + 20);
            const F80 g1 = x87::from_f32(f), g2 = x87::from_f64(d), g3 = x87::from_int(iv);
            checked += 3;
            // A signalling NaN load: identical apart from the quiet bit QEMU leaves clear.
            auto snan = [](F80 w, F80 g) {
                return (w.se & 0x7fff) == 0x7fff && w.se == g.se &&
                       (w.mant | 0x4000000000000000ull) == g.mant && !(w.mant & 0x4000000000000000ull);
            };
            if (snan(w1, g1)) known++, w1 = g1;
            if (snan(w2, g2)) known++, w2 = g2;
            if (w1.mant != g1.mant || w1.se != g1.se) fail("fld m32 " + std::to_string(f) + " -> " + hex80(w1) + " ours " + hex80(g1));
            if (w2.mant != g2.mant || w2.se != g2.se) fail("fld m64 -> " + hex80(w2) + " ours " + hex80(g2));
            if (w3.mant != g3.mant || w3.se != g3.se) fail("fild m64 -> " + hex80(w3) + " ours " + hex80(g3));
        }
    }
    if (qemu) {
        std::printf("x87 (QEMU-compatible mode): %d results checked against Unicorn, %d different\n",
                    checked, failures + known);
        return failures + known ? 1 : 0;
    }
    std::printf("x87: %d results checked against Unicorn, %d different, %d known Unicorn "
                "differences (hardware behaviour kept)\n", checked, failures, known);
    return failures ? 1 : 0;
}
