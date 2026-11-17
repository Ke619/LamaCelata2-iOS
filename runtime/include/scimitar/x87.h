#pragma once
// The x87 FPU for generated code. Registers hold real 80-bit extended values;
// loads from float/double/int are exact, and stores and arithmetic round as
// the control word says (rounding control and precision control), so results
// match the original hardware (and Unicorn) bit for bit. The game uses x87
// mainly to return float/double values in ST0 (the i386 ABI) and for 64-bit
// integer conversions, so speed does not matter here.
#include <cstdint>

namespace scimitar {

// An 80-bit extended value: sign and 15-bit exponent in `se`, explicit 64-bit
// significand in `mant`.
struct F80 {
    uint64_t mant = 0;
    uint16_t se = 0;
};

struct X87 {
    F80 r[8];             // physical registers
    uint32_t top = 0;     // ST(i) is r[(top + i) & 7]
    uint8_t valid = 0;    // bit per physical register: holds a value (tag != empty)
    uint16_t cw = 0x037f; // control word: all exceptions masked, extended, nearest
    uint16_t sw = 0;      // status word's condition codes C0-C3 (TOP is `top`)
};

namespace x87 {

// Conversions in (exact).
F80 from_f32(uint32_t bits);
F80 from_f64(uint64_t bits);
F80 from_int(int64_t v);
constexpr F80 kZero{0, 0};
constexpr F80 kOne{0x8000000000000000ull, 0x3fff};

// Conversions out, rounded by the control word's rounding control.
uint32_t to_f32(const X87& s, F80 v);
uint64_t to_f64(const X87& s, F80 v);
// To a `bits`-wide integer (16, 32, 64), rounding by the control word or, for
// fisttp, truncating. Out of range and NaN give the "integer indefinite"
// (only the sign bit set).
int64_t to_int(const X87& s, F80 v, int bits, bool truncate);

// Arithmetic, rounded to the control word's precision and rounding control.
F80 add(const X87& s, F80 a, F80 b);
F80 sub(const X87& s, F80 a, F80 b);
F80 mul(const X87& s, F80 a, F80 b);
F80 div(const X87& s, F80 a, F80 b);
// Precision control is not applied to these (MSVC code calls them through
// its CRT, which never relies on that): sqrt, the transcendental instructions
// and fprem compute in double precision on the host; frndint and fscale are exact.
F80 sqrt(const X87& s, F80 a);
F80 rndint(const X87& s, F80 a);
F80 scale(F80 a, F80 b);       // a * 2^trunc(b)
F80 prem(X87& s, F80 a, F80 b, bool ieee);  // fprem (ieee: fprem1), sets C0-C3
F80 f2xm1(F80 a);
F80 yl2x(F80 y, F80 x);        // y * log2(x)
F80 patan(F80 y, F80 x);       // atan2(y, x)
F80 tan(F80 a);
F80 sin(F80 a);
F80 cos(F80 a);
// fldpi, fldl2e, fldl2t, fldlg2, fldln2 (rounded to nearest).
constexpr F80 kPi{0xc90fdaa22168c235ull, 0x4000};
constexpr F80 kL2e{0xb8aa3b295c17f0bcull, 0x3fff};
constexpr F80 kL2t{0xd49a784bcd1b8afeull, 0x4000};
constexpr F80 kLg2{0x9a209a84fbcff799ull, 0x3ffd};
constexpr F80 kLn2{0xb17217f7d1cf79acull, 0x3ffe};

// Comparison: -1 a < b, 0 equal, 1 a > b, 2 unordered (a NaN operand).
int compare(F80 a, F80 b);
// fcom & co.: C3, C2, C0 from a comparison (C1 cleared).
inline void set_compare(X87& s, int r) {
    static const uint16_t kCC[4] = {0x4000, 0x0000, 0x4500, 0x0100};  // =, >, unordered, <
    s.sw = (uint16_t)((s.sw & ~0x4700u) | kCC[r == 0 ? 0 : r == 1 ? 1 : r == 2 ? 2 : 3]);
}
// fxam: the class of ST0 in C3, C2, C0 and its sign in C1.
void examine(X87& s);
// The status word as fnstsw stores it.
inline uint16_t status(const X87& s) { return (uint16_t)((s.sw & ~0x3800u) | (s.top & 7) << 11); }
// fninit.
inline void init(X87& s) {
    s.top = 0;
    s.valid = 0;
    s.cw = 0x037f;
    s.sw = 0;
}
// fnstenv/fldenv (the 28-byte protected-mode environment) and fsave/frstor
// (environment plus the eight registers, 108 bytes) at host address `p`.
void store_env(const X87& s, uint8_t* p);
void load_env(X87& s, const uint8_t* p);
void save(X87& s, uint8_t* p);
void restore(X87& s, const uint8_t* p);

inline F80 chs(F80 a) { return {a.mant, (uint16_t)(a.se ^ 0x8000)}; }
inline F80 abs(F80 a) { return {a.mant, (uint16_t)(a.se & 0x7fff)}; }

// Test harnesses compare against Unicorn, whose QEMU base differs from x86
// hardware in two places: 32/64-bit fisttp of an out-of-range value saturates
// instead of storing the integer indefinite, and converting a signalling NaN
// (loads from float/double, stores to them) does not quiet it. When enabled, this runtime does the same (tests only; the
// default is the hardware behaviour).
void set_qemu_compat(bool on);

// The value an empty register reads as (stack underflow, masked): the
// default quiet NaN.
constexpr F80 kIndefinite{0xc000000000000000ull, 0xffff};

inline F80& st(X87& s, int i) { return s.r[(s.top + i) & 7]; }
inline F80 get(const X87& s, int i) {
    const uint32_t p = (s.top + i) & 7;
    return s.valid >> p & 1 ? s.r[p] : kIndefinite;
}
inline void set(X87& s, int i, F80 v) {
    const uint32_t p = (s.top + i) & 7;
    s.r[p] = v;
    s.valid |= 1u << p;
}
inline void push(X87& s, F80 v) {
    s.top = (s.top - 1) & 7;
    s.r[s.top] = v;
    s.valid |= 1u << s.top;
}
inline void pop(X87& s) {
    s.valid &= ~(1u << s.top);
    s.top = (s.top + 1) & 7;
}

// MMX registers alias the x87 registers: mmN is the significand of physical
// register N. Writing one sets its exponent field to all ones, and every MMX
// instruction sets TOP to 0 and marks all registers in use; emms empties them.
inline uint64_t mm(X87& s, int n) {
    s.top = 0;
    s.valid = 0xff;
    return s.r[n].mant;
}
inline void set_mm(X87& s, int n, uint64_t v) {
    s.top = 0;
    s.valid = 0xff;
    s.r[n] = {v, 0xffff};
}
inline void emms(X87& s) { s.valid = 0; }

}  // namespace x87
}  // namespace scimitar
