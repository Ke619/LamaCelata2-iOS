// Native replacements for hot ACB math functions (scimitar-recomp --override),
// found by profiling the render thread in Rome. The same rules as AC2's
// (games/ac2/native_math.cpp): each computes exactly what ACB's own x86
// computes, operation for operation, NaNs included, and leaves eax, ecx and
// edx as the original does. ACB's compiler laid some of them out differently
// from AC2's, so each is derived from ACB's code (addresses below are ACB's);
// tests/difftest/native_test.cpp (native_test_acb) checks them against the
// original run in Unicorn.
#include <cstdint>
#include <cstring>

#include "scimitar/runtime.h"
#include "scimitar/sse.h"

#pragma clang fp contract(off)

namespace {

using f4 = float __attribute__((ext_vector_type(4)));
using u4 = uint32_t __attribute__((ext_vector_type(4)));

// SSE operations, as in AT&T syntax `op src, dst` -> op(dst, src).
template <int imm> inline f4 shufps(f4 dst, f4 src) {
    return __builtin_shufflevector(dst, src, imm & 3, (imm >> 2) & 3, 4 + ((imm >> 4) & 3), 4 + ((imm >> 6) & 3));
}
inline f4 andps(f4 dst, f4 src) { return (f4)((u4)dst & (u4)src); }
inline f4 andnps(f4 dst, f4 src) { return (f4)(~(u4)dst & (u4)src); }
inline f4 orps(f4 dst, f4 src) { return (f4)((u4)dst | (u4)src); }
inline f4 unpckhps(f4 dst, f4 src) { return __builtin_shufflevector(dst, src, 2, 6, 3, 7); }

// addps/subps/mulps dst, src with x86's NaN results: NEON, and only when a lane
// is NaN the lanes redone with scimitar::sse's rules.
inline f4 fix(f4 d, f4 s, f4 r) {
    if (__builtin_expect(r.x == r.x && r.y == r.y && r.z == r.z && r.w == r.w, 1)) return r;
    for (int i = 0; i < 4; i++) r[i] = scimitar::sse::fix2<float>(d[i], s[i], r[i]);
    return r;
}
inline f4 add(f4 d, f4 s) { return fix(d, s, d + s); }
inline f4 sub(f4 d, f4 s) { return fix(d, s, d - s); }
inline f4 mul(f4 d, f4 s) { return fix(d, s, d * s); }
inline f4 splat(float v) { return f4{v, v, v, v}; }

inline f4 ld(scimitar::Context& c, uint32_t a) {
    f4 v;
    std::memcpy(&v, c.mem->ptr(a), 16);
    return v;
}
inline void st(scimitar::Context& c, uint32_t a, f4 v) { std::memcpy(c.mem->ptr(a), &v, 16); }
inline float ldf(scimitar::Context& c, uint32_t a) { return c.mem->read<float>(a); }

// cvttss2si: truncation; out of range or NaN gives 0x80000000.
inline int32_t cvtt(float f) {
    return f >= -2147483648.0f && f < 2147483648.0f ? (int32_t)f : INT32_MIN;
}

// The two-step horizontal sum the engine's dot products use:
// (m0 + m2) + (m1 + m3).
inline float hsum(f4 m) {
    const f4 s = add(f4{m.x, m.y, 0, 0}, f4{m.z, m.w, 0, 0});
    return add(splat(s.x), splat(s.y)).x;
}

}  // namespace

namespace scimitar::overrides {

using scimitar::arg32;
using scimitar::shim_return;

// int scimitar::CullPlaneAABBoxRes(const Vector4& plane, const Vector4& min,
// const Vector4& max) at 0x001834c0: as in AC2. The box's nearest corner to
// the plane (min where the normal is positive, max where negative, w the 1.0
// at 0x0225a590), then its farthest. 2 if the nearest is in front (d > 0),
// else 3 if the farthest is, else 1. ecx and edx keep min and max.
void ov__ZN8scimitar18CullPlaneAABBoxResERKN4Gear7Vector4IfEES4_S4_(scimitar::Context& c) {
    const f4 p = ld(c, arg32(c, 0)), lo = ld(c, arg32(c, 1)), hi = ld(c, arg32(c, 2));
    const float one = ldf(c, 0x0225a590);
    const f4 near = {0.0f > p.x ? hi.x : lo.x, 0.0f > p.y ? hi.y : lo.y, 0.0f > p.z ? hi.z : lo.z, one};
    const f4 far = {0.0f > p.x ? lo.x : hi.x, 0.0f > p.y ? lo.y : hi.y, 0.0f > p.z ? lo.z : hi.z, one};
    const float d1 = hsum(mul(near, p)), d2 = hsum(mul(far, p));
    c.ecx = arg32(c, 1), c.edx = arg32(c, 2);
    shim_return(c, d1 > 0.0f ? 2 : d2 > 0.0f ? 3 : 1);
}

// Gear::Matrix44<float>& Gear::Matrix44<float>::operator*=(const Matrix44& b) at
// 0x00b48b70: as in AC2. this = b * this, row i = ((b.x a0 + b.y a1) + b.z a2)
// + b.w a3, every row computed before any store (safe when they alias).
void ov__ZN4Gear8Matrix44IfEmLERKS1_(scimitar::Context& c) {
    const uint32_t ta = arg32(c, 0), tb = arg32(c, 1);
    const f4 a0 = ld(c, ta), a1 = ld(c, ta + 16), a2 = ld(c, ta + 32), a3 = ld(c, ta + 48);
    f4 r[4];
    for (int i = 0; i < 4; i++) {
        const f4 b = ld(c, tb + 16 * i);
        r[i] = add(mul(splat(b.w), a3), add(mul(splat(b.z), a2), add(mul(splat(b.y), a1), mul(splat(b.x), a0))));
    }
    for (int i = 0; i < 4; i++) st(c, ta + 16 * i, r[i]);
    c.edx = tb;
    shim_return(c, ta);
}

// void Gear::Quaternion4<float>::ToMatrix(Matrix44& m) const at 0x00b52110:
// ACB's SSE sequence, instruction for instruction (registers named as
// there). Masks through the pointers at 0x02c67a70, 0x02c6a984 and
// 0x02c6a950; constants at 0x022f6120 and 0x022f6110. eax ends as the last
// mask pointer, edx as m.
void ov__ZNK4Gear11Quaternion4IfE8ToMatrixERNS_8Matrix44IfEE(scimitar::Context& c) {
    const uint32_t m = arg32(c, 1);
    f4 xmm0 = ld(c, arg32(c, 0));
    f4 xmm2 = add(xmm0, xmm0);
    f4 xmm1 = mul(xmm0, xmm2);
    f4 xmm6 = ld(c, 0x022f6120);
    f4 xmm3 = shufps<0x01>(xmm1, xmm1);
    xmm1 = shufps<0x1a>(xmm1, xmm1);
    xmm6 = sub(xmm6, xmm3);
    xmm6 = sub(xmm6, xmm1);
    xmm1 = shufps<0xd0>(xmm0, xmm0);
    xmm3 = shufps<0xe6>(xmm2, xmm2);
    xmm1 = mul(xmm1, xmm3);
    xmm0 = shufps<0xff>(xmm0, xmm0);
    xmm2 = shufps<0xc9>(xmm2, xmm2);
    xmm0 = mul(xmm0, xmm2);
    xmm3 = add(xmm1, xmm0);
    f4 xmm4 = sub(xmm1, xmm0);
    xmm6 = shufps<0xa4>(xmm6, xmm6);
    f4 xmm5 = ld(c, c.mem->read<uint32_t>(0x02c67a70));
    xmm5 = andnps(xmm5, xmm6);
    xmm0 = f4{0, 0, 0, 0};
    xmm5 = orps(xmm5, xmm0);
    const f4 xmm7 = shufps<0xd0>(xmm5, xmm5);
    xmm6 = shufps<0x81>(xmm3, xmm3);
    xmm2 = shufps<0x4e>(xmm4, xmm4);
    xmm3 = shufps<0x4e>(xmm3, xmm3);
    xmm0 = ld(c, c.mem->read<uint32_t>(0x02c6a984));
    xmm1 = andnps(xmm0, xmm2);
    xmm2 = xmm1;
    xmm3 = andps(xmm3, xmm0);
    xmm2 = orps(xmm2, xmm3);
    xmm6 = shufps<0xd0>(xmm6, xmm6);
    xmm4 = shufps<0xd0>(xmm4, xmm4);
    const uint32_t mask3 = c.mem->read<uint32_t>(0x02c6a950);
    xmm3 = ld(c, mask3);
    xmm0 = andnps(xmm3, xmm6);
    xmm4 = andps(xmm4, xmm3);
    xmm0 = orps(xmm0, xmm4);
    xmm2 = shufps<0x22>(xmm2, xmm2);
    xmm1 = ld(c, 0x022f6110);
    st(c, m + 0x30, xmm1);
    xmm2 = shufps<0xe4>(xmm2, xmm5);
    st(c, m + 0x20, xmm2);
    xmm2 = unpckhps(xmm0, xmm7);
    st(c, m + 0x10, xmm2);
    xmm0 = shufps<0xd0>(xmm0, xmm0);
    xmm2 = ld(c, mask3);
    xmm1 = andnps(xmm2, xmm7);
    xmm0 = andps(xmm0, xmm2);
    xmm1 = orps(xmm1, xmm0);
    st(c, m, xmm1);
    c.edx = m;
    shim_return(c, mask3);
}

// bool scimitar::IsConstantEqual<Gear::Vector4<float>>(const Vector4&, const
// Vector4&) at 0x00b2d110: the four floats compared as bits. Unlike AC2's,
// ACB's leaves edx alone.
void ov__ZN8scimitar15IsConstantEqualIN4Gear7Vector4IfEEEEbRKT_S6_(scimitar::Context& c) {
    const bool same = std::memcmp(c.mem->ptr(arg32(c, 0)), c.mem->ptr(arg32(c, 1)), 16) == 0;
    shim_return(c, same ? 1 : 0);
}

// void scimitar::IColor::ConvertFromFloat(const Vector4& v) at 0x00ab9090:
// AC2's code at ACB's constants: each component clamped (above the constant
// at 0x022f6790 -> it, below 0 -> 0, NaN kept), scaled by the float at
// 0x022f7fc4, truncated to its low byte. edx: self; eax: the last
// conversion.
void ov__ZN8scimitar6IColor16ConvertFromFloatERKN4Gear7Vector4IfEE(scimitar::Context& c) {
    const uint32_t self = arg32(c, 0);
    const f4 v = ld(c, arg32(c, 1));
    const f4 hi = ld(c, 0x022f6790);
    const float k = ldf(c, 0x022f7fc4);
    int32_t last = 0;
    for (int i = 0; i < 4; i++) {
        float x = hi[i] < v[i] ? hi[i] : v[i];
        if (v[i] < 0.0f) x = 0.0f;
        last = cvtt(x * k);
        c.mem->write<uint8_t>(self + i, (uint8_t)last);
    }
    c.edx = self;
    shim_return(c, (uint32_t)last);
}

}  // namespace scimitar::overrides
