#pragma once
// SSE/SSE2/SSE3 operations for generated code, with x86 results on any host:
// - an invalid operation yields the x86 default NaN (negative quiet NaN), and a
//   NaN operand propagates as on x86 (the first operand's, quieted, else the
//   second's); ARM's default NaN is positive, so results are fixed up;
// - float->int conversions give the "integer indefinite" (0x80000000) for NaN
//   and out-of-range values instead of saturating;
// - min/max follow x86 (the second operand unless the first is strictly
//   smaller/larger), not IEEE minNum/maxNum;
// - rcp/rsqrt are computed exactly (1/x, 1/sqrt(x)), as Unicorn/QEMU do; real
//   CPUs return 12-bit approximations, which callers refine anyway.
// Arithmetic uses the host FPU, which rounds to nearest like the default
// MXCSR; ldmxcsr applies other rounding modes and flush-to-zero to the host.
#include <cmath>
#include <cstdint>
#include <cstring>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include "scimitar/context.h"

namespace scimitar::sse {

using X = Context::Xmm;

// ---- lanes ----
inline float f32(const X& x, int i) { float v; std::memcpy(&v, x.b + 4 * i, 4); return v; }
inline double f64(const X& x, int i) { double v; std::memcpy(&v, x.b + 8 * i, 8); return v; }
inline uint32_t u32(const X& x, int i) { uint32_t v; std::memcpy(&v, x.b + 4 * i, 4); return v; }
inline uint64_t u64(const X& x, int i) { uint64_t v; std::memcpy(&v, x.b + 8 * i, 8); return v; }
inline uint16_t u16(const X& x, int i) { uint16_t v; std::memcpy(&v, x.b + 2 * i, 2); return v; }
inline void set(X& x, int i, float v) { std::memcpy(x.b + 4 * i, &v, 4); }
inline void set(X& x, int i, double v) { std::memcpy(x.b + 8 * i, &v, 8); }
inline void set32(X& x, int i, uint32_t v) { std::memcpy(x.b + 4 * i, &v, 4); }
inline void set64(X& x, int i, uint64_t v) { std::memcpy(x.b + 8 * i, &v, 8); }
inline void set16(X& x, int i, uint16_t v) { std::memcpy(x.b + 2 * i, &v, 2); }

inline X zero() { return X{}; }
inline X from32(uint32_t v) { X x{}; set32(x, 0, v); return x; }
inline X from64(uint64_t v) { X x{}; set64(x, 0, v); return x; }
inline X ld128(const uint8_t* M, uint32_t a) { X x; std::memcpy(x.b, M + a, 16); return x; }
inline void st128(uint8_t* M, uint32_t a, const X& x) { std::memcpy(M + a, x.b, 16); }

// ---- NaN handling ----
inline float bits_f(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }
inline double bits_d(uint64_t u) { double d; std::memcpy(&d, &u, 8); return d; }
inline uint32_t fbits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
inline uint64_t dbits(double d) { uint64_t u; std::memcpy(&u, &d, 8); return u; }

inline float quiet(float a) { return bits_f(fbits(a) | 0x00400000u); }
inline double quiet(double a) { return bits_d(dbits(a) | 0x0008000000000000ull); }
inline float default_nan_f() { return bits_f(0xffc00000u); }
inline double default_nan_d() { return bits_d(0xfff8000000000000ull); }

template <typename T> inline T default_nan();
template <> inline float default_nan<float>() { return default_nan_f(); }
template <> inline double default_nan<double>() { return default_nan_d(); }

template <typename T> inline T fix2(T a, T b, T r) {
    if (__builtin_expect(r == r, 1)) return r;
    if (a != a) return quiet(a);
    if (b != b) return quiet(b);
    return default_nan<T>();
}
template <typename T> inline T fix1(T a, T r) {
    if (__builtin_expect(r == r, 1)) return r;
    if (a != a) return quiet(a);
    return default_nan<T>();
}

template <typename T> inline T add(T a, T b) { return fix2(a, b, a + b); }
template <typename T> inline T sub(T a, T b) { return fix2(a, b, a - b); }
template <typename T> inline T mul(T a, T b) { return fix2(a, b, a * b); }
template <typename T> inline T div(T a, T b) { return fix2(a, b, a / b); }
template <typename T> inline T min(T a, T b) { return a < b ? a : b; }
template <typename T> inline T max(T a, T b) { return a > b ? a : b; }
inline float sqrt(float a) { return fix1(a, std::sqrt(a)); }
inline double sqrt(double a) { return fix1(a, std::sqrt(a)); }
inline float rcp(float a) { return fix1(a, 1.0f / a); }
inline float rsqrt(float a) { return fix1(a, 1.0f / sqrt(a)); }

// ---- scalar and packed arithmetic: d = d op s ----
// Packed operations on arm64 run as one NEON instruction; only when a lane of
// the result is NaN are the lanes redone one by one with the x86 NaN rules
// (rare, so the check is a single vector compare). min/max are a compare and
// select, which is exactly x86's rule.
#define SCIMITAR_SSE_SCALAR(NAME, FN)                                                      \
    inline X NAME##ss(X d, const X& s) { set(d, 0, FN(f32(d, 0), f32(s, 0))); return d; } \
    inline X NAME##sd(X d, const X& s) { set(d, 0, FN(f64(d, 0), f64(s, 0))); return d; }
#define SCIMITAR_SSE_PACKED_LANES(NAME, FN)                                                \
    inline X NAME##ps_lanes(X d, const X& s) {                                            \
        for (int i = 0; i < 4; i++) set(d, i, FN(f32(d, i), f32(s, i)));                   \
        return d;                                                                          \
    }                                                                                      \
    inline X NAME##pd_lanes(X d, const X& s) {                                            \
        for (int i = 0; i < 2; i++) set(d, i, FN(f64(d, i), f64(s, i)));                   \
        return d;                                                                          \
    }
SCIMITAR_SSE_SCALAR(add, add)
SCIMITAR_SSE_SCALAR(sub, sub)
SCIMITAR_SSE_SCALAR(mul, mul)
SCIMITAR_SSE_SCALAR(div, div)
SCIMITAR_SSE_SCALAR(min, min)
SCIMITAR_SSE_SCALAR(max, max)
SCIMITAR_SSE_PACKED_LANES(add, add)
SCIMITAR_SSE_PACKED_LANES(sub, sub)
SCIMITAR_SSE_PACKED_LANES(mul, mul)
SCIMITAR_SSE_PACKED_LANES(div, div)
SCIMITAR_SSE_PACKED_LANES(min, min)
SCIMITAR_SSE_PACKED_LANES(max, max)
#undef SCIMITAR_SSE_SCALAR
#undef SCIMITAR_SSE_PACKED_LANES

#if defined(__aarch64__)
inline float32x4_t vf(const X& x) { float32x4_t v; std::memcpy(&v, x.b, 16); return v; }
inline float64x2_t vd(const X& x) { float64x2_t v; std::memcpy(&v, x.b, 16); return v; }
inline X xv(float32x4_t v) { X x; std::memcpy(x.b, &v, 16); return x; }
inline X xv(float64x2_t v) { X x; std::memcpy(x.b, &v, 16); return x; }
inline bool any_nan(float32x4_t r) { return vminvq_u32(vceqq_f32(r, r)) == 0; }
inline bool any_nan(float64x2_t r) { return (vgetq_lane_u64(vceqq_f64(r, r), 0) & vgetq_lane_u64(vceqq_f64(r, r), 1)) == 0; }
#define SCIMITAR_SSE_NEON(NAME, OPF, OPD)                                                  \
    inline X NAME##ps(X d, const X& s) {                                                  \
        const float32x4_t r = OPF(vf(d), vf(s));                                           \
        return __builtin_expect(any_nan(r), 0) ? NAME##ps_lanes(d, s) : xv(r);             \
    }                                                                                      \
    inline X NAME##pd(X d, const X& s) {                                                  \
        const float64x2_t r = OPD(vd(d), vd(s));                                           \
        return __builtin_expect(any_nan(r), 0) ? NAME##pd_lanes(d, s) : xv(r);             \
    }
SCIMITAR_SSE_NEON(add, vaddq_f32, vaddq_f64)
SCIMITAR_SSE_NEON(sub, vsubq_f32, vsubq_f64)
SCIMITAR_SSE_NEON(mul, vmulq_f32, vmulq_f64)
SCIMITAR_SSE_NEON(div, vdivq_f32, vdivq_f64)
#undef SCIMITAR_SSE_NEON
// x86 min/max: the first operand if strictly smaller (larger), else the second.
inline X minps(X d, const X& s) { const float32x4_t a = vf(d), b = vf(s); return xv(vbslq_f32(vcltq_f32(a, b), a, b)); }
inline X maxps(X d, const X& s) { const float32x4_t a = vf(d), b = vf(s); return xv(vbslq_f32(vcgtq_f32(a, b), a, b)); }
inline X minpd(X d, const X& s) { const float64x2_t a = vd(d), b = vd(s); return xv(vbslq_f64(vcltq_f64(a, b), a, b)); }
inline X maxpd(X d, const X& s) { const float64x2_t a = vd(d), b = vd(s); return xv(vbslq_f64(vcgtq_f64(a, b), a, b)); }
#else
#define SCIMITAR_SSE_PACKED(NAME)                                                          \
    inline X NAME##ps(X d, const X& s) { return NAME##ps_lanes(d, s); }                   \
    inline X NAME##pd(X d, const X& s) { return NAME##pd_lanes(d, s); }
SCIMITAR_SSE_PACKED(add)
SCIMITAR_SSE_PACKED(sub)
SCIMITAR_SSE_PACKED(mul)
SCIMITAR_SSE_PACKED(div)
SCIMITAR_SSE_PACKED(min)
SCIMITAR_SSE_PACKED(max)
#undef SCIMITAR_SSE_PACKED
#endif

inline X sqrtss(X d, const X& s) { set(d, 0, sqrt(f32(s, 0))); return d; }
inline X sqrtsd(X d, const X& s) { set(d, 0, sqrt(f64(s, 0))); return d; }
inline X rcpss(X d, const X& s) { set(d, 0, rcp(f32(s, 0))); return d; }
inline X rsqrtss(X d, const X& s) { set(d, 0, rsqrt(f32(s, 0))); return d; }
inline X sqrtps(X, const X& s) { X d; for (int i = 0; i < 4; i++) set(d, i, sqrt(f32(s, i))); return d; }
inline X sqrtpd(X, const X& s) { X d; for (int i = 0; i < 2; i++) set(d, i, sqrt(f64(s, i))); return d; }
inline X rcpps(X, const X& s) { X d; for (int i = 0; i < 4; i++) set(d, i, rcp(f32(s, i))); return d; }
inline X rsqrtps(X, const X& s) { X d; for (int i = 0; i < 4; i++) set(d, i, rsqrt(f32(s, i))); return d; }

// SSE3 horizontal adds.
inline X haddps(X d, const X& s) {
    X r;
    set(r, 0, add(f32(d, 0), f32(d, 1))), set(r, 1, add(f32(d, 2), f32(d, 3)));
    set(r, 2, add(f32(s, 0), f32(s, 1))), set(r, 3, add(f32(s, 2), f32(s, 3)));
    return r;
}
inline X haddpd(X d, const X& s) {
    X r;
    set(r, 0, add(f64(d, 0), f64(d, 1))), set(r, 1, add(f64(s, 0), f64(s, 1)));
    return r;
}

// ---- comparisons ----
// cmpXX predicate (imm8 & 7): eq lt le unord neq nlt nle ord.
template <typename T> inline bool cmp_pred(T a, T b, int p) {
    switch (p & 7) {
    case 0: return a == b;
    case 1: return a < b;
    case 2: return a <= b;
    case 3: return a != a || b != b;
    case 4: return !(a == b);
    case 5: return !(a < b);
    case 6: return !(a <= b);
    default: return a == a && b == b;
    }
}
inline X cmpss(X d, const X& s, int p) { set32(d, 0, cmp_pred(f32(d, 0), f32(s, 0), p) ? ~0u : 0); return d; }
inline X cmpsd(X d, const X& s, int p) { set64(d, 0, cmp_pred(f64(d, 0), f64(s, 0), p) ? ~0ull : 0); return d; }
inline X cmpps(X d, const X& s, int p) {
    for (int i = 0; i < 4; i++) set32(d, i, cmp_pred(f32(d, i), f32(s, i), p) ? ~0u : 0);
    return d;
}
inline X cmppd(X d, const X& s, int p) {
    for (int i = 0; i < 2; i++) set64(d, i, cmp_pred(f64(d, i), f64(s, i), p) ? ~0ull : 0);
    return d;
}

// ucomiss/comiss: ZF, PF, CF as x86 sets them (OF, SF, AF cleared).
struct Flags { bool zf, pf, cf; };
template <typename T> inline Flags ucomi(T a, T b) {
    if (a != a || b != b) return {true, true, true};
    if (a < b) return {false, false, true};
    if (a == b) return {true, false, false};
    return {false, false, false};
}

// ---- bitwise and data movement ----
inline X andps(X d, const X& s) { for (int i = 0; i < 16; i++) d.b[i] &= s.b[i]; return d; }
inline X andnps(X d, const X& s) { for (int i = 0; i < 16; i++) d.b[i] = ~d.b[i] & s.b[i]; return d; }
inline X orps(X d, const X& s) { for (int i = 0; i < 16; i++) d.b[i] |= s.b[i]; return d; }
inline X xorps(X d, const X& s) { for (int i = 0; i < 16; i++) d.b[i] ^= s.b[i]; return d; }

inline X shufps(const X& d, const X& s, int imm) {
    X r;
    set32(r, 0, u32(d, imm & 3)), set32(r, 1, u32(d, imm >> 2 & 3));
    set32(r, 2, u32(s, imm >> 4 & 3)), set32(r, 3, u32(s, imm >> 6 & 3));
    return r;
}
inline X shufpd(const X& d, const X& s, int imm) {
    X r;
    set64(r, 0, u64(d, imm & 1)), set64(r, 1, u64(s, imm >> 1 & 1));
    return r;
}
inline X unpcklps(const X& d, const X& s) {
    X r;
    set32(r, 0, u32(d, 0)), set32(r, 1, u32(s, 0)), set32(r, 2, u32(d, 1)), set32(r, 3, u32(s, 1));
    return r;
}
inline X unpckhps(const X& d, const X& s) {
    X r;
    set32(r, 0, u32(d, 2)), set32(r, 1, u32(s, 2)), set32(r, 2, u32(d, 3)), set32(r, 3, u32(s, 3));
    return r;
}
inline X unpcklpd(const X& d, const X& s) { X r; set64(r, 0, u64(d, 0)), set64(r, 1, u64(s, 0)); return r; }
inline X unpckhpd(const X& d, const X& s) { X r; set64(r, 0, u64(d, 1)), set64(r, 1, u64(s, 1)); return r; }
inline X movhlps(X d, const X& s) { set64(d, 0, u64(s, 1)); return d; }
inline X movlhps(X d, const X& s) { set64(d, 1, u64(s, 0)); return d; }
inline uint32_t movmskps(const X& s) {
    uint32_t m = 0;
    for (int i = 0; i < 4; i++) m |= (u32(s, i) >> 31) << i;
    return m;
}
inline uint32_t movmskpd(const X& s) { return (uint32_t)(u64(s, 0) >> 63) | (uint32_t)(u64(s, 1) >> 63) << 1; }

// ---- conversions ----
inline int32_t indefinite_or(bool ok, int32_t v) { return ok ? v : INT32_MIN; }
template <typename T> inline bool fits_i32(T v) { return v >= (T)-2147483648.0 && v < (T)2147483648.0; }
template <typename T> inline int32_t cvtt_i32(T v) { return fits_i32(v) ? (int32_t)v : INT32_MIN; }
template <typename T> inline int32_t cvt_i32(T v) {
    const T r = std::nearbyint(v);  // current rounding mode, as MXCSR.RC
    return fits_i32(r) ? (int32_t)r : INT32_MIN;
}

inline X cvtsi2ss(X d, int32_t v) { set(d, 0, (float)v); return d; }
inline X cvtsi2sd(X d, int32_t v) { set(d, 0, (double)v); return d; }
inline X cvtss2sd(X d, const X& s) { set(d, 0, (double)f32(s, 0)); return d; }
inline X cvtsd2ss(X d, const X& s) { set(d, 0, (float)f64(s, 0)); return d; }
inline X cvtdq2ps(X, const X& s) { X d; for (int i = 0; i < 4; i++) set(d, i, (float)(int32_t)u32(s, i)); return d; }
inline X cvtdq2pd(X, const X& s) { X d; for (int i = 0; i < 2; i++) set(d, i, (double)(int32_t)u32(s, i)); return d; }
inline X cvttps2dq(X, const X& s) { X d; for (int i = 0; i < 4; i++) set32(d, i, (uint32_t)cvtt_i32(f32(s, i))); return d; }
inline X cvtps2dq(X, const X& s) { X d; for (int i = 0; i < 4; i++) set32(d, i, (uint32_t)cvt_i32(f32(s, i))); return d; }
inline X cvttpd2dq(X, const X& s) {
    X d{};
    for (int i = 0; i < 2; i++) set32(d, i, (uint32_t)cvtt_i32(f64(s, i)));
    return d;
}
inline X cvtps2pd(X, const X& s) { X d; for (int i = 0; i < 2; i++) set(d, i, (double)f32(s, i)); return d; }
inline X cvtpd2ps(X, const X& s) {
    X d{};
    for (int i = 0; i < 2; i++) set(d, i, (float)f64(s, i));
    return d;
}

// ---- SSE2 integer ----
inline X paddd(X d, const X& s) { for (int i = 0; i < 4; i++) set32(d, i, u32(d, i) + u32(s, i)); return d; }
inline X psubd(X d, const X& s) { for (int i = 0; i < 4; i++) set32(d, i, u32(d, i) - u32(s, i)); return d; }
inline X paddq(X d, const X& s) { for (int i = 0; i < 2; i++) set64(d, i, u64(d, i) + u64(s, i)); return d; }
inline X psubq(X d, const X& s) { for (int i = 0; i < 2; i++) set64(d, i, u64(d, i) - u64(s, i)); return d; }
inline X paddw(X d, const X& s) { for (int i = 0; i < 8; i++) set16(d, i, (uint16_t)(u16(d, i) + u16(s, i))); return d; }
inline X psubw(X d, const X& s) { for (int i = 0; i < 8; i++) set16(d, i, (uint16_t)(u16(d, i) - u16(s, i))); return d; }
inline X pand(X d, const X& s) { return andps(d, s); }
inline X pandn(X d, const X& s) { return andnps(d, s); }
inline X por(X d, const X& s) { return orps(d, s); }
inline X pxor(X d, const X& s) { return xorps(d, s); }
inline X pcmpeqd(X d, const X& s) { for (int i = 0; i < 4; i++) set32(d, i, u32(d, i) == u32(s, i) ? ~0u : 0); return d; }
inline X pcmpgtd(X d, const X& s) {
    for (int i = 0; i < 4; i++) set32(d, i, (int32_t)u32(d, i) > (int32_t)u32(s, i) ? ~0u : 0);
    return d;
}
inline X pcmpgtw(X d, const X& s) {
    for (int i = 0; i < 8; i++) set16(d, i, (int16_t)u16(d, i) > (int16_t)u16(s, i) ? 0xffff : 0);
    return d;
}
// Shifts by a count; counts beyond the lane width clear the lane (or fill
// with the sign for arithmetic shifts).
inline X pslld(X d, uint64_t n) { for (int i = 0; i < 4; i++) set32(d, i, n > 31 ? 0 : u32(d, i) << n); return d; }
inline X psrld(X d, uint64_t n) { for (int i = 0; i < 4; i++) set32(d, i, n > 31 ? 0 : u32(d, i) >> n); return d; }
inline X psrad(X d, uint64_t n) {
    for (int i = 0; i < 4; i++) set32(d, i, (uint32_t)((int32_t)u32(d, i) >> (n > 31 ? 31 : n)));
    return d;
}
inline X psllq(X d, uint64_t n) { for (int i = 0; i < 2; i++) set64(d, i, n > 63 ? 0 : u64(d, i) << n); return d; }
inline X psrlq(X d, uint64_t n) { for (int i = 0; i < 2; i++) set64(d, i, n > 63 ? 0 : u64(d, i) >> n); return d; }
inline X psllw(X d, uint64_t n) { for (int i = 0; i < 8; i++) set16(d, i, n > 15 ? 0 : (uint16_t)(u16(d, i) << n)); return d; }
inline X psrlw(X d, uint64_t n) { for (int i = 0; i < 8; i++) set16(d, i, n > 15 ? 0 : (uint16_t)(u16(d, i) >> n)); return d; }
inline X psraw(X d, uint64_t n) {
    for (int i = 0; i < 8; i++) set16(d, i, (uint16_t)((int16_t)u16(d, i) >> (n > 15 ? 15 : n)));
    return d;
}
inline X pslldq(const X& d, int n) {
    X r{};
    if (n > 15) return r;
    for (int i = n; i < 16; i++) r.b[i] = d.b[i - n];
    return r;
}
inline X psrldq(const X& d, int n) {
    X r{};
    if (n > 15) return r;
    for (int i = 0; i + n < 16; i++) r.b[i] = d.b[i + n];
    return r;
}
inline X punpcklbw(const X& d, const X& s) { X r; for (int i = 0; i < 8; i++) r.b[2 * i] = d.b[i], r.b[2 * i + 1] = s.b[i]; return r; }
inline X punpckhbw(const X& d, const X& s) { X r; for (int i = 0; i < 8; i++) r.b[2 * i] = d.b[8 + i], r.b[2 * i + 1] = s.b[8 + i]; return r; }
inline X punpcklwd(const X& d, const X& s) { X r; for (int i = 0; i < 4; i++) set16(r, 2 * i, u16(d, i)), set16(r, 2 * i + 1, u16(s, i)); return r; }
inline X punpckhwd(const X& d, const X& s) { X r; for (int i = 0; i < 4; i++) set16(r, 2 * i, u16(d, 4 + i)), set16(r, 2 * i + 1, u16(s, 4 + i)); return r; }
inline X punpckldq(const X& d, const X& s) { return unpcklps(d, s); }
inline X punpckhdq(const X& d, const X& s) { return unpckhps(d, s); }
inline X punpcklqdq(const X& d, const X& s) { return unpcklpd(d, s); }
inline X punpckhqdq(const X& d, const X& s) { return unpckhpd(d, s); }
// Packs with saturation: d's elements in the low half, s's in the high half.
inline X packssdw(const X& d, const X& s) {
    X r;
    auto sat = [](uint32_t v) { const int32_t x = (int32_t)v; return (uint16_t)(int16_t)(x < -32768 ? -32768 : x > 32767 ? 32767 : x); };
    for (int i = 0; i < 4; i++) set16(r, i, sat(u32(d, i))), set16(r, 4 + i, sat(u32(s, i)));
    return r;
}
inline X packsswb(const X& d, const X& s) {
    X r;
    auto sat = [](uint16_t v) { const int16_t x = (int16_t)v; return (uint8_t)(int8_t)(x < -128 ? -128 : x > 127 ? 127 : x); };
    for (int i = 0; i < 8; i++) r.b[i] = sat(u16(d, i)), r.b[8 + i] = sat(u16(s, i));
    return r;
}
inline X packuswb(const X& d, const X& s) {
    X r;
    auto sat = [](uint16_t v) { const int16_t x = (int16_t)v; return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x); };
    for (int i = 0; i < 8; i++) r.b[i] = sat(u16(d, i)), r.b[8 + i] = sat(u16(s, i));
    return r;
}
inline X pshufd(const X&, const X& s, int imm) {
    X r;
    for (int i = 0; i < 4; i++) set32(r, i, u32(s, imm >> (2 * i) & 3));
    return r;
}

// Saturating and multiplying word/byte arithmetic (MSVC builds' sound mixers).
inline int16_t sat16(int32_t x) { return (int16_t)(x < -32768 ? -32768 : x > 32767 ? 32767 : x); }
inline uint16_t usat16(int32_t x) { return (uint16_t)(x < 0 ? 0 : x > 65535 ? 65535 : x); }
inline X paddsw(X d, const X& s) { for (int i = 0; i < 8; i++) set16(d, i, (uint16_t)sat16((int16_t)u16(d, i) + (int16_t)u16(s, i))); return d; }
inline X psubsw(X d, const X& s) { for (int i = 0; i < 8; i++) set16(d, i, (uint16_t)sat16((int16_t)u16(d, i) - (int16_t)u16(s, i))); return d; }
inline X paddusw(X d, const X& s) { for (int i = 0; i < 8; i++) set16(d, i, usat16(u16(d, i) + u16(s, i))); return d; }
inline X psubusw(X d, const X& s) { for (int i = 0; i < 8; i++) set16(d, i, usat16(u16(d, i) - u16(s, i))); return d; }
inline X paddb(X d, const X& s) { for (int i = 0; i < 16; i++) d.b[i] = (uint8_t)(d.b[i] + s.b[i]); return d; }
inline X psubb(X d, const X& s) { for (int i = 0; i < 16; i++) d.b[i] = (uint8_t)(d.b[i] - s.b[i]); return d; }
inline X pmullw(X d, const X& s) { for (int i = 0; i < 8; i++) set16(d, i, (uint16_t)((int16_t)u16(d, i) * (int16_t)u16(s, i))); return d; }
inline X pmulhw(X d, const X& s) { for (int i = 0; i < 8; i++) set16(d, i, (uint16_t)(((int16_t)u16(d, i) * (int16_t)u16(s, i)) >> 16)); return d; }
inline X pmulhuw(X d, const X& s) { for (int i = 0; i < 8; i++) set16(d, i, (uint16_t)(((uint32_t)u16(d, i) * u16(s, i)) >> 16)); return d; }
inline X pmaddwd(X d, const X& s) {
    X r;
    for (int i = 0; i < 4; i++)
        set32(r, i, (uint32_t)((int32_t)(int16_t)u16(d, 2 * i) * (int16_t)u16(s, 2 * i) +
                               (int32_t)(int16_t)u16(d, 2 * i + 1) * (int16_t)u16(s, 2 * i + 1)));
    return r;
}
inline X pmuludq(X d, const X& s) { for (int i = 0; i < 2; i++) set64(d, i, (uint64_t)u32(d, 2 * i) * u32(s, 2 * i)); return d; }
inline X pcmpeqw(X d, const X& s) { for (int i = 0; i < 8; i++) set16(d, i, u16(d, i) == u16(s, i) ? 0xffff : 0); return d; }
inline X pcmpeqb(X d, const X& s) { for (int i = 0; i < 16; i++) d.b[i] = d.b[i] == s.b[i] ? 0xff : 0; return d; }
inline X pminsw(X d, const X& s) { for (int i = 0; i < 8; i++) set16(d, i, (int16_t)u16(d, i) < (int16_t)u16(s, i) ? u16(d, i) : u16(s, i)); return d; }
inline X pmaxsw(X d, const X& s) { for (int i = 0; i < 8; i++) set16(d, i, (int16_t)u16(d, i) > (int16_t)u16(s, i) ? u16(d, i) : u16(s, i)); return d; }
inline uint32_t pmovmskb(const X& s, int bytes) {
    uint32_t m = 0;
    for (int i = 0; i < bytes; i++) m |= (uint32_t)(s.b[i] >> 7) << i;
    return m;
}
inline X pshuflw(const X&, const X& s, int imm) {
    X r = s;
    for (int i = 0; i < 4; i++) set16(r, i, u16(s, imm >> (2 * i) & 3));
    return r;
}
inline X pshufhw(const X&, const X& s, int imm) {
    X r = s;
    for (int i = 0; i < 4; i++) set16(r, 4 + i, u16(s, 4 + (imm >> (2 * i) & 3)));
    return r;
}

// ---- MMX (64-bit) forms, through the SSE helpers on the low half ----
// Packs: d's elements then s's, which in 64 bits means both in one register.
inline uint64_t mmx_pack(X (*op)(const X&, const X&), uint64_t d, uint64_t s) {
    X x = from64(d);
    set64(x, 1, s);
    return u64(op(x, X{}), 0);
}
inline uint64_t pshufw(uint64_t s, int imm) {
    uint64_t r = 0;
    for (int i = 0; i < 4; i++) r |= (s >> (16 * (imm >> (2 * i) & 3)) & 0xffff) << (16 * i);
    return r;
}
inline uint64_t mmx(X (*op)(X, const X&), uint64_t d, uint64_t s) {
    return u64(op(from64(d), from64(s)), 0);
}
inline uint64_t mmx_shift(X (*op)(X, uint64_t), uint64_t d, uint64_t n) {
    return u64(op(from64(d), n), 0);
}
// The "high" unpacks interleave the upper halves of the 64-bit operands.
inline uint64_t mmx_punpckh(X (*low)(const X&, const X&), uint64_t d, uint64_t s) {
    return u64(low(from64(d >> 32), from64(s >> 32)), 0);
}
inline uint64_t mmx_punpckl(X (*low)(const X&, const X&), uint64_t d, uint64_t s) {
    return u64(low(from64(d), from64(s)), 0);
}
// cvtpi2ps: two int32 to the low two floats; the upper two are kept.
inline X cvtpi2ps(X d, uint64_t s) {
    set(d, 0, (float)(int32_t)(uint32_t)s);
    set(d, 1, (float)(int32_t)(uint32_t)(s >> 32));
    return d;
}

// cvt(t)ps2pi: the low two floats to two int32 in an MMX register.
inline uint64_t cvttps2pi(const X& s) {
    return (uint32_t)cvtt_i32(f32(s, 0)) | (uint64_t)(uint32_t)cvtt_i32(f32(s, 1)) << 32;
}
inline uint64_t cvtps2pi(const X& s) {
    return (uint32_t)cvt_i32(f32(s, 0)) | (uint64_t)(uint32_t)cvt_i32(f32(s, 1)) << 32;
}

// ---- MXCSR ----
// Applies MXCSR's rounding control and flush-to-zero to the host FPU.
void ldmxcsr(Context& c, uint32_t v);

}  // namespace scimitar::sse
