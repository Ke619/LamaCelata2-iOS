// printf-family imports. The format string and arguments live in guest
// memory: arguments are read in order from the guest va_list (on i386 a
// pointer to the next argument on the stack: 4 bytes per int/pointer, 8 per
// double and long long, 12 per long double). Each conversion is formatted by
// the host's snprintf; %s and %ls read guest strings (wchar_t is 32 bits).
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "scimitar/imports.h"
#include "scimitar/x87.h"

namespace scimitar {

namespace {

class Args {
public:
    Args(GuestMemory& m, uint32_t ap) : m_(m), ap_(ap) {}
    uint32_t u32() { const uint32_t v = m_.read<uint32_t>(ap_); ap_ += 4; return v; }
    uint64_t u64() { const uint64_t v = m_.read<uint64_t>(ap_); ap_ += 8; return v; }
    double f64() { const uint64_t b = u64(); double d; std::memcpy(&d, &b, 8); return d; }
    double f80() {  // long double: 10 bytes in a 12-byte slot
        F80 v;
        std::memcpy(&v.mant, m_.ptr(ap_), 8);
        std::memcpy(&v.se, m_.ptr(ap_ + 8), 2);
        ap_ += 12;
        X87 s;
        const uint64_t b = x87::to_f64(s, v);
        double d;
        std::memcpy(&d, &b, 8);
        return d;
    }

private:
    GuestMemory& m_;
    uint32_t ap_;
};

void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) out += (char)(0xc0 | cp >> 6), out += (char)(0x80 | (cp & 0x3f));
    else if (cp < 0x10000)
        out += (char)(0xe0 | cp >> 12), out += (char)(0x80 | (cp >> 6 & 0x3f)), out += (char)(0x80 | (cp & 0x3f));
    else
        out += (char)(0xf0 | cp >> 18), out += (char)(0x80 | (cp >> 12 & 0x3f)),
            out += (char)(0x80 | (cp >> 6 & 0x3f)), out += (char)(0x80 | (cp & 0x3f));
}

ObjectText g_object_text = nullptr;

}  // namespace

void set_object_formatter(ObjectText f) { g_object_text = f; }

// Formats guest format `fmt` (bytes, or 32-bit units when `wide`) with
// arguments from guest va_list `ap`. The result is UTF-8 (wide output is
// converted back by the caller).
std::string guest_format(GuestMemory& m, uint32_t fmt, uint32_t ap, bool wide) {
    Args args(m, ap);
    std::string out;
    auto ch = [&](uint32_t k) -> uint32_t {
        return wide ? m.read<uint32_t>(fmt + 4 * k) : m.read<uint8_t>(fmt + k);
    };
    for (uint32_t k = 0;;) {
        uint32_t c = ch(k);
        if (!c) break;
        if (c != '%') {
            if (wide) append_utf8(out, c);
            else out += (char)c;
            k++;
            continue;
        }
        // %[flags][width][.precision][length]conversion
        std::string spec = "%";
        k++;
        while ((c = ch(k)) && std::strchr("-+ #0'", (int)c)) spec += (char)c, k++;
        if (ch(k) == '*') {
            spec += std::to_string((int32_t)args.u32());
            k++;
        } else {
            while ((c = ch(k)) >= '0' && c <= '9') spec += (char)c, k++;
        }
        if (ch(k) == '.') {
            spec += '.';
            k++;
            if (ch(k) == '*') {
                spec += std::to_string((int32_t)args.u32());
                k++;
            } else {
                while ((c = ch(k)) >= '0' && c <= '9') spec += (char)c, k++;
            }
        }
        std::string len;
        while ((c = ch(k)) && std::strchr("hlLqjzt", (int)c)) len += (char)c, k++;
        const uint32_t conv = ch(k);
        if (!conv) break;
        k++;
        char buf[512];
        const bool ll = len == "ll" || len == "q" || len == "j";
        switch (conv) {
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': {
            const std::string f = spec + (ll ? "ll" : "") + (char)conv;
            if (ll) std::snprintf(buf, sizeof buf, f.c_str(), (long long)args.u64());
            else if (len == "hh") std::snprintf(buf, sizeof buf, f.c_str(), conv == 'd' || conv == 'i' ? (int)(int8_t)args.u32() : (int)(uint8_t)args.u32());
            else if (len == "h") std::snprintf(buf, sizeof buf, f.c_str(), conv == 'd' || conv == 'i' ? (int)(int16_t)args.u32() : (int)(uint16_t)args.u32());
            else std::snprintf(buf, sizeof buf, f.c_str(), (int)args.u32());
            out += buf;
            break;
        }
        case 'c':
        case 'C': {
            const uint32_t v = args.u32();
            if (conv == 'C' || len == "l") {
                std::string u;
                append_utf8(u, v);
                std::snprintf(buf, sizeof buf, (spec + "s").c_str(), u.c_str());
            } else {
                std::snprintf(buf, sizeof buf, (spec + "c").c_str(), (int)(char)v);
            }
            out += buf;
            break;
        }
        case 's':
        case 'S': {
            const uint32_t p = args.u32();
            std::string s;
            if (!p) s = "(null)";
            else if (conv == 'S' || len == "l")
                for (uint32_t i = 0; m.read<uint32_t>(p + 4 * i); i++) append_utf8(s, m.read<uint32_t>(p + 4 * i));
            else s = (const char*)m.ptr(p);
            const size_t need = s.size() + 64;
            std::string tmp(need + 256, '\0');
            const int n = std::snprintf(tmp.data(), tmp.size(), (spec + "s").c_str(), s.c_str());
            if (n > 0) out.append(tmp.data(), std::min<size_t>((size_t)n, tmp.size() - 1));
            break;
        }
        case 'p':
            std::snprintf(buf, sizeof buf, "0x%x", args.u32());
            out += buf;
            break;
        case '@': {  // CFString/NSString formats: an object, printed as its text
            const uint32_t obj = args.u32();
            out += g_object_text ? g_object_text(m, obj) : "(object)";
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            const double v = len == "L" ? args.f80() : args.f64();
            std::snprintf(buf, sizeof buf, (spec + (char)conv).c_str(), v);
            out += buf;
            break;
        }
        case 'n':
            m.write<uint32_t>(args.u32(), (uint32_t)out.size());
            break;
        case '%':
            out += '%';
            break;
        default:
            out += spec + len + (char)conv;
            break;
        }
    }
    return out;
}

namespace imports {

namespace {

// Writes `s` (with a terminating NUL, truncated to `size`) to guest `dst`;
// returns the untruncated length, as snprintf does.
uint32_t emit(Context& c, uint32_t dst, uint32_t size, const std::string& s) {
    if (size) {
        const size_t n = std::min<size_t>(s.size(), size - 1);
        std::memcpy(c.mem->ptr(dst), s.data(), n);
        c.mem->write<uint8_t>(dst + (uint32_t)n, 0);
    }
    return (uint32_t)s.size();
}

// Wide output: UTF-8 back to 32-bit units. Returns the count, or -1 when it
// does not fit (as swprintf does).
uint32_t emit_wide(Context& c, uint32_t dst, uint32_t size, const std::string& s) {
    std::u32string w;
    for (size_t i = 0; i < s.size();) {
        const uint8_t b = (uint8_t)s[i];
        uint32_t cp = b, extra = 0;
        if (b >= 0xf0) cp = b & 7, extra = 3;
        else if (b >= 0xe0) cp = b & 15, extra = 2;
        else if (b >= 0xc0) cp = b & 31, extra = 1;
        i++;
        for (uint32_t e = 0; e < extra && i < s.size(); e++, i++) cp = cp << 6 | ((uint8_t)s[i] & 0x3f);
        w.push_back((char32_t)cp);
    }
    if (!size) return (uint32_t)-1;
    const size_t n = std::min<size_t>(w.size(), size - 1);
    for (size_t i = 0; i < n; i++) c.mem->write<uint32_t>(dst + 4 * (uint32_t)i, (uint32_t)w[i]);
    c.mem->write<uint32_t>(dst + 4 * (uint32_t)n, 0);
    return w.size() < size ? (uint32_t)w.size() : (uint32_t)-1;
}

// The va_list of a variadic shim: the address of the first variadic argument.
uint32_t varargs(Context& c, int first) { return c.esp + 4 + 4 * (uint32_t)first; }

}  // namespace

// int vsnprintf(char* dst, size_t size, const char* fmt, va_list ap)
void imp_vsnprintf(Context& c) {
    shim_return(c, emit(c, arg32(c, 0), arg32(c, 1), guest_format(*c.mem, arg32(c, 2), arg32(c, 3), false)));
}
// int snprintf(char* dst, size_t size, const char* fmt, ...)
void imp_snprintf(Context& c) {
    shim_return(c, emit(c, arg32(c, 0), arg32(c, 1), guest_format(*c.mem, arg32(c, 2), varargs(c, 3), false)));
}
// int vsprintf(char* dst, const char* fmt, va_list ap) -- no bound, as in C
void imp_vsprintf(Context& c) {
    shim_return(c, emit(c, arg32(c, 0), 0xffffffffu, guest_format(*c.mem, arg32(c, 1), arg32(c, 2), false)));
}
// int sprintf(char* dst, const char* fmt, ...)
void imp_sprintf(Context& c) {
    shim_return(c, emit(c, arg32(c, 0), 0xffffffffu, guest_format(*c.mem, arg32(c, 1), varargs(c, 2), false)));
}
// int printf(const char* fmt, ...): to the host's stdout.
void imp_printf(Context& c) {
    const std::string s = guest_format(*c.mem, arg32(c, 0), varargs(c, 1), false);
    std::fwrite(s.data(), 1, s.size(), stdout);
    shim_return(c, (uint32_t)s.size());
}
// int swprintf(wchar_t* dst, size_t size, const wchar_t* fmt, ...)
void imp_swprintf(Context& c) {
    shim_return(c, emit_wide(c, arg32(c, 0), arg32(c, 1), guest_format(*c.mem, arg32(c, 2), varargs(c, 3), true)));
}
// int vswprintf(wchar_t* dst, size_t size, const wchar_t* fmt, va_list ap)
void imp_vswprintf(Context& c) {
    shim_return(c, emit_wide(c, arg32(c, 0), arg32(c, 1), guest_format(*c.mem, arg32(c, 2), arg32(c, 3), true)));
}

}  // namespace imports

namespace {
using namespace imports;
const ImportShim kPrintf[] = {
    {"_vsnprintf", imp_vsnprintf}, {"_snprintf", imp_snprintf}, {"_vsprintf", imp_vsprintf},
    {"_sprintf", imp_sprintf},     {"_printf", imp_printf},     {"_swprintf", imp_swprintf},
    {"_vswprintf", imp_vswprintf},
};
SCIMITAR_REGISTER_SHIMS(kPrintf);
}  // namespace

}  // namespace scimitar
