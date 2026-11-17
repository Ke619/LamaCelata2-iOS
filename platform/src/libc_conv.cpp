// Number conversions, ctype/locale, multibyte conversions, scanf and strftime.
// The game runs in the "C" locale: one byte per character, wchar_t (32 bits)
// holds the byte value.
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <runetype.h>
#include <string>

#include "scimitar/guest_heap.h"
#include "scimitar/imports.h"
#include "scimitar/runtime.h"

namespace scimitar::imports {

namespace {

const char* S(Context& c, uint32_t a) { return (const char*)c.mem->ptr(a); }

// Guest copies of host strings that must stay valid (setlocale's result...).
uint32_t static_string(Context& c, const std::string& s) {
    const uint32_t a = heap::alloc(*c.mem, (uint32_t)s.size() + 1);
    std::memcpy(c.mem->ptr(a), s.c_str(), s.size() + 1);
    return a;
}

void set_end(Context& c, uint32_t endp, uint32_t s, const char* host_s, const char* host_end) {
    if (endp) c.mem->write<uint32_t>(endp, s + (uint32_t)(host_end - host_s));
}

// i386 struct tm: nine ints, long tm_gmtoff, char* tm_zone (44 bytes).
std::tm read_tm(Context& c, uint32_t p) {
    std::tm t{};
    int* f[9] = {&t.tm_sec, &t.tm_min, &t.tm_hour, &t.tm_mday, &t.tm_mon,
                 &t.tm_year, &t.tm_wday, &t.tm_yday, &t.tm_isdst};
    for (int k = 0; k < 9; k++) *f[k] = (int32_t)c.mem->read<uint32_t>(p + 4 * k);
    t.tm_gmtoff = (int32_t)c.mem->read<uint32_t>(p + 36);
    return t;
}

}  // namespace

void write_tm(Context& c, uint32_t p, const std::tm& t) {
    const int f[9] = {t.tm_sec, t.tm_min, t.tm_hour, t.tm_mday, t.tm_mon,
                      t.tm_year, t.tm_wday, t.tm_yday, t.tm_isdst};
    for (int k = 0; k < 9; k++) c.mem->write<uint32_t>(p + 4 * k, (uint32_t)f[k]);
    c.mem->write<uint32_t>(p + 36, (uint32_t)t.tm_gmtoff);
    c.mem->write<uint32_t>(p + 40, 0);
}

// ---- numbers ----
void imp_atoi(Context& c) { shim_return(c, (uint32_t)std::atoi(S(c, arg32(c, 0)))); }
void imp_atof(Context& c) { shim_return_f64(c, std::atof(S(c, arg32(c, 0)))); }
void imp_strtod(Context& c) {
    char* end;
    const char* s = S(c, arg32(c, 0));
    const double v = std::strtod(s, &end);
    set_end(c, arg32(c, 1), arg32(c, 0), s, end);
    shim_return_f64(c, v);
}
void imp_strtod_UNIX2003(Context& c) { imp_strtod(c); }
void imp_strtof_UNIX2003(Context& c) {
    char* end;
    const char* s = S(c, arg32(c, 0));
    const float v = std::strtof(s, &end);
    set_end(c, arg32(c, 1), arg32(c, 0), s, end);
    shim_return_f32(c, v);
}
// long double is the 80-bit type on i386; parsed as a double here.
void imp_strtold(Context& c) { imp_strtod(c); }
void imp_strtol(Context& c) {
    char* end;
    const char* s = S(c, arg32(c, 0));
    long long v = std::strtoll(s, &end, (int)arg32(c, 2));
    if (v > INT32_MAX) v = INT32_MAX, errno = ERANGE;
    if (v < INT32_MIN) v = INT32_MIN, errno = ERANGE;
    set_end(c, arg32(c, 1), arg32(c, 0), s, end);
    shim_return(c, (uint32_t)(int32_t)v);
}
void imp_strtoul(Context& c) {
    char* end;
    const char* s = S(c, arg32(c, 0));
    unsigned long long v = std::strtoull(s, &end, (int)arg32(c, 2));
    if (v > UINT32_MAX && s[std::strspn(s, " \t\n")] != '-') v = UINT32_MAX;
    set_end(c, arg32(c, 1), arg32(c, 0), s, end);
    shim_return(c, (uint32_t)v);
}
void imp_strtoll(Context& c) {
    char* end;
    const char* s = S(c, arg32(c, 0));
    const long long v = std::strtoll(s, &end, (int)arg32(c, 2));
    set_end(c, arg32(c, 1), arg32(c, 0), s, end);
    shim_return64(c, (uint64_t)v);
}

// ---- ctype, locale ----
// int __maskrune(__darwin_ct_rune_t c, unsigned long mask)
void imp___maskrune(Context& c) {
    const uint32_t ch = arg32(c, 0);
    shim_return(c, ch < 256 ? (uint32_t)(_DefaultRuneLocale.__runetype[ch] & arg32(c, 1)) : 0);
}
void imp_setlocale(Context& c) {
    static uint32_t s_c = 0;
    if (!s_c) s_c = static_string(c, "C");
    shim_return(c, s_c);
}
void imp_strcoll(Context& c) { shim_return(c, (uint32_t)std::strcmp(S(c, arg32(c, 0)), S(c, arg32(c, 1)))); }
void imp_strxfrm(Context& c) {
    const uint32_t d = arg32(c, 0), n = arg32(c, 2);
    const char* s = S(c, arg32(c, 1));
    if (n) strlcpy((char*)c.mem->ptr(d), s, n);
    shim_return(c, (uint32_t)std::strlen(s));
}
void imp_wcscoll(Context& c) {
    for (uint32_t a = arg32(c, 0), b = arg32(c, 1);; a += 4, b += 4) {
        const int32_t x = (int32_t)c.mem->read<uint32_t>(a), y = (int32_t)c.mem->read<uint32_t>(b);
        if (x != y) return shim_return(c, x < y ? (uint32_t)-1 : 1);
        if (!x) return shim_return(c, 0);
    }
}
void imp_wcsxfrm(Context& c) {
    const uint32_t d = arg32(c, 0), s = arg32(c, 1), n = arg32(c, 2);
    uint32_t len = 0;
    while (c.mem->read<uint32_t>(s + 4 * len)) len++;
    if (n) {
        const uint32_t m = len < n - 1 ? len : n - 1;
        std::memmove(c.mem->ptr(d), c.mem->ptr(s), 4 * m);
        c.mem->write<uint32_t>(d + 4 * m, 0);
    }
    shim_return(c, len);
}

// ---- multibyte (C locale: bytes) ----
void imp_wctob(Context& c) { const uint32_t w = arg32(c, 0); shim_return(c, w < 0x80 ? w : (uint32_t)EOF); }
void imp_btowc(Context& c) {
    const int32_t b = (int32_t)arg32(c, 0);
    shim_return(c, b == EOF ? 0xffffffffu : (uint32_t)(b & 0xff));
}
// size_t mbrtowc(wchar_t* pwc, const char* s, size_t n, mbstate_t*)
void imp_mbrtowc(Context& c) {
    const uint32_t pwc = arg32(c, 0), s = arg32(c, 1), n = arg32(c, 2);
    if (!s) return shim_return(c, 0);
    if (!n) return shim_return(c, (uint32_t)-2);
    const uint8_t b = c.mem->read<uint8_t>(s);
    if (pwc) c.mem->write<uint32_t>(pwc, b);
    shim_return(c, b ? 1 : 0);
}
// size_t wcrtomb(char* s, wchar_t wc, mbstate_t*)
void imp_wcrtomb(Context& c) {
    const uint32_t s = arg32(c, 0), wc = arg32(c, 1);
    if (!s) return shim_return(c, 1);
    if (wc > 0xff) {
        errno = EILSEQ;
        return shim_return(c, (uint32_t)-1);
    }
    c.mem->write<uint8_t>(s, (uint8_t)wc);
    shim_return(c, 1);
}
// size_t mbstowcs(wchar_t* dst, const char* src, size_t n)
void imp_mbstowcs(Context& c) {
    const uint32_t d = arg32(c, 0), s = arg32(c, 1), n = arg32(c, 2);
    uint32_t k = 0;
    for (; !d || k < n; k++) {
        const uint8_t b = c.mem->read<uint8_t>(s + k);
        if (d) c.mem->write<uint32_t>(d + 4 * k, b);
        if (!b) break;
    }
    shim_return(c, k);
}
// size_t wcstombs(char* dst, const wchar_t* src, size_t n)
void imp_wcstombs(Context& c) {
    const uint32_t d = arg32(c, 0), s = arg32(c, 1), n = arg32(c, 2);
    uint32_t k = 0;
    for (; !d || k < n; k++) {
        const uint32_t w = c.mem->read<uint32_t>(s + 4 * k);
        if (w > 0xff) {
            errno = EILSEQ;
            return shim_return(c, (uint32_t)-1);
        }
        if (d) c.mem->write<uint8_t>(d + k, (uint8_t)w);
        if (!w) break;
    }
    shim_return(c, k);
}

// ---- scanf ----
namespace {

// Scans guest `input` by guest `fmt`, storing through the guest pointers in
// the arguments at `ap`. Each conversion is matched by the host's sscanf (with
// %n for how much it consumed). Returns the number of conversions stored, or
// EOF if the input ended before the first one.
int guest_scan(Context& c, const std::string& in, const std::string& fmt, uint32_t ap, bool wide) {
    size_t pos = 0, f = 0;
    int stored = 0;
    auto next_arg = [&] {
        const uint32_t p = c.mem->read<uint32_t>(ap);
        ap += 4;
        return p;
    };
    while (f < fmt.size()) {
        const char ch = fmt[f];
        if (std::isspace((unsigned char)ch)) {
            while (pos < in.size() && std::isspace((unsigned char)in[pos])) pos++;
            f++;
            continue;
        }
        if (ch != '%') {
            if (pos >= in.size() || in[pos] != ch) break;
            pos++, f++;
            continue;
        }
        // %[*][width][length]conv
        std::string spec = "%";
        f++;
        bool suppress = false;
        if (f < fmt.size() && fmt[f] == '*') suppress = true, f++;
        while (f < fmt.size() && std::isdigit((unsigned char)fmt[f])) spec += fmt[f++];
        std::string len;
        while (f < fmt.size() && std::strchr("hlLqjzt", fmt[f])) len += fmt[f++];
        if (f >= fmt.size()) break;
        char conv = fmt[f++];
        std::string set;
        if (conv == '[') {
            set = "[";
            if (f < fmt.size() && fmt[f] == '^') set += fmt[f++];
            if (f < fmt.size() && fmt[f] == ']') set += fmt[f++];
            while (f < fmt.size() && fmt[f] != ']') set += fmt[f++];
            set += ']';
            f++;
        }
        if (conv == '%') {
            while (pos < in.size() && std::isspace((unsigned char)in[pos])) pos++;
            if (pos >= in.size() || in[pos] != '%') break;
            pos++;
            continue;
        }
        if (pos >= in.size() && conv != 'n') return stored ? stored : EOF;
        const char* rest = in.c_str() + pos;
        int used = 0;
        bool ok = false;
        const bool ll = len == "ll" || len == "q" || len == "j";
        switch (conv) {
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': {
            long long v = 0;
            const std::string h = spec + "ll" + conv + "%n";
            ok = std::sscanf(rest, h.c_str(), &v, &used) == 1;
            if (ok && !suppress) {
                const uint32_t p = next_arg();
                if (ll) c.mem->write<uint64_t>(p, (uint64_t)v);
                else if (len == "h") c.mem->write<uint16_t>(p, (uint16_t)v);
                else if (len == "hh") c.mem->write<uint8_t>(p, (uint8_t)v);
                else c.mem->write<uint32_t>(p, (uint32_t)v);
            }
            break;
        }
        case 'f': case 'e': case 'g': case 'E': case 'G': case 'a': {
            double v = 0;
            const std::string h = spec + "l" + conv + "%n";
            ok = std::sscanf(rest, h.c_str(), &v, &used) == 1;
            if (ok && !suppress) {
                const uint32_t p = next_arg();
                if (len == "l") c.mem->write<double>(p, v);
                else if (len == "L") {  // long double: 80-bit
                    uint64_t b;
                    std::memcpy(&b, &v, 8);
                    const F80 x = x87::from_f64(b);
                    c.mem->write<uint64_t>(p, x.mant);
                    c.mem->write<uint16_t>(p + 8, x.se);
                } else c.mem->write<float>(p, (float)v);
            }
            break;
        }
        case 's': case 'c': case '[': {
            std::string buf(in.size() + 1, '\0');
            const std::string h = spec + (conv == '[' ? set : std::string(1, conv)) + "%n";
            ok = std::sscanf(rest, h.c_str(), buf.data(), &used) == 1;
            if (ok && !suppress) {
                const uint32_t p = next_arg();
                const size_t n = conv == 'c' ? (size_t)used : std::strlen(buf.c_str());
                const bool w = wide ? !(len == "h") : len == "l";
                for (size_t k = 0; k < n; k++) {
                    if (w) c.mem->write<uint32_t>(p + 4 * (uint32_t)k, (uint8_t)buf[k]);
                    else c.mem->write<uint8_t>(p + (uint32_t)k, (uint8_t)buf[k]);
                }
                if (conv != 'c') {
                    if (w) c.mem->write<uint32_t>(p + 4 * (uint32_t)n, 0);
                    else c.mem->write<uint8_t>(p + (uint32_t)n, 0);
                }
            }
            break;
        }
        case 'n':
            if (!suppress) c.mem->write<uint32_t>(next_arg(), (uint32_t)pos);
            continue;
        default:
            return stored;
        }
        if (!ok) break;
        pos += (size_t)used;
        if (!suppress) stored++;
    }
    return stored;
}

std::string narrow_wide(Context& c, uint32_t s) {
    std::string out;
    for (uint32_t k = 0;; k++) {
        const uint32_t w = c.mem->read<uint32_t>(s + 4 * k);
        if (!w) break;
        out += w < 0x100 ? (char)w : '?';
    }
    return out;
}

}  // namespace

// int sscanf(const char* s, const char* fmt, ...)
void imp_sscanf(Context& c) {
    shim_return(c, (uint32_t)guest_scan(c, S(c, arg32(c, 0)), S(c, arg32(c, 1)), c.esp + 12, false));
}
// int swscanf(const wchar_t* s, const wchar_t* fmt, ...)
void imp_swscanf(Context& c) {
    shim_return(c, (uint32_t)guest_scan(c, narrow_wide(c, arg32(c, 0)), narrow_wide(c, arg32(c, 1)),
                                        c.esp + 12, true));
}

// ---- time formatting ----
// size_t strftime(char* s, size_t max, const char* fmt, const struct tm*)
void imp_strftime_UNIX2003(Context& c) {
    const uint32_t s = arg32(c, 0), max = arg32(c, 1);
    const std::tm t = read_tm(c, arg32(c, 3));
    std::string buf(max + 1, '\0');
    const size_t n = std::strftime(buf.data(), max, S(c, arg32(c, 2)), &t);
    std::memcpy(c.mem->ptr(s), buf.data(), n + (n < max ? 1 : 0));
    shim_return(c, (uint32_t)n);
}
// size_t wcsftime(wchar_t* s, size_t max, const wchar_t* fmt, const struct tm*)
void imp_wcsftime_UNIX2003(Context& c) {
    const uint32_t s = arg32(c, 0), max = arg32(c, 1);
    const std::tm t = read_tm(c, arg32(c, 3));
    std::string buf(max + 1, '\0');
    const size_t n = std::strftime(buf.data(), max, narrow_wide(c, arg32(c, 2)).c_str(), &t);
    for (size_t k = 0; k < n; k++) c.mem->write<uint32_t>(s + 4 * (uint32_t)k, (uint8_t)buf[k]);
    if (n < max) c.mem->write<uint32_t>(s + 4 * (uint32_t)n, 0);
    shim_return(c, (uint32_t)n);
}

}  // namespace scimitar::imports

namespace scimitar {
namespace {
using namespace imports;
const ImportShim kConv[] = {
    {"_atoi", imp_atoi},           {"_atof", imp_atof},           {"_strtod", imp_strtod},
    {"_strtod$UNIX2003", imp_strtod_UNIX2003}, {"_strtof$UNIX2003", imp_strtof_UNIX2003},
    {"_strtold", imp_strtold},     {"_strtol", imp_strtol},       {"_strtoul", imp_strtoul},
    {"_strtoll", imp_strtoll},     {"___maskrune", imp___maskrune}, {"_setlocale", imp_setlocale},
    {"_strcoll", imp_strcoll},     {"_strxfrm", imp_strxfrm},     {"_wcscoll", imp_wcscoll},
    {"_wcsxfrm", imp_wcsxfrm},     {"_wctob", imp_wctob},         {"_btowc", imp_btowc},
    {"_mbrtowc", imp_mbrtowc},     {"_wcrtomb", imp_wcrtomb},     {"_mbstowcs", imp_mbstowcs},
    {"_wcstombs", imp_wcstombs},   {"_sscanf", imp_sscanf},       {"_swscanf", imp_swscanf},
    {"_strftime$UNIX2003", imp_strftime_UNIX2003},
    {"_wcsftime$UNIX2003", imp_wcsftime_UNIX2003},
};
SCIMITAR_REGISTER_SHIMS(kConv);
}  // namespace
}  // namespace scimitar
