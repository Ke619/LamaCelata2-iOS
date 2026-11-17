// libm imports. Arguments are floats (one stack slot) or doubles (two);
// results come back in x87 ST0, as the i386 ABI returns floating-point values.
// The host libm may differ from the original in the last bit of some results.
#include <cmath>

#include "scimitar/imports.h"
#include "scimitar/runtime.h"

namespace scimitar::imports {

#define D1(NAME, FN) \
    void imp_##NAME(Context& c) { shim_return_f64(c, FN(arg_f64(c, 0))); }
#define D2(NAME, FN) \
    void imp_##NAME(Context& c) { shim_return_f64(c, FN(arg_f64(c, 0), arg_f64(c, 2))); }
#define F1(NAME, FN) \
    void imp_##NAME(Context& c) { shim_return_f32(c, FN(arg_f32(c, 0))); }
#define F2(NAME, FN) \
    void imp_##NAME(Context& c) { shim_return_f32(c, FN(arg_f32(c, 0), arg_f32(c, 1))); }

D1(acos, std::acos) D1(asin, std::asin) D1(atan, std::atan) D1(ceil, std::ceil)
D1(cos, std::cos) D1(exp, std::exp) D1(floor, std::floor) D1(log, std::log)
D1(log10, std::log10) D1(sin, std::sin) D1(tan, std::tan) D1(sqrt, std::sqrt)
D2(atan2, std::atan2) D2(fmod, std::fmod) D2(pow, std::pow)
F1(acosf, std::acos) F1(asinf, std::asin) F1(atanf, std::atan) F1(ceilf, std::ceil)
F1(cosf, std::cos) F1(expf, std::exp) F1(floorf, std::floor) F1(logf, std::log)
F1(log10f, std::log10) F1(sinf, std::sin) F1(tanf, std::tan) F1(sqrtf, std::sqrt)
F1(fabsf, std::fabs) F2(atan2f, std::atan2) F2(fmodf, std::fmod) F2(powf, std::pow)
#undef D1
#undef D2
#undef F1
#undef F2

// double frexp(double, int* exp)
void imp_frexp(Context& c) {
    int e = 0;
    const double m = std::frexp(arg_f64(c, 0), &e);
    c.mem->write<uint32_t>(arg32(c, 2), (uint32_t)e);
    shim_return_f64(c, m);
}
// float modff(float, float* integral)
void imp_modff(Context& c) {
    float ip = 0;
    const float f = std::modf(arg_f32(c, 0), &ip);
    uint32_t b;
    std::memcpy(&b, &ip, 4);
    c.mem->write<uint32_t>(arg32(c, 1), b);
    shim_return_f32(c, f);
}
// libgcc's 64-bit division (i386 has none): long long __divdi3(long long,
// long long) and the others, results in edx:eax. A zero divisor traps, as on x86.
namespace {
uint64_t divisor(Context& c) {
    const uint64_t d = arg64(c, 2);
    if (!d) fatal("64-bit division by zero (called from %08x)", c.mem->read<uint32_t>(c.esp));
    return d;
}
}  // namespace
void imp___divdi3(Context& c) { const int64_t d = (int64_t)divisor(c); shim_return64(c, (uint64_t)((int64_t)arg64(c, 0) / d)); }
void imp___moddi3(Context& c) { const int64_t d = (int64_t)divisor(c); shim_return64(c, (uint64_t)((int64_t)arg64(c, 0) % d)); }
void imp___udivdi3(Context& c) { const uint64_t d = divisor(c); shim_return64(c, arg64(c, 0) / d); }
void imp___umoddi3(Context& c) { const uint64_t d = divisor(c); shim_return64(c, arg64(c, 0) % d); }

// int finite(double)
void imp_finite(Context& c) { shim_return(c, std::isfinite(arg_f64(c, 0)) ? 1 : 0); }

}  // namespace scimitar::imports

namespace scimitar {
namespace {
using namespace imports;
const ImportShim kMath[] = {
    {"_fabsf", imp_fabsf},         {"___divdi3", imp___divdi3},   {"___moddi3", imp___moddi3},
    {"___udivdi3", imp___udivdi3}, {"___umoddi3", imp___umoddi3},
    {"_acos", imp_acos},   {"_asin", imp_asin},   {"_atan", imp_atan},     {"_ceil", imp_ceil},
    {"_cos", imp_cos},     {"_exp", imp_exp},     {"_floor", imp_floor},   {"_log", imp_log},
    {"_log10", imp_log10}, {"_sin", imp_sin},     {"_tan", imp_tan},       {"_sqrt", imp_sqrt},
    {"_atan2", imp_atan2}, {"_fmod", imp_fmod},   {"_pow", imp_pow},       {"_acosf", imp_acosf},
    {"_asinf", imp_asinf}, {"_atanf", imp_atanf}, {"_ceilf", imp_ceilf},   {"_cosf", imp_cosf},
    {"_expf", imp_expf},   {"_floorf", imp_floorf}, {"_logf", imp_logf},   {"_log10f", imp_log10f},
    {"_sinf", imp_sinf},   {"_tanf", imp_tanf},   {"_sqrtf", imp_sqrtf},   {"_atan2f", imp_atan2f},
    {"_fmodf", imp_fmodf}, {"_powf", imp_powf},   {"_frexp", imp_frexp},   {"_modff", imp_modff},
    {"_finite", imp_finite},
};
SCIMITAR_REGISTER_SHIMS(kMath);
}  // namespace
}  // namespace scimitar
