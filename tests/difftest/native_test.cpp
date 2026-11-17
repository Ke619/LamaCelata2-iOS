// The native replacements of games/ac2/native_math.cpp (games/acb/ built with
// NATIVE_ACB: native_test_acb, ACB's addresses) against the original
// x86 functions run in Unicorn: for generated inputs (infinities, signed zeros,
// denormals, equal and aliased operands) both run from the same memory, and
// eax, ecx, edx, esp and every byte of the inputs' scratch area must end up
// identical. Every fourth case also has NaN inputs; there a NaN result only has
// to be a NaN on both sides: with two NaN operands QEMU (Unicorn) picks one by
// x87 rules, while SSE hardware, the generated code and the native code
// (scimitar/sse.h) return the first operand's. Needs the game installed locally.
//
//   native_test GAME.app [--cases N]

#include <unicorn/unicorn.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "scimitar/image_loader.h"
#include "scimitar/runtime.h"

namespace scimitar::overrides {
void ov__ZN8scimitar18CullPlaneAABBoxResERKN4Gear7Vector4IfEES4_S4_(scimitar::Context& c);  // CullPlaneAABBoxRes
void ov__ZN4Gear8Matrix44IfEmLERKS1_(scimitar::Context& c);  // Matrix44::operator*=
void ov__ZNK4Gear11Quaternion4IfE8ToMatrixERNS_8Matrix44IfEE(scimitar::Context& c);  // Quaternion4::ToMatrix
void ov__ZN8scimitar15IsConstantEqualIN4Gear7Vector4IfEEEEbRKT_S6_(scimitar::Context& c);  // IsConstantEqual<Vector4>
void ov__ZN8scimitar6IColor16ConvertFromFloatERKN4Gear7Vector4IfEE(scimitar::Context& c);  // IColor::ConvertFromFloat
}  // namespace scimitar::overrides

using namespace scimitar;

namespace {

constexpr uint32_t kScratch = 0x60000000, kScratchSize = 0x1000;
constexpr uint32_t kStackLo = 0x50000000, kStackTop = 0x50010000, kEntryEsp = kStackTop - 0x1000;
constexpr uint32_t kSentinel = 0xdead0000;

struct Rng {
    uint64_t s;
    uint64_t next() {
        uint64_t z = (s += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    uint32_t below(uint32_t n) { return (uint32_t)(next() % n); }
    float unit() { return (float)(next() >> 40) / (float)(1 << 24); }
};

// A float: mostly ordinary values, sometimes special ones; NaN only in the
// cases that allow it.
bool g_nan_inputs = false;
float gen_float(Rng& r) {
    switch (r.below(16)) {
    case 0: return 0.0f;
    case 1: return -0.0f;
    case 2: return g_nan_inputs ? NAN : 2.0f;
    case 3: return r.below(2) ? INFINITY : -INFINITY;
    case 4: return 1e-40f * (r.below(2) ? 1 : -1);  // denormal
    case 5: return r.below(2) ? 1.0f : -1.0f;
    case 6: return 3e38f * (r.unit() - 0.5f);
    case 7: return r.unit();  // 0..1
    default: return (r.unit() - 0.5f) * (float)(1u << r.below(12));
    }
}

class Machine {
public:
    explicit Machine(const std::string& binary) {
        image_ = load_image(mem_, binary);
        mem_.commit(kScratch, kScratchSize);
        mem_.commit(kStackLo, kStackTop - kStackLo);
        if (uc_open(UC_ARCH_X86, UC_MODE_32, &uc_) != UC_ERR_OK) fatal("unicorn open");
        uint64_t cr0 = 0, cr4 = 0;  // SSE on
        uc_reg_read(uc_, UC_X86_REG_CR0, &cr0);
        uc_reg_read(uc_, UC_X86_REG_CR4, &cr4);
        cr0 = (cr0 & ~4ull) | 2ull, cr4 |= (1u << 9) | (1u << 10);
        uc_reg_write(uc_, UC_X86_REG_CR0, &cr0);
        uc_reg_write(uc_, UC_X86_REG_CR4, &cr4);
        std::vector<std::pair<uint64_t, uint64_t>> r;
        for (auto [a, n] : mem_.committed()) r.emplace_back(a, a + n);
        std::sort(r.begin(), r.end());
        std::vector<std::pair<uint64_t, uint64_t>> merged;
        for (auto x : r) {
            if (!merged.empty() && x.first <= merged.back().second)
                merged.back().second = std::max(merged.back().second, x.second);
            else merged.push_back(x);
        }
        for (auto [a, e] : merged)
            if (uc_mem_map_ptr(uc_, a, e - a, UC_PROT_ALL, mem_.ptr((uint32_t)a)) != UC_ERR_OK) fatal("unicorn map");
    }
    ~Machine() { uc_close(uc_); }
    GuestMemory& mem() { return mem_; }

    struct Regs {
        uint32_t eax, ecx, edx, esp;
    };
    // The original from `fn` until it returns to the sentinel.
    bool emulate(uint32_t fn, const Regs& in, Regs* out) {
        mem_.write<uint32_t>(in.esp, kSentinel);
        set(UC_X86_REG_EAX, in.eax), set(UC_X86_REG_ECX, in.ecx), set(UC_X86_REG_EDX, in.edx);
        set(UC_X86_REG_ESP, in.esp), set(UC_X86_REG_EBP, 0x44444444), set(UC_X86_REG_EFLAGS, 0x202);
        const uc_err e = uc_emu_start(uc_, fn, kSentinel, 0, 100000);
        *out = {get(UC_X86_REG_EAX), get(UC_X86_REG_ECX), get(UC_X86_REG_EDX), get(UC_X86_REG_ESP)};
        return e == UC_ERR_OK && get(UC_X86_REG_EIP) == kSentinel;
    }
    void native(GuestFn fn, const Regs& in, Regs* out) {
        mem_.write<uint32_t>(in.esp, kSentinel);
        Context c;
        c.mem = &mem_;
        c.eax = in.eax, c.ecx = in.ecx, c.edx = in.edx, c.esp = in.esp, c.ebp = 0x44444444;
        fn(c);
        *out = {c.eax, c.ecx, c.edx, c.esp};
    }

private:
    uint32_t get(int r) {
        uint32_t v = 0;
        uc_reg_read(uc_, r, &v);
        return v;
    }
    void set(int r, uint32_t v) { uc_reg_write(uc_, r, &v); }
    GuestMemory mem_;
    LoadedImage image_;
    uc_engine* uc_ = nullptr;
};

struct Case {
    const char* name;
    uint32_t addr;
    GuestFn fn;
    // Fills the scratch area and returns the arguments.
    std::function<std::vector<uint32_t>(Machine&, Rng&)> inputs;
};

void put_vec(Machine& m, uint32_t a, Rng& r) {
    for (int k = 0; k < 4; k++) m.mem().write<float>(a + 4 * k, gen_float(r));
}

// The originals' addresses.
#ifdef NATIVE_ACB
constexpr uint32_t kCull = 0x001834c0, kMatMul = 0x00b48b70, kToMatrix = 0x00b52110, kConstEq = 0x00b2d110,
                   kToColor = 0x00ab9090;
#else
constexpr uint32_t kCull = 0x00210110, kMatMul = 0x009ca830, kToMatrix = 0x009cae50, kConstEq = 0x00aee880,
                   kToColor = 0x00c09de0;
#endif

std::vector<Case> cases() {
    const uint32_t A = kScratch, B = kScratch + 0x100, C = kScratch + 0x200;
    return {
        {"CullPlaneAABBoxRes", kCull, scimitar::overrides::ov__ZN8scimitar18CullPlaneAABBoxResERKN4Gear7Vector4IfEES4_S4_,
         [=](Machine& m, Rng& r) {
             put_vec(m, A, r), put_vec(m, B, r), put_vec(m, C, r);
             if (r.below(2))  // an ordinary box and plane
                 for (int k = 0; k < 4; k++) {
                     const float lo = (r.unit() - 0.5f) * 100, hi = lo + r.unit() * 50;
                     m.mem().write<float>(A + 4 * k, (r.unit() - 0.5f) * 2);
                     m.mem().write<float>(B + 4 * k, lo), m.mem().write<float>(C + 4 * k, hi);
                 }
             return std::vector<uint32_t>{A, B, C};
         }},
        {"Matrix44::operator*=", kMatMul, scimitar::overrides::ov__ZN4Gear8Matrix44IfEmLERKS1_,
         [=](Machine& m, Rng& r) {
             for (int k = 0; k < 4; k++) put_vec(m, A + 16 * k, r), put_vec(m, B + 16 * k, r);
             return std::vector<uint32_t>{A, r.below(4) == 0 ? A : B};  // sometimes m *= m
         }},
        {"Quaternion4::ToMatrix", kToMatrix, scimitar::overrides::ov__ZNK4Gear11Quaternion4IfE8ToMatrixERNS_8Matrix44IfEE,
         [=](Machine& m, Rng& r) {
             put_vec(m, A, r);
             for (int k = 0; k < 4; k++) put_vec(m, B + 16 * k, r);
             if (r.below(2)) {  // a unit quaternion
                 float q[4], n = 0;
                 for (float& x : q) x = r.unit() - 0.5f, n += x * x;
                 for (int k = 0; k < 4; k++) m.mem().write<float>(A + 4 * k, q[k] / std::sqrt(n));
             }
             return std::vector<uint32_t>{A, B};
         }},
        {"IsConstantEqual<Vector4>", kConstEq, scimitar::overrides::ov__ZN8scimitar15IsConstantEqualIN4Gear7Vector4IfEEEEbRKT_S6_,
         [=](Machine& m, Rng& r) {
             put_vec(m, A, r), put_vec(m, B, r);
             if (r.below(2)) {
                 std::memcpy(m.mem().ptr(B), m.mem().ptr(A), 16);
                 if (r.below(2)) m.mem().write<uint32_t>(B + 4 * r.below(4), m.mem().read<uint32_t>(B) ^ (1u << r.below(32)));
             }
             return std::vector<uint32_t>{A, B};
         }},
        {"IColor::ConvertFromFloat", kToColor, scimitar::overrides::ov__ZN8scimitar6IColor16ConvertFromFloatERKN4Gear7Vector4IfEE,
         [=](Machine& m, Rng& r) {
             m.mem().write<uint32_t>(C, (uint32_t)r.next());
             put_vec(m, A, r);
             return std::vector<uint32_t>{C, A};
         }},
    };
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s GAME.app [--cases N]\n", argv[0]);
        return 2;
    }
    int n = 20000;
    for (int k = 2; k + 1 < argc; k++)
        if (std::string(argv[k]) == "--cases") n = std::stoi(argv[k + 1]);
    Machine m(std::string(argv[1]) + "/Contents/MacOS/scimitar_sf");
    int bad_functions = 0;
    for (const Case& t : cases()) {
        Rng r{t.addr};
        int bad = 0;
        std::vector<uint8_t> before(kScratchSize), after(kScratchSize);
        for (int k = 0; k < n; k++) {
            g_nan_inputs = k % 4 == 0;
            const std::vector<uint32_t> args = t.inputs(m, r);
            for (size_t i = 0; i < args.size(); i++) m.mem().write<uint32_t>(kEntryEsp + 4 + 4 * (uint32_t)i, args[i]);
            const Machine::Regs in = {(uint32_t)r.next(), (uint32_t)r.next(), (uint32_t)r.next(), kEntryEsp};
            std::memcpy(before.data(), m.mem().ptr(kScratch), kScratchSize);
            Machine::Regs re, rn;
            if (!m.emulate(t.addr, in, &re)) {
                std::printf("FAIL %s: the original did not run to its return\n", t.name);
                bad++;
                break;
            }
            std::memcpy(after.data(), m.mem().ptr(kScratch), kScratchSize);
            std::memcpy(m.mem().ptr(kScratch), before.data(), kScratchSize);
            m.native(t.fn, in, &rn);
            const bool regs = re.eax == rn.eax && re.ecx == rn.ecx && re.edx == rn.edx && re.esp == rn.esp;
            bool memory = true;
            for (uint32_t o = 0; o < kScratchSize; o += 4) {
                uint32_t x, y;
                std::memcpy(&x, after.data() + o, 4), std::memcpy(&y, m.mem().ptr(kScratch + o), 4);
                const auto nan = [](uint32_t v) { return (v & 0x7f800000u) == 0x7f800000u && (v & 0x7fffffu); };
                if (x != y && !(g_nan_inputs && nan(x) && nan(y))) memory = false;
            }
            if (!regs || !memory) {
                if (bad++ < 3) {
                    std::printf("FAIL %s case %d: eax %08x/%08x ecx %08x/%08x edx %08x/%08x esp %08x/%08x%s\n", t.name, k,
                                re.eax, rn.eax, re.ecx, rn.ecx, re.edx, rn.edx, re.esp, rn.esp,
                                memory ? "" : ", memory differs");
                    std::printf("    %s\n", g_nan_inputs ? "(NaN inputs)" : "(no NaN inputs)");
                    for (uint32_t o = 0; o < kScratchSize && !memory; o += 4) {
                        uint32_t x, y;
                        std::memcpy(&x, after.data() + o, 4), std::memcpy(&y, m.mem().ptr(kScratch + o), 4);
                        if (x != y) std::printf("    at +%03x original %08x native %08x\n", o, x, y);
                    }
                }
            }
        }
        std::printf("%s %-28s %d cases%s\n", bad ? "BAD" : "ok ", t.name, n,
                    bad ? (" (" + std::to_string(bad) + " differ)").c_str() : "");
        bad_functions += bad != 0;
    }
    return bad_functions ? 1 : 0;
}
