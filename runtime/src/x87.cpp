// Exact x87 extended-precision arithmetic (see scimitar/x87.h).
//
// Values are unpacked to sign, exponent and a 128-bit significand normalised
// with its leading one at bit 126 (one bit of headroom for additions), so the
// value is sig * 2^(exp - 126). That leaves 63 bits below a 64-bit mantissa,
// enough to round every operation correctly with a sticky bit.
#include "scimitar/x87.h"

#include <cmath>
#include <cstring>
#include <utility>

namespace scimitar::x87 {

namespace {

bool g_qemu = false;

#if defined(__SIZEOF_INT128__) && !defined(SCIMITAR_PORTABLE_U128)
using u128 = unsigned __int128;
#else
// 32-bit hosts (the Vita's ARMv7) have no __int128: the same arithmetic on
// two 64-bit halves. Only what this file uses; conversions out are explicit.
struct u128 {
    uint64_t lo = 0, hi = 0;
    constexpr u128() = default;
    constexpr u128(uint64_t v) : lo(v) {}
    constexpr u128(uint64_t h, uint64_t l) : lo(l), hi(h) {}
    constexpr explicit operator uint64_t() const { return lo; }
    constexpr explicit operator bool() const { return (lo | hi) != 0; }

    friend constexpr u128 operator<<(u128 v, int n) {
        if (n <= 0) return v;
        if (n >= 128) return {};
        if (n >= 64) return {v.lo << (n - 64), 0};
        return {v.hi << n | v.lo >> (64 - n), v.lo << n};
    }
    friend constexpr u128 operator>>(u128 v, int n) {
        if (n <= 0) return v;
        if (n >= 128) return {};
        if (n >= 64) return {0, v.hi >> (n - 64)};
        return {v.hi >> n, v.lo >> n | v.hi << (64 - n)};
    }
    friend constexpr u128 operator&(u128 a, u128 b) { return {a.hi & b.hi, a.lo & b.lo}; }
    friend constexpr u128 operator|(u128 a, u128 b) { return {a.hi | b.hi, a.lo | b.lo}; }
    friend constexpr u128 operator~(u128 a) { return {~a.hi, ~a.lo}; }
    friend constexpr u128 operator+(u128 a, u128 b) {
        const uint64_t lo = a.lo + b.lo;
        return {a.hi + b.hi + (lo < a.lo), lo};
    }
    friend constexpr u128 operator-(u128 a, u128 b) { return {a.hi - b.hi - (a.lo < b.lo), a.lo - b.lo}; }
    // Products of 64-bit values (what this file multiplies), low 128 bits otherwise.
    friend constexpr u128 operator*(u128 a, u128 b) {
        const uint64_t a0 = (uint32_t)a.lo, a1 = a.lo >> 32, b0 = (uint32_t)b.lo, b1 = b.lo >> 32;
        const uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
        const uint64_t mid = (p00 >> 32) + (uint32_t)p01 + (uint32_t)p10;
        u128 r{p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32), (mid << 32) | (uint32_t)p00};
        r.hi += a.lo * b.hi + a.hi * b.lo;
        return r;
    }
    friend constexpr bool operator==(u128 a, u128 b) { return a.lo == b.lo && a.hi == b.hi; }
    friend constexpr bool operator!=(u128 a, u128 b) { return !(a == b); }
    friend constexpr bool operator<(u128 a, u128 b) { return a.hi != b.hi ? a.hi < b.hi : a.lo < b.lo; }
    friend constexpr bool operator>(u128 a, u128 b) { return b < a; }
    friend constexpr bool operator<=(u128 a, u128 b) { return !(b < a); }
    friend constexpr bool operator>=(u128 a, u128 b) { return !(a < b); }
    // Bit by bit (division is the rare fdiv path).
    static constexpr void divmod(u128 n, u128 d, u128& q, u128& r) {
        q = {}, r = {};
        for (int i = 127; i >= 0; i--) {
            r = r << 1 | (n >> i & 1);
            if (r >= d) r = r - d, q = q | (u128{1} << i);
        }
    }
    friend constexpr u128 operator/(u128 n, u128 d) { u128 q, r; divmod(n, d, q, r); return q; }
    friend constexpr u128 operator%(u128 n, u128 d) { u128 q, r; divmod(n, d, q, r); return r; }
    u128& operator<<=(int n) { return *this = *this << n; }
    u128& operator>>=(int n) { return *this = *this >> n; }
    u128 operator++(int) { const u128 old = *this; *this = *this + 1; return old; }
};
#endif

enum Class { kZeroC, kFinite, kInf, kNaN };

struct U {
    bool sign = false;
    Class cls = kZeroC;
    int32_t exp = 0;
    u128 sig = 0;
    uint64_t nan_mant = 0;  // NaN significand, for propagation
};

constexpr int kTop = 126;

int clz128(u128 v) {
    const uint64_t hi = (uint64_t)(v >> 64), lo = (uint64_t)v;
    return hi ? __builtin_clzll(hi) : 64 + __builtin_clzll(lo);
}

u128 shift_right_sticky(u128 v, int n) {
    if (n <= 0) return v;
    if (n >= 128) return v != 0;
    const u128 lost = v & (((u128)1 << n) - 1);
    return (v >> n) | (lost != 0);
}

void normalize(U& u) {
    const int lz = clz128(u.sig);
    const int shift = lz - (127 - kTop);
    if (shift > 0) u.sig <<= shift;
    else if (shift < 0) u.sig = shift_right_sticky(u.sig, -shift);
    u.exp -= shift;
}

U unpack(F80 v) {
    U u;
    u.sign = v.se >> 15;
    const int e = v.se & 0x7fff;
    if (e == 0x7fff) {
        if ((v.mant << 1) == 0) u.cls = kInf;
        else {
            u.cls = kNaN;
            u.nan_mant = v.mant;
        }
        return u;
    }
    if (v.mant == 0) return u;  // zero (also a pseudo-denormal zero)
    if (e != 0 && !(v.mant >> 63)) {  // unnormal: invalid operand on a 387 and later
        u.cls = kNaN;
        u.nan_mant = kIndefinite.mant;
        u.sign = true;
        return u;
    }
    u.cls = kFinite;
    u.sig = (u128)v.mant << 63;  // bit 63 of mant -> bit 126
    u.exp = e == 0 ? -16382 : e - 16383;
    normalize(u);
    return u;
}

int rounding(const X87& s) { return s.cw >> 10 & 3; }  // 0 nearest, 1 down, 2 up, 3 zero

int precision(const X87& s) {
    switch (s.cw >> 8 & 3) {
    case 0: return 24;
    case 2: return 53;
    default: return 64;
    }
}

// Rounds the finite value `u` to `prec` significant bits with exponent range
// [emin, emax] (denormals below emin). On return, `kept` holds the rounded
// significand (`prec` bits, leading one at bit prec-1 unless denormal), and
// `exp` its exponent; `overflow` is set when the result exceeds emax.
struct Rounded {
    uint64_t kept = 0;
    int32_t exp = 0;
    bool overflow = false, denormal = false;
};

Rounded round(const U& u, int prec, int32_t emin, int32_t emax, int rc) {
    Rounded r;
    u128 sig = u.sig;
    int32_t exp = u.exp;
    if (exp < emin) {
        sig = shift_right_sticky(sig, emin - exp);
        exp = emin;
    }
    const int drop = kTop + 1 - prec;
    u128 kept = sig >> drop;
    const u128 rest = sig & (((u128)1 << drop) - 1);
    const u128 half = (u128)1 << (drop - 1);
    bool inc = false;
    switch (rc) {
    case 0: inc = rest > half || (rest == half && (kept & 1)); break;
    case 1: inc = u.sign && rest != 0; break;
    case 2: inc = !u.sign && rest != 0; break;
    default: break;
    }
    if (inc) {
        kept++;
        if (kept >> prec) {  // carried out: 1.111.. rounded up to 10.000..
            kept >>= 1;
            exp++;
        }
    }
    r.kept = (uint64_t)kept;
    r.exp = exp;
    r.denormal = !(r.kept >> (prec - 1));
    r.overflow = exp > emax;
    return r;
}

// Rounding an overflowing result: infinity, or the largest finite value when
// the rounding direction is toward zero for this sign.
bool overflow_to_inf(bool sign, int rc) {
    return rc == 0 || (rc == 1 && sign) || (rc == 2 && !sign);
}

F80 pack(const U& u, int prec, int rc) {
    const uint16_t sign = u.sign ? 0x8000 : 0;
    switch (u.cls) {
    case kZeroC: return {0, sign};
    case kInf: return {0x8000000000000000ull, (uint16_t)(sign | 0x7fff)};
    case kNaN: return {u.nan_mant | 0x4000000000000000ull, (uint16_t)(sign | 0x7fff)};
    default: break;
    }
    const Rounded r = round(u, prec, -16382, 16383, rc);
    if (r.overflow) {
        if (overflow_to_inf(u.sign, rc)) return {0x8000000000000000ull, (uint16_t)(sign | 0x7fff)};
        const uint64_t max = prec == 64 ? ~0ull : (~0ull << (64 - prec));
        return {max, (uint16_t)(sign | 0x7ffe)};
    }
    const uint64_t mant = r.kept << (64 - prec);
    if (mant == 0) return {0, sign};
    if (!(mant >> 63)) return {mant, sign};  // denormal
    return {mant, (uint16_t)(sign | (r.exp + 16383))};
}

// x87 NaN propagation: the operand that is a NaN (quieted); of two NaNs, the
// one with the larger significand.
F80 propagate(const U& a, const U& b) {
    const uint64_t q = 0x4000000000000000ull;
    if (a.cls == kNaN && b.cls == kNaN) {
        const U& w = (a.nan_mant | q) >= (b.nan_mant | q) ? a : b;
        return {w.nan_mant | q, (uint16_t)((w.sign ? 0x8000 : 0) | 0x7fff)};
    }
    const U& w = a.cls == kNaN ? a : b;
    return {w.nan_mant | q, (uint16_t)((w.sign ? 0x8000 : 0) | 0x7fff)};
}

uint64_t pack_ieee(const U& u, int prec, int ebits, int rc) {
    const int bias = (1 << (ebits - 1)) - 1;
    const uint64_t sign = (uint64_t)u.sign << (prec + ebits - 1);
    const uint64_t inf = ((uint64_t)((1 << ebits) - 1)) << (prec - 1);
    switch (u.cls) {
    case kZeroC: return sign;
    case kInf: return sign | inf;
    case kNaN: {  // quieted (QEMU keeps a signalling NaN signalling)
        const uint64_t frac = (u.nan_mant << 1) >> (64 - (prec - 1));
        return sign | inf | frac | (g_qemu ? 0 : 1ull << (prec - 2));
    }
    default: break;
    }
    const Rounded r = round(u, prec, 1 - bias, bias, rc);
    if (r.overflow) {
        if (overflow_to_inf(u.sign, rc)) return sign | inf;
        return sign | (inf - 1);  // largest finite
    }
    const uint64_t frac = r.kept & ((1ull << (prec - 1)) - 1);
    if (r.denormal) return sign | frac;
    return sign | ((uint64_t)(r.exp + bias) << (prec - 1)) | frac;
}

U from_ieee(uint64_t bits, int prec, int ebits) {
    U u;
    const int bias = (1 << (ebits - 1)) - 1;
    u.sign = bits >> (prec + ebits - 1) & 1;
    const int e = (int)(bits >> (prec - 1) & ((1u << ebits) - 1));
    const uint64_t frac = bits & ((1ull << (prec - 1)) - 1);
    if (e == (1 << ebits) - 1) {
        if (frac == 0) u.cls = kInf;
        else {
            u.cls = kNaN;  // loading quiets a signalling NaN (QEMU: keeps it)
            u.nan_mant = (g_qemu ? 0x8000000000000000ull : 0xc000000000000000ull) |
                         (frac << (64 - prec));
        }
        return u;
    }
    if (e == 0 && frac == 0) return u;
    u.cls = kFinite;
    const uint64_t m = e == 0 ? frac : frac | (1ull << (prec - 1));
    u.sig = (u128)m << (kTop - (prec - 1));
    u.exp = e == 0 ? 1 - bias : e - bias;
    normalize(u);
    return u;
}

F80 exact(const U& u) {
    if (u.cls == kNaN && g_qemu)  // keep the significand as loaded
        return {u.nan_mant, (uint16_t)((u.sign ? 0x8000 : 0) | 0x7fff)};
    return pack(u, 64, 0);
}

// ---- fast paths ----
// Most operands in game code are normal (biased exponent 1..0x7ffe with the
// explicit leading one) and so are the results: for them the same exact
// arithmetic and rounding run on the packed form, without unpacking to U.
// Anything else (zeros where the sign rules matter, denormals, infinities,
// NaNs, results out of the normal range) takes the general code below, so the
// results are the same bit for bit.
bool g_fast = true;  // tests compare the two

inline bool is_normal(F80 v) {
    const unsigned e = v.se & 0x7fffu;
    return e - 1u < 0x7ffeu && (v.mant >> 63);
}
inline bool is_zero(F80 v) { return v.mant == 0 && (v.se & 0x7fff) != 0x7fff; }

template <typename T> inline bool round_up(int rc, bool sign, T rest, T half, bool odd) {
    switch (rc) {
    case 0: return rest > half || (rest == half && odd);
    case 1: return sign && rest != 0;
    case 2: return !sign && rest != 0;
    default: return false;
    }
}

// `p` (leading one at bit 127) with biased exponent `e`, rounded to `prec`
// bits; false when the result leaves the normal range. In 64-bit halves:
// the kept bits are all in the high half (prec <= 64).
inline bool round_normal(u128 p, int32_t e, bool sign, int prec, int rc, F80* out) {
    const uint64_t hi = (uint64_t)(p >> 64), lo = (uint64_t)p;
    const int dh = 64 - prec;  // bits of the high half below the kept ones
    uint64_t kept = hi >> dh;
    bool up;
    if (dh == 0) {
        up = round_up(rc, sign, lo, 1ull << 63, kept & 1);
    } else {
        const uint64_t rest = hi & ((1ull << dh) - 1), half = 1ull << (dh - 1);
        switch (rc) {
        case 0: up = rest > half || (rest == half && (lo != 0 || (kept & 1))); break;
        case 1: up = sign && (rest | lo) != 0; break;
        case 2: up = !sign && (rest | lo) != 0; break;
        default: up = false; break;
        }
    }
    if (up && ++kept == (prec == 64 ? 0 : 1ull << prec)) {  // carried out: 1.11..1 rounded up to 10.00..0
        kept = 1ull << (prec - 1);
        e++;
    }
    if (e < 1 || e > 0x7ffe) return false;
    out->mant = kept << dh;
    out->se = (uint16_t)((sign ? 0x8000 : 0) | e);
    return true;
}

inline bool fast_mul(const X87& s, F80 a, F80 b, F80* out) {
    if (!is_normal(a) || !is_normal(b)) return false;
    u128 p = (u128)a.mant * b.mant;
    int32_t e = (int32_t)(a.se & 0x7fff) + (int32_t)(b.se & 0x7fff) - 16383;
    if (p >> 127) e++;
    else p <<= 1;
    return round_normal(p, e, (a.se ^ b.se) >> 15, precision(s), rounding(s), out);
}

inline bool fast_add(const X87& s, F80 a, F80 b, F80* out) {
    if (!is_normal(a) || !is_normal(b)) return false;
    int32_t ea = a.se & 0x7fff, eb = b.se & 0x7fff;
    if (ea < eb || (ea == eb && a.mant < b.mant)) {
        std::swap(a, b);
        std::swap(ea, eb);
    }
    const u128 A = (u128)a.mant << 63;  // leading one at bit 126
    const u128 B = shift_right_sticky((u128)b.mant << 63, ea - eb);
    const bool sign = a.se >> 15;
    u128 r;
    if ((a.se ^ b.se) >> 15) {
        r = A - B;
        if (r == 0) return false;  // exact zero: its sign depends on the rounding
    } else {
        r = A + B;
    }
    const int lz = clz128(r);
    return round_normal(r << lz, ea + 1 - lz, sign, precision(s), rounding(s), out);
}

inline bool fast_from_f32(uint32_t bits, F80* out) {
    const uint32_t e = bits >> 23 & 0xff, f = bits & 0x7fffff;
    const uint16_t sign = (uint16_t)(bits >> 16 & 0x8000);
    if (e == 0 && f == 0) return *out = {0, sign}, true;
    if (e == 0 || e == 0xff) return false;
    *out = {0x8000000000000000ull | (uint64_t)f << 40, (uint16_t)(sign | (e - 127 + 16383))};
    return true;
}

inline bool fast_to_f32(const X87& s, F80 v, uint32_t* out) {
    const bool sign = v.se >> 15;
    if (is_zero(v)) return *out = (uint32_t)sign << 31, true;
    if (!is_normal(v)) return false;
    int32_t e = (int32_t)(v.se & 0x7fff) - 16383;
    if (e < -126 || e > 127) return false;
    uint64_t kept = v.mant >> 40;
    const uint64_t rest = v.mant & ((1ull << 40) - 1);
    if (round_up(rounding(s), sign, rest, 1ull << 39, kept & 1)) {
        kept++;
        if (kept >> 24) {
            kept >>= 1;
            e++;
        }
    }
    if (e > 127) return false;
    *out = (uint32_t)sign << 31 | (uint32_t)(e + 127) << 23 | (uint32_t)(kept & 0x7fffff);
    return true;
}

inline bool fast_to_int(const X87& s, F80 v, int bits, bool truncate, int64_t* out) {
    if (is_zero(v)) return *out = 0, true;
    if (!is_normal(v)) return false;
    const int32_t e = (int32_t)(v.se & 0x7fff) - 16383;
    if (e < -64 || e > 62) return false;
    const bool sign = v.se >> 15;
    const int shift = 63 - e;  // 1..127
    u128 ip = (u128)v.mant >> shift;
    const u128 rest = (u128)v.mant & (((u128)1 << shift) - 1), half = (u128)1 << (shift - 1);
    if (round_up(truncate ? 3 : rounding(s), sign, rest, half, (ip & 1) != 0)) ip++;
    const u128 limit = (u128)1 << (bits - 1);
    if (sign ? ip > limit : ip >= limit) return false;
    *out = (int64_t)(sign ? 0 - (uint64_t)ip : (uint64_t)ip);
    return true;
}

inline bool fast_compare(F80 a, F80 b, int* out) {
    const bool za = is_zero(a), zb = is_zero(b);
    if ((!za && !is_normal(a)) || (!zb && !is_normal(b))) return false;
    if (za && zb) return *out = 0, true;
    const bool sa = !za && (a.se >> 15), sb = !zb && (b.se >> 15);
    if (sa != sb) return *out = sa ? -1 : 1, true;
    int m;
    if (za || zb) m = za ? -1 : 1;
    else {
        const unsigned ea = a.se & 0x7fff, eb = b.se & 0x7fff;
        m = ea != eb ? (ea < eb ? -1 : 1) : a.mant == b.mant ? 0 : a.mant < b.mant ? -1 : 1;
    }
    *out = sa ? -m : m;
    return true;
}

}  // namespace


void set_qemu_compat(bool on) { g_qemu = on; }

F80 from_f32(uint32_t bits) {
    if (F80 r; g_fast && fast_from_f32(bits, &r)) return r;
    return exact(from_ieee(bits, 24, 8));
}
F80 from_f64(uint64_t bits) { return exact(from_ieee(bits, 53, 11)); }

F80 from_int(int64_t v) {
    U u;
    if (v == 0) return exact(u);
    u.cls = kFinite;
    u.sign = v < 0;
    const uint64_t m = u.sign ? 0 - (uint64_t)v : (uint64_t)v;
    u.sig = (u128)m << (kTop - 63);
    u.exp = 63;
    normalize(u);
    return exact(u);
}

uint32_t to_f32(const X87& s, F80 v) {
    if (uint32_t r; g_fast && fast_to_f32(s, v, &r)) return r;
    return (uint32_t)pack_ieee(unpack(v), 24, 8, rounding(s));
}
uint64_t to_f64(const X87& s, F80 v) { return pack_ieee(unpack(v), 53, 11, rounding(s)); }

int64_t to_int(const X87& s, F80 v, int bits, bool truncate) {
    if (int64_t r; g_fast && fast_to_int(s, v, bits, truncate, &r)) return r;
    const int64_t indefinite = (int64_t)(~0ull << (bits - 1));  // e.g. 0x8000 sign-extended
    const U u = unpack(v);
    // QEMU's 32- and 64-bit fisttp saturate positive overflow (and NaN) to the
    // maximum; its 16-bit form stores the indefinite like the hardware.
    const int64_t positive_overflow = g_qemu && truncate && bits >= 32 ? ~indefinite : indefinite;
    if (u.cls == kZeroC) return 0;
    if (u.cls == kNaN) return positive_overflow;
    if (u.cls == kInf) return u.sign ? indefinite : positive_overflow;
    if (u.exp >= 64) return u.sign ? indefinite : positive_overflow;
    // Integer part and the bits below it.
    u128 ip, rest, half;
    const int shift = kTop - u.exp;  // >= 63
    if (shift >= 128) {  // |value| < 0.5
        ip = 0;
        rest = u.sig;
        half = ~(u128)0;
    } else {
        ip = u.sig >> shift;
        rest = u.sig & (((u128)1 << shift) - 1);
        half = (u128)1 << (shift - 1);
    }
    const int rc = truncate ? 3 : rounding(s);
    bool inc = false;
    switch (rc) {
    case 0: inc = rest > half || (rest == half && (ip & 1)); break;
    case 1: inc = u.sign && rest != 0; break;
    case 2: inc = !u.sign && rest != 0; break;
    default: break;
    }
    if (inc) ip++;
    const u128 limit = (u128)1 << (bits - 1);  // |min|
    if (u.sign ? ip > limit : ip >= limit) return u.sign ? indefinite : positive_overflow;
    return (int64_t)(u.sign ? 0 - (uint64_t)ip : (uint64_t)ip);
}

F80 add(const X87& s, F80 fa, F80 fb) {
    if (F80 r; g_fast && fast_add(s, fa, fb, &r)) return r;
    U a = unpack(fa), b = unpack(fb);
    const int rc = rounding(s);
    if (a.cls == kNaN || b.cls == kNaN) return propagate(a, b);
    if (a.cls == kInf || b.cls == kInf) {
        if (a.cls == kInf && b.cls == kInf && a.sign != b.sign) return kIndefinite;
        return exact(a.cls == kInf ? a : b);
    }
    if (a.cls == kZeroC && b.cls == kZeroC) {
        U z;
        z.sign = a.sign == b.sign ? a.sign : rc == 1;
        return exact(z);
    }
    if (a.cls == kZeroC) return pack(b, precision(s), rc);
    if (b.cls == kZeroC) return pack(a, precision(s), rc);
    if (a.exp < b.exp || (a.exp == b.exp && a.sig < b.sig)) std::swap(a, b);
    b.sig = shift_right_sticky(b.sig, a.exp - b.exp);
    U r;
    r.cls = kFinite;
    r.sign = a.sign;
    r.exp = a.exp;
    if (a.sign == b.sign) {
        r.sig = a.sig + b.sig;
    } else {
        r.sig = a.sig - b.sig;
        if (r.sig == 0) {
            U z;
            z.sign = rc == 1;
            return exact(z);
        }
    }
    normalize(r);
    return pack(r, precision(s), rc);
}

F80 sub(const X87& s, F80 a, F80 b) {
    const U ua = unpack(a), ub = unpack(b);
    if (ua.cls == kNaN || ub.cls == kNaN) return propagate(ua, ub);  // signs unchanged
    return add(s, a, chs(b));
}

F80 mul(const X87& s, F80 fa, F80 fb) {
    if (F80 r; g_fast && fast_mul(s, fa, fb, &r)) return r;
    const U a = unpack(fa), b = unpack(fb);
    if (a.cls == kNaN || b.cls == kNaN) return propagate(a, b);
    U r;
    r.sign = a.sign != b.sign;
    if (a.cls == kInf || b.cls == kInf) {
        if (a.cls == kZeroC || b.cls == kZeroC) return kIndefinite;
        r.cls = kInf;
        return exact(r);
    }
    if (a.cls == kZeroC || b.cls == kZeroC) return exact(r);
    // 64-bit significands (leading one at bit 63) multiply exactly into 128 bits.
    const uint64_t ma = (uint64_t)(a.sig >> 63), mb = (uint64_t)(b.sig >> 63);
    r.cls = kFinite;
    r.sig = (u128)ma * mb;  // value = sig * 2^(ea + eb - 126)
    r.exp = a.exp + b.exp;
    normalize(r);
    return pack(r, precision(s), rounding(s));
}

F80 div(const X87& s, F80 fa, F80 fb) {
    const U a = unpack(fa), b = unpack(fb);
    if (a.cls == kNaN || b.cls == kNaN) return propagate(a, b);
    U r;
    r.sign = a.sign != b.sign;
    if (a.cls == kInf) {
        if (b.cls == kInf) return kIndefinite;
        r.cls = kInf;
        return exact(r);
    }
    if (b.cls == kInf) return exact(r);  // zero
    if (b.cls == kZeroC) {
        if (a.cls == kZeroC) return kIndefinite;
        r.cls = kInf;  // division by zero (masked)
        return exact(r);
    }
    if (a.cls == kZeroC) return exact(r);
    const uint64_t ma = (uint64_t)(a.sig >> 63), mb = (uint64_t)(b.sig >> 63);
    // Q = floor(ma * 2^k / mb) in [2^126, 2^127), by two 128/64 divisions.
    const int k = ma >= mb ? 126 : 127;
    const u128 n1 = (u128)ma << (k - 64);
    const u128 q1 = n1 / mb, r1 = n1 % mb;
    const u128 n2 = r1 << 64;
    const u128 q2 = n2 / mb, r2 = n2 % mb;
    r.cls = kFinite;
    r.sig = (q1 << 64) | q2 | (r2 != 0);
    r.exp = a.exp - b.exp - (k - 126);
    normalize(r);
    return pack(r, precision(s), rounding(s));
}

namespace {

double to_double(F80 v) {
    const uint64_t b = pack_ieee(unpack(v), 53, 11, 0);
    double d;
    std::memcpy(&d, &b, 8);
    return d;
}

F80 from_double(double d) {
    uint64_t b;
    std::memcpy(&b, &d, 8);
    return from_f64(b);
}

}  // namespace

int compare(F80 fa, F80 fb) {
    if (int r; g_fast && fast_compare(fa, fb, &r)) return r;
    const U a = unpack(fa), b = unpack(fb);
    if (a.cls == kNaN || b.cls == kNaN) return 2;
    auto key = [](const U& u, int* cls) { *cls = u.cls == kZeroC ? 0 : u.cls == kInf ? 2 : 1; };
    int ca, cb;
    key(a, &ca);
    key(b, &cb);
    if (ca == 0 && cb == 0) return 0;  // +0 == -0
    // Magnitude order, then sign.
    auto mag = [](const U& u, int cls, const U& v, int vcls) {
        if (cls != vcls) return cls < vcls ? -1 : 1;
        if (cls != 1) return 0;
        if (u.exp != v.exp) return u.exp < v.exp ? -1 : 1;
        return u.sig == v.sig ? 0 : u.sig < v.sig ? -1 : 1;
    };
    const bool sa = ca != 0 && a.sign, sb = cb != 0 && b.sign;
    if (sa != sb) return sa ? -1 : 1;
    const int m = mag(a, ca, b, cb);
    return sa ? -m : m;
}

void examine(X87& s) {
    const uint32_t p = s.top & 7;
    uint16_t cc;
    const F80 v = s.r[p];
    const U u = unpack(v);
    if (!(s.valid >> p & 1)) cc = 0x4100;                                   // empty
    else if (u.cls == kNaN) cc = 0x0100;                                     // NaN
    else if (u.cls == kInf) cc = 0x0500;                                     // infinity
    else if (u.cls == kZeroC) cc = 0x4000;                                   // zero
    else if ((v.se & 0x7fff) == 0) cc = 0x4400;                              // denormal
    else cc = 0x0400;                                                        // normal
    if (v.se & 0x8000) cc |= 0x0200;
    s.sw = (uint16_t)((s.sw & ~0x4700u) | cc);
}

F80 sqrt(const X87& s, F80 a) {
    (void)s;
    const U u = unpack(a);
    if (u.cls == kZeroC || (u.cls == kInf && !u.sign)) return a;
    if (u.cls == kNaN) return exact(u);
    if (u.sign) return kIndefinite;
    return from_double(std::sqrt(to_double(a)));
}

F80 rndint(const X87& s, F80 a) {
    const U u = unpack(a);
    if (u.cls != kFinite || u.exp >= 63) return a;
    u128 ip, rest, half;
    const int shift = kTop - u.exp;
    if (shift >= 128) {
        ip = 0, rest = u.sig, half = ~(u128)0;
    } else {
        ip = u.sig >> shift;
        rest = u.sig & (((u128)1 << shift) - 1);
        half = (u128)1 << (shift - 1);
    }
    bool inc = false;
    switch (rounding(s)) {
    case 0: inc = rest > half || (rest == half && (ip & 1)); break;
    case 1: inc = u.sign && rest != 0; break;
    case 2: inc = !u.sign && rest != 0; break;
    default: break;
    }
    if (inc) ip++;
    F80 r = from_int((int64_t)(uint64_t)ip);
    if (u.sign) r.se |= 0x8000;  // keeps -0 for small negatives, as the hardware does
    return r;
}

F80 scale(F80 a, F80 b) {
    U u = unpack(a);
    const double n = std::trunc(to_double(b));
    if (u.cls != kFinite) return a;
    if (std::isnan(n)) return kIndefinite;
    const double c = std::fmax(-70000.0, std::fmin(70000.0, n));
    u.exp += (int32_t)c;
    return pack(u, 64, 0);
}

F80 prem(X87& s, F80 a, F80 b, bool ieee) {
    const double x = to_double(a), y = to_double(b);
    const double r = ieee ? std::remainder(x, y) : std::fmod(x, y);
    const double q = ieee ? std::nearbyint(x / y) : std::trunc(x / y);
    const uint64_t qi = std::isfinite(q) ? (uint64_t)std::fabs(q) : 0;
    // Complete reduction (C2 = 0); C0, C3, C1 hold quotient bits 2, 1, 0.
    s.sw = (uint16_t)((s.sw & ~0x4700u) | (qi & 4 ? 0x0100 : 0) | (qi & 2 ? 0x4000 : 0) |
                      (qi & 1 ? 0x0200 : 0));
    return from_double(r);
}

F80 f2xm1(F80 a) { return from_double(std::exp2(to_double(a)) - 1.0); }
F80 yl2x(F80 y, F80 x) { return from_double(to_double(y) * std::log2(to_double(x))); }
F80 patan(F80 y, F80 x) { return from_double(std::atan2(to_double(y), to_double(x))); }
F80 tan(F80 a) { return from_double(std::tan(to_double(a))); }
F80 sin(F80 a) { return from_double(std::sin(to_double(a))); }
F80 cos(F80 a) { return from_double(std::cos(to_double(a))); }

void store_env(const X87& s, uint8_t* p) {
    std::memset(p, 0, 28);
    uint16_t tags = 0;
    for (int k = 0; k < 8; k++)
        if (!(s.valid >> k & 1)) tags |= 3u << (2 * k);
    const uint16_t sw = status(s);
    std::memcpy(p, &s.cw, 2);
    std::memcpy(p + 4, &sw, 2);
    std::memcpy(p + 8, &tags, 2);
}

void load_env(X87& s, const uint8_t* p) {
    uint16_t cw, sw, tags;
    std::memcpy(&cw, p, 2);
    std::memcpy(&sw, p + 4, 2);
    std::memcpy(&tags, p + 8, 2);
    s.cw = cw;
    s.sw = sw & ~0x3800;
    s.top = sw >> 11 & 7;
    s.valid = 0;
    for (int k = 0; k < 8; k++)
        if ((tags >> (2 * k) & 3) != 3) s.valid |= 1u << k;
}

void save(X87& s, uint8_t* p) {
    store_env(s, p);
    for (int i = 0; i < 8; i++) {  // ST(0)..ST(7)
        const F80& v = s.r[(s.top + i) & 7];
        std::memcpy(p + 28 + 10 * i, &v.mant, 8);
        std::memcpy(p + 36 + 10 * i, &v.se, 2);
    }
    init(s);
}

void restore(X87& s, const uint8_t* p) {
    load_env(s, p);
    for (int i = 0; i < 8; i++) {
        F80& v = s.r[(s.top + i) & 7];
        std::memcpy(&v.mant, p + 28 + 10 * i, 8);
        std::memcpy(&v.se, p + 36 + 10 * i, 2);
    }
}

}  // namespace scimitar::x87
