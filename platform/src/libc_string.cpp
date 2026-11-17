// String and character imports from libSystem, on guest memory. The game's
// wchar_t is 32 bits (Mac), so the wide functions work on 32-bit units.
#include <cctype>
#include <cstring>
#include <string>

#include "scimitar/guest_heap.h"
#include "scimitar/imports.h"

namespace scimitar::imports {

namespace {

uint8_t* P(Context& c, uint32_t a) { return c.mem->ptr(a); }
uint32_t W(Context& c, uint32_t a, uint32_t k) { return c.mem->read<uint32_t>(a + 4 * k); }
void setW(Context& c, uint32_t a, uint32_t k, uint32_t v) { c.mem->write<uint32_t>(a + 4 * k, v); }

uint32_t wlen(Context& c, uint32_t s) {
    uint32_t n = 0;
    while (W(c, s, n)) n++;
    return n;
}

}  // namespace

// ---- memory ----
void imp_memset(Context& c) {
    const uint32_t d = arg32(c, 0), n = arg32(c, 2);
    if ((uint64_t)d + n > GuestMemory::size()) fatal("memset: range outside guest memory");
    std::memset(P(c, d), (int)(arg32(c, 1) & 0xff), n);
    shim_return(c, d);
}
void imp_bzero(Context& c) {
    const uint32_t d = arg32(c, 0), n = arg32(c, 1);
    if ((uint64_t)d + n > GuestMemory::size()) fatal("bzero: range outside guest memory");
    std::memset(P(c, d), 0, n);
    shim_return(c, 0);
}
void imp_memcmp(Context& c) {
    shim_return(c, (uint32_t)std::memcmp(P(c, arg32(c, 0)), P(c, arg32(c, 1)), arg32(c, 2)));
}
void imp_memchr(Context& c) {
    const uint32_t s = arg32(c, 0), n = arg32(c, 2);
    const void* r = std::memchr(P(c, s), (int)(arg32(c, 1) & 0xff), n);
    shim_return(c, r ? s + (uint32_t)((const uint8_t*)r - P(c, s)) : 0);
}

// ---- byte strings ----
void imp_strlen(Context& c) { shim_return(c, (uint32_t)std::strlen((char*)P(c, arg32(c, 0)))); }
void imp_strcmp(Context& c) {
    shim_return(c, (uint32_t)std::strcmp((char*)P(c, arg32(c, 0)), (char*)P(c, arg32(c, 1))));
}
void imp_strncmp(Context& c) {
    shim_return(c, (uint32_t)std::strncmp((char*)P(c, arg32(c, 0)), (char*)P(c, arg32(c, 1)), arg32(c, 2)));
}
void imp_strcasecmp(Context& c) {
    shim_return(c, (uint32_t)strcasecmp((char*)P(c, arg32(c, 0)), (char*)P(c, arg32(c, 1))));
}
void imp_strncasecmp(Context& c) {
    shim_return(c, (uint32_t)strncasecmp((char*)P(c, arg32(c, 0)), (char*)P(c, arg32(c, 1)), arg32(c, 2)));
}
void imp_strcpy(Context& c) {
    const uint32_t d = arg32(c, 0);
    std::memmove(P(c, d), P(c, arg32(c, 1)), std::strlen((char*)P(c, arg32(c, 1))) + 1);
    shim_return(c, d);
}
void imp_strncpy(Context& c) {
    const uint32_t d = arg32(c, 0);
    std::strncpy((char*)P(c, d), (char*)P(c, arg32(c, 1)), arg32(c, 2));
    shim_return(c, d);
}
void imp_strcat(Context& c) {
    const uint32_t d = arg32(c, 0);
    std::strcat((char*)P(c, d), (char*)P(c, arg32(c, 1)));
    shim_return(c, d);
}
void imp_strncat(Context& c) {
    const uint32_t d = arg32(c, 0);
    std::strncat((char*)P(c, d), (char*)P(c, arg32(c, 1)), arg32(c, 2));
    shim_return(c, d);
}
void imp_strlcpy(Context& c) {
    shim_return(c, (uint32_t)strlcpy((char*)P(c, arg32(c, 0)), (char*)P(c, arg32(c, 1)), arg32(c, 2)));
}
void imp_strlcat(Context& c) {
    shim_return(c, (uint32_t)strlcat((char*)P(c, arg32(c, 0)), (char*)P(c, arg32(c, 1)), arg32(c, 2)));
}
void imp_strchr(Context& c) {
    const uint32_t s = arg32(c, 0);
    const char* r = std::strchr((char*)P(c, s), (int)(arg32(c, 1) & 0xff));
    shim_return(c, r ? s + (uint32_t)(r - (char*)P(c, s)) : 0);
}
void imp_strrchr(Context& c) {
    const uint32_t s = arg32(c, 0);
    const char* r = std::strrchr((char*)P(c, s), (int)(arg32(c, 1) & 0xff));
    shim_return(c, r ? s + (uint32_t)(r - (char*)P(c, s)) : 0);
}
void imp_strstr(Context& c) {
    const uint32_t s = arg32(c, 0);
    const char* r = std::strstr((char*)P(c, s), (char*)P(c, arg32(c, 1)));
    shim_return(c, r ? s + (uint32_t)(r - (char*)P(c, s)) : 0);
}
void imp_strcspn(Context& c) {
    shim_return(c, (uint32_t)std::strcspn((char*)P(c, arg32(c, 0)), (char*)P(c, arg32(c, 1))));
}
void imp_strdup(Context& c) {
    const uint32_t s = arg32(c, 0);
    const uint32_t n = (uint32_t)std::strlen((char*)P(c, s)) + 1;
    const uint32_t d = heap::alloc(*c.mem, n);
    if (d) std::memcpy(P(c, d), P(c, s), n);
    shim_return(c, d);
}
void imp___tolower(Context& c) { shim_return(c, (uint32_t)std::tolower((int)arg32(c, 0))); }
void imp___toupper(Context& c) { shim_return(c, (uint32_t)std::toupper((int)arg32(c, 0))); }

// ---- wide strings (32-bit units) ----
void imp_wcslen(Context& c) { shim_return(c, wlen(c, arg32(c, 0))); }
void imp_wcscmp(Context& c) {
    const uint32_t a = arg32(c, 0), b = arg32(c, 1);
    for (uint32_t k = 0;; k++) {
        const int32_t x = (int32_t)W(c, a, k), y = (int32_t)W(c, b, k);
        if (x != y) return shim_return(c, x < y ? (uint32_t)-1 : 1);
        if (!x) return shim_return(c, 0);
    }
}
void imp_wcsncmp(Context& c) {
    const uint32_t a = arg32(c, 0), b = arg32(c, 1), n = arg32(c, 2);
    for (uint32_t k = 0; k < n; k++) {
        const int32_t x = (int32_t)W(c, a, k), y = (int32_t)W(c, b, k);
        if (x != y) return shim_return(c, x < y ? (uint32_t)-1 : 1);
        if (!x) break;
    }
    shim_return(c, 0);
}
void imp_wcscpy(Context& c) {
    const uint32_t d = arg32(c, 0), s = arg32(c, 1), n = wlen(c, s);
    std::memmove(P(c, d), P(c, s), 4 * (n + 1));
    shim_return(c, d);
}
void imp_wcsncpy(Context& c) {
    const uint32_t d = arg32(c, 0), s = arg32(c, 1), n = arg32(c, 2);
    uint32_t k = 0;
    for (; k < n && W(c, s, k); k++) setW(c, d, k, W(c, s, k));
    for (; k < n; k++) setW(c, d, k, 0);
    shim_return(c, d);
}
void imp_wcslcpy(Context& c) {
    const uint32_t d = arg32(c, 0), s = arg32(c, 1), size = arg32(c, 2), n = wlen(c, s);
    if (size) {
        const uint32_t m = n < size - 1 ? n : size - 1;
        std::memmove(P(c, d), P(c, s), 4 * m);
        setW(c, d, m, 0);
    }
    shim_return(c, n);
}
void imp_wcslcat(Context& c) {
    const uint32_t d = arg32(c, 0), s = arg32(c, 1), size = arg32(c, 2);
    uint32_t dl = 0;
    while (dl < size && W(c, d, dl)) dl++;
    const uint32_t sl = wlen(c, s);
    if (dl < size) {
        const uint32_t room = size - dl - 1, m = sl < room ? sl : room;
        std::memmove(P(c, d + 4 * dl), P(c, s), 4 * m);
        setW(c, d, dl + m, 0);
    }
    shim_return(c, dl + sl);
}
void imp_wcsstr(Context& c) {
    const uint32_t h = arg32(c, 0), n = arg32(c, 1), hl = wlen(c, h), nl = wlen(c, n);
    if (!nl) return shim_return(c, h);
    for (uint32_t k = 0; k + nl <= hl; k++)
        if (std::memcmp(P(c, h + 4 * k), P(c, n), 4 * nl) == 0) return shim_return(c, h + 4 * k);
    shim_return(c, 0);
}
void imp_wmemcpy(Context& c) {
    const uint32_t d = arg32(c, 0);
    std::memmove(P(c, d), P(c, arg32(c, 1)), 4 * (size_t)arg32(c, 2));
    shim_return(c, d);
}
void imp_wmemmove(Context& c) { imp_wmemcpy(c); }
void imp_wmemset(Context& c) {
    const uint32_t d = arg32(c, 0), v = arg32(c, 1), n = arg32(c, 2);
    for (uint32_t k = 0; k < n; k++) setW(c, d, k, v);
    shim_return(c, d);
}
void imp_wmemcmp(Context& c) {
    const uint32_t a = arg32(c, 0), b = arg32(c, 1), n = arg32(c, 2);
    for (uint32_t k = 0; k < n; k++) {
        const int32_t x = (int32_t)W(c, a, k), y = (int32_t)W(c, b, k);
        if (x != y) return shim_return(c, x < y ? (uint32_t)-1 : 1);
    }
    shim_return(c, 0);
}
void imp_wmemchr(Context& c) {
    const uint32_t s = arg32(c, 0), v = arg32(c, 1), n = arg32(c, 2);
    for (uint32_t k = 0; k < n; k++)
        if (W(c, s, k) == v) return shim_return(c, s + 4 * k);
    shim_return(c, 0);
}

}  // namespace scimitar::imports

namespace scimitar {
namespace {
using namespace imports;
const ImportShim kString[] = {
    {"_memset", imp_memset},       {"_bzero", imp_bzero},       {"_memcmp", imp_memcmp},       {"_memchr", imp_memchr},
    {"_strlen", imp_strlen},       {"_strcmp", imp_strcmp},       {"_strncmp", imp_strncmp},
    {"_strcasecmp", imp_strcasecmp}, {"_strncasecmp", imp_strncasecmp},
    {"_strcpy", imp_strcpy},       {"_strncpy", imp_strncpy},     {"_strcat", imp_strcat},
    {"_strncat", imp_strncat},     {"_strlcpy", imp_strlcpy},     {"_strlcat", imp_strlcat},
    {"_strchr", imp_strchr},       {"_strrchr", imp_strrchr},     {"_strstr", imp_strstr},
    {"_strcspn", imp_strcspn},     {"_strdup", imp_strdup},
    {"___tolower", imp___tolower}, {"___toupper", imp___toupper},
    {"_wcslen", imp_wcslen},       {"_wcscmp", imp_wcscmp},       {"_wcsncmp", imp_wcsncmp},
    {"_wcscpy", imp_wcscpy},       {"_wcsncpy", imp_wcsncpy},     {"_wcslcpy", imp_wcslcpy},
    {"_wcslcat", imp_wcslcat},     {"_wcsstr", imp_wcsstr},       {"_wmemcpy", imp_wmemcpy},
    {"_wmemmove", imp_wmemmove},   {"_wmemset", imp_wmemset},     {"_wmemcmp", imp_wmemcmp},
    {"_wmemchr", imp_wmemchr},
};
SCIMITAR_REGISTER_SHIMS(kString);
}  // namespace
}  // namespace scimitar
