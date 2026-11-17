// The smaller imports Prince of Persia (2008) has and AC1 and Revelations do
// not (docs/POP_WINDOWS.md), mostly over their shims: named mutexes, the
// executable's resources, wide file searches and paths, GDI and common
// controls (nothing to do), BSTR and VARIANT release, WININET's ANSI cookies,
// and D3DX 9.39's three functions (over d3d9.mm's constant table and the
// device's CreateTexture). The registry is registry.cpp, USER32's window
// functions are in user32.mm, Bink's in bink_win.cpp.
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>

#include "macho.h"
#include "win32.h"

namespace scimitar::imports {
#define SHIM(dll, fn) void imp_##dll##_##fn(Context& c);
SHIM(kernel32, FindFirstFileA) SHIM(kernel32, FindNextFileA) SHIM(kernel32, GetModuleFileNameA)
SHIM(shell32, SHGetSpecialFolderPathA) SHIM(d3dx9_36, D3DXGetShaderConstantTable)
#undef SHIM
}  // namespace scimitar::imports

namespace scimitar::win32 {
namespace {

uint32_t call_shim(Context& c, void (*fn)(Context&), std::initializer_list<uint32_t> args) {
    const uint32_t esp = c.esp;
    c.esp = (c.esp - 4 * (uint32_t)args.size() - 4) & ~15u;
    uint32_t p = c.esp + 4;
    for (uint32_t a : args) wr(p, a), p += 4;
    fn(c);
    c.esp = esp;
    return c.eax;
}

// Scratch bytes below the stack pointer while a shim runs (put esp back).
uint32_t scratch(Context& c, uint32_t n) {
    c.esp = (c.esp - n - 16) & ~15u;
    return c.esp;
}

// A mutex: owned by one thread at a time, recursively.
struct Mutex : Waitable {
    std::mutex lock;
    uint32_t owner = 0, count = 0;
    bool try_acquire() override {
        std::lock_guard<std::mutex> l(lock);
        const uint32_t me = current_thread_id();
        if (count && owner != me) return false;
        owner = me, count++;
        return true;
    }
    bool signalled() const override { return count == 0; }
};

std::mutex g_names_lock;
std::map<std::string, std::shared_ptr<Mutex>> g_named_mutexes;

// WIN32_FIND_DATAA -> WIN32_FIND_DATAW: the 44 bytes of attributes, times and
// sizes, then the names widened.
void widen_find_data(uint32_t a, uint32_t w) {
    std::memcpy(gptr(w), gptr(a), 44);
    put_wstr(w + 44, 260, utf16(gstr(a + 44)));
    put_wstr(w + 44 + 520, 14, utf16(gstr(a + 44 + 260)));
}

// ---- resources: the executable's .rsrc tree, read where it is loaded ----

uint32_t resource_root() {
    static const uint32_t root = [] {
        const recomp::Image& img = *process().image;
        const auto& d = img.data();
        const uint32_t pe = *(const uint32_t*)(d.data() + 0x3c);
        // Optional header at pe + 24; the data directories at +96 (PE32), the
        // resource directory is the third.
        const uint32_t rva = *(const uint32_t*)(d.data() + pe + 24 + 96 + 2 * 8);
        return rva ? img.image_base() + rva : 0u;
    }();
    return root;
}

std::u16string upper16(std::u16string s) {
    for (char16_t& ch : s)
        if (ch >= u'a' && ch <= u'z') ch = (char16_t)(ch - 32);
    return s;
}

// The entry of directory `dir` named by `key` (a MAKEINTRESOURCE id, "#id" or
// a name; 0xffffffff takes the first entry); its OffsetToData, or 0.
uint32_t resource_entry(uint32_t dir, uint32_t key) {
    const uint32_t root = resource_root();
    const uint32_t named = rd16(dir + 12), ids = rd16(dir + 14);
    std::u16string name;
    uint32_t id = key;
    if (key != 0xffffffffu && key >= 0x10000) {
        const std::string s = gstr(key);
        if (!s.empty() && s[0] == '#') id = (uint32_t)std::strtoul(s.c_str() + 1, nullptr, 10);
        else name = upper16(utf16(s)), id = 0xfffffffeu;
    }
    for (uint32_t k = 0; k < named + ids; k++) {
        const uint32_t e = dir + 16 + 8 * k, n = rd(e), off = rd(e + 4);
        if (key == 0xffffffffu) return off;
        if (n & 0x80000000u) {
            if (name.empty()) continue;
            const uint32_t s = root + (n & 0x7fffffffu);
            std::u16string have(rd16(s), u'\0');
            for (size_t i = 0; i < have.size(); i++) have[i] = (char16_t)rd16(s + 2 + 2 * (uint32_t)i);
            if (upper16(have) == name) return off;
        } else if (n == id) {
            return off;
        }
    }
    return 0;
}

}  // namespace
}  // namespace scimitar::win32

namespace scimitar::imports {
using namespace scimitar::win32;

// ---- KERNEL32 ----

// CreateMutexA(attributes, initial owner, name): a name already used opens the
// same mutex (ERROR_ALREADY_EXISTS, how a game finds it runs twice).
W32(kernel32, CreateMutexA) {
    const bool own = arg(c, 1) != 0;
    const std::string name = arg(c, 2) ? gstr(arg(c, 2)) : "";
    std::shared_ptr<Mutex> m;
    bool existed = false;
    if (!name.empty()) {
        std::lock_guard<std::mutex> l(g_names_lock);
        auto& slot = g_named_mutexes[name];
        existed = slot != nullptr;
        if (!slot) slot = std::make_shared<Mutex>();
        m = slot;
    } else {
        m = std::make_shared<Mutex>();
    }
    if (own && !existed) m->try_acquire();
    trace("CreateMutexA(%s)%s", name.c_str(), existed ? ": exists" : "");
    const uint32_t h = add_handle(m);
    set_last_error(existed ? 183 : 0);  // ERROR_ALREADY_EXISTS
    ret(c, h, 3);
}

W32(kernel32, ReleaseMutex) {
    auto m = handle_as<Mutex>(arg(c, 0));
    bool ok = false;
    if (m) {
        std::lock_guard<std::mutex> l(m->lock);
        if (m->count && m->owner == current_thread_id()) ok = true, m->count--;
    }
    if (ok) notify_waiters();
    else set_last_error(288);  // ERROR_NOT_OWNER
    ret(c, ok, 1);
}

W32(kernel32, FindFirstFileW) {
    const std::string pattern = utf8(gwstr(arg(c, 0)));
    const uint32_t out = arg(c, 1), esp = c.esp;
    const uint32_t p = scratch(c, (uint32_t)pattern.size() + 1);
    put_str(p, (uint32_t)pattern.size() + 1, pattern);
    const uint32_t a = scratch(c, 320);
    const uint32_t h = call_shim(c, imp_kernel32_FindFirstFileA, {p, a});
    if (h != 0xffffffffu) widen_find_data(a, out);
    c.esp = esp;
    ret(c, h, 2);
}

W32(kernel32, FindNextFileW) {
    const uint32_t h = arg(c, 0), out = arg(c, 1), esp = c.esp;
    const uint32_t a = scratch(c, 320);
    const uint32_t r = call_shim(c, imp_kernel32_FindNextFileA, {h, a});
    if (r) widen_find_data(a, out);
    c.esp = esp;
    ret(c, r, 2);
}

W32(kernel32, GetModuleFileNameW) {
    const uint32_t module = arg(c, 0), out = arg(c, 1), size = arg(c, 2), esp = c.esp;
    const uint32_t a = scratch(c, 520);
    call_shim(c, imp_kernel32_GetModuleFileNameA, {module, a, 520});
    const uint32_t n = put_wstr(out, size, utf16(gstr(a)));
    c.esp = esp;
    ret(c, n, 3);
}

// GetDiskFreeSpaceExW(dir, available to the caller, total, free): plenty.
W32(kernel32, GetDiskFreeSpaceExW) {
    const uint64_t free = 200ull << 30, total = 500ull << 30;
    auto put64 = [](uint32_t a, uint64_t v) {
        if (a) wr(a, (uint32_t)v), wr(a + 4, (uint32_t)(v >> 32));
    };
    put64(arg(c, 1), free), put64(arg(c, 2), total), put64(arg(c, 3), free);
    ret(c, 1, 4);
}

// FindResourceA(module, name, type): the language level's first entry; the
// handle is the data entry's guest address {RVA, size, code page, 0}.
W32(kernel32, FindResourceA) {
    const uint32_t name = arg(c, 1), type = arg(c, 2), root = resource_root();
    uint32_t r = 0;
    if (root) {
        uint32_t off = resource_entry(root, type);
        if (off & 0x80000000u) off = resource_entry(root + (off & 0x7fffffffu), name);
        else off = 0;
        if (off & 0x80000000u) off = resource_entry(root + (off & 0x7fffffffu), 0xffffffffu);
        else off = 0;
        if (off && !(off & 0x80000000u)) r = root + off;
    }
    trace("FindResourceA(%s, %s) = %08x", name >= 0x10000 ? gstr(name).c_str() : std::to_string(name).c_str(),
          type >= 0x10000 ? gstr(type).c_str() : std::to_string(type).c_str(), r);
    if (!r) set_last_error(1814);  // ERROR_RESOURCE_NAME_NOT_FOUND
    ret(c, r, 3);
}
W32(kernel32, LoadResource) {
    const uint32_t h = arg(c, 1);
    ret(c, h ? process().image->image_base() + rd(h) : 0, 2);
}
W32(kernel32, LockResource) { ret(c, arg(c, 0), 1); }
W32(kernel32, SizeofResource) {
    const uint32_t h = arg(c, 1);
    ret(c, h ? rd(h + 4) : 0, 2);
}

// ---- GDI32, COMCTL32, SHELL32, OLEAUT32, WININET ----

W32(gdi32, GetStockObject) { ret(c, 0x10090 + arg(c, 0), 1); }
W32(comctl32, InitCommonControlsEx) { ret(c, 1, 1); }

W32(shell32, SHGetFolderPathW) {
    const uint32_t hwnd = arg(c, 0), csidl = arg(c, 1), out = arg(c, 4), esp = c.esp;
    const uint32_t path = scratch(c, 520);
    const uint32_t r = call_shim(c, imp_shell32_SHGetSpecialFolderPathA, {hwnd, path, csidl, 1});
    if (r) put_wstr(out, 260, utf16(gstr(path)));
    c.esp = esp;
    ret(c, r ? 0 : 0x80004005u, 5);  // S_OK / E_FAIL
}

// BSTRs are SysAllocString's (system.cpp): a length word before the text.
W32(oleaut32, SysFreeString) {
    if (arg(c, 0)) gfree(arg(c, 0) - 4);
    ret(c, 0, 1);
}
W32(oleaut32, VariantClear) {
    if (arg(c, 0)) wr16(arg(c, 0), 0);  // VT_EMPTY
    ret(c, 0, 1);
}

W32(wininet, InternetGetCookieA) { ret(c, 0, 4); }
W32(wininet, InternetSetCookieA) { ret(c, 0, 3); }

// ---- D3DX 9.39 ----

W32(d3dx9_39, D3DXGetShaderConstantTable) { imp_d3dx9_36_D3DXGetShaderConstantTable(c); }

// D3DXCreateTexture(device, width, height, levels, usage, format, pool, &texture):
// the device's CreateTexture (IDirect3DDevice9 slot 23) with D3DX's defaults
// (D3DX_DEFAULT sizes: 256; levels 0 or D3DX_DEFAULT: the full chain).
W32(d3dx9_39, D3DXCreateTexture) {
    const uint32_t dev = arg(c, 0), usage = arg(c, 4), format = arg(c, 5), pool = arg(c, 6), out = arg(c, 7);
    uint32_t w = arg(c, 1), h = arg(c, 2), levels = arg(c, 3);
    if (!w || w == 0xffffffffu) w = 256;
    if (!h || h == 0xffffffffu) h = 256;
    if (levels == 0xffffffffu) levels = 0;
    const uint32_t create = rd(rd(dev) + 23 * 4);
    const uint32_t hr = call_guest(c, create, {dev, w, h, levels, usage, format, pool, out, 0});
    if (hr) trace("D3DXCreateTexture(%ux%u, format %u, pool %u) failed: %08x", w, h, format, pool, hr);
    ret(c, hr, 8);
}

// D3DXFloat16To32Array(float* out, const D3DXFLOAT16* in, UINT n): returns out.
// As D3DX, the largest exponent is a number like the others (no infinities).
W32(d3dx9_39, D3DXFloat16To32Array) {
    const uint32_t out = arg(c, 0), in = arg(c, 1), n = arg(c, 2);
    for (uint32_t k = 0; k < n; k++) {
        const uint16_t hbits = rd16(in + 2 * k);
        const uint32_t sign = (uint32_t)(hbits & 0x8000) << 16;
        const int exp = (hbits >> 10) & 0x1f;
        const uint32_t man = hbits & 0x3ff;
        float f;
        if (exp == 0) f = std::ldexp((float)man, -24);
        else f = std::ldexp((float)(man | 0x400), exp - 25);
        uint32_t bits;
        std::memcpy(&bits, &f, 4);
        wr(out + 4 * k, bits | sign);
    }
    ret(c, out, 3);
}

}  // namespace scimitar::imports
