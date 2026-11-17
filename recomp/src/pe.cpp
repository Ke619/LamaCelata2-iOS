// PE32 executables (the Windows releases): sections, imports, exports, TLS and
// the vtables MSVC's RTTI describes. Function starts come later, from
// discover_functions (flow.cpp): a PE carries no symbols.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

#include "macho.h"

namespace recomp {

namespace {

[[noreturn]] void fail(const std::string& msg) {
    std::fprintf(stderr, "pe: %s\n", msg.c_str());
    std::exit(1);
}

uint32_t u32(const std::vector<uint8_t>& d, size_t off) {
    if (off + 4 > d.size()) fail("truncated file");
    uint32_t v;
    std::memcpy(&v, d.data() + off, 4);
    return v;
}

uint16_t u16(const std::vector<uint8_t>& d, size_t off) {
    if (off + 2 > d.size()) fail("truncated file");
    uint16_t v;
    std::memcpy(&v, d.data() + off, 2);
    return v;
}

// Names of the ordinal imports the games use, from the DLLs' export tables.
const char* ordinal_name(const std::string& dll_any_case, uint32_t ord) {
    std::string dll;
    for (char ch : dll_any_case) dll += (char)std::tolower((unsigned char)ch);
    struct Entry { const char* dll; uint32_t ord; const char* name; };
    static const Entry kNames[] = {
        {"ws2_32.dll", 1, "accept"}, {"ws2_32.dll", 2, "bind"}, {"ws2_32.dll", 3, "closesocket"},
        {"ws2_32.dll", 4, "connect"}, {"ws2_32.dll", 5, "getpeername"},
        {"ws2_32.dll", 6, "getsockname"}, {"ws2_32.dll", 7, "getsockopt"},
        {"ws2_32.dll", 8, "htonl"}, {"ws2_32.dll", 9, "htons"}, {"ws2_32.dll", 10, "ioctlsocket"},
        {"ws2_32.dll", 11, "inet_addr"}, {"ws2_32.dll", 12, "inet_ntoa"},
        {"ws2_32.dll", 13, "listen"}, {"ws2_32.dll", 14, "ntohl"}, {"ws2_32.dll", 15, "ntohs"},
        {"ws2_32.dll", 16, "recv"}, {"ws2_32.dll", 17, "recvfrom"}, {"ws2_32.dll", 18, "select"},
        {"ws2_32.dll", 19, "send"}, {"ws2_32.dll", 20, "sendto"}, {"ws2_32.dll", 21, "setsockopt"},
        {"ws2_32.dll", 22, "shutdown"}, {"ws2_32.dll", 23, "socket"},
        {"ws2_32.dll", 51, "gethostbyaddr"}, {"ws2_32.dll", 52, "gethostbyname"},
        {"ws2_32.dll", 57, "gethostname"}, {"ws2_32.dll", 101, "WSAAsyncSelect"},
        {"ws2_32.dll", 111, "WSAGetLastError"}, {"ws2_32.dll", 112, "WSASetLastError"},
        {"ws2_32.dll", 115, "WSAStartup"}, {"ws2_32.dll", 116, "WSACleanup"},
        {"ws2_32.dll", 151, "__WSAFDIsSet"},
        {"dsound.dll", 1, "DirectSoundCreate"}, {"dsound.dll", 2, "DirectSoundEnumerateA"},
        {"dsound.dll", 3, "DirectSoundEnumerateW"}, {"dsound.dll", 6, "DirectSoundCaptureCreate"},
        {"dsound.dll", 11, "DirectSoundCreate8"}, {"dsound.dll", 12, "DirectSoundCaptureCreate8"},
        {"xinput1_3.dll", 2, "XInputGetState"}, {"xinput1_3.dll", 3, "XInputSetState"},
        {"xinput1_3.dll", 4, "XInputGetCapabilities"}, {"xinput1_3.dll", 5, "XInputEnable"},
        {"xinput1_3.dll", 7, "XInputGetBatteryInformation"},
        {"xinput1_3.dll", 8, "XInputGetKeystroke"},
        {"eax.dll", 5, "EAXDirectSoundCreate"}, {"eax.dll", 6, "EAXDirectSoundCreate8"},
        {"eax.dll", 7, "GetCurrentVersion"},
        {"oleaut32.dll", 2, "SysAllocString"}, {"oleaut32.dll", 6, "SysFreeString"},
        {"oleaut32.dll", 7, "SysStringLen"}, {"oleaut32.dll", 9, "VariantClear"},
    };
    for (const Entry& e : kNames)
        if (dll == e.dll && ord == e.ord) return e.name;
    return nullptr;
}

}  // namespace

void Image::load_pe() {
    pe_ = true;
    const uint32_t pe = u32(data_, 0x3c);
    if (u32(data_, pe) != 0x00004550) fail("no PE signature");
    if (u16(data_, pe + 4) != 0x14c) fail("not an i386 PE");
    const uint32_t nsec = u16(data_, pe + 6), optsize = u16(data_, pe + 20);
    const size_t opt = pe + 24;
    if (u16(data_, opt) != 0x10b) fail("not PE32");
    image_base_ = u32(data_, opt + 28);
    entry_ = image_base_ + u32(data_, opt + 16);
    const uint32_t ndirs = u32(data_, opt + 92);
    auto dir = [&](uint32_t k, uint32_t* rva, uint32_t* size) {
        *rva = k < ndirs ? u32(data_, opt + 96 + 8 * k) : 0;
        *size = k < ndirs ? u32(data_, opt + 100 + 8 * k) : 0;
    };

    for (uint32_t k = 0; k < nsec; k++) {
        const size_t p = opt + optsize + 40 * k;
        Section sec;
        sec.sectname.assign(reinterpret_cast<const char*>(data_.data() + p),
                            strnlen(reinterpret_cast<const char*>(data_.data() + p), 8));
        const uint32_t vsize = u32(data_, p + 8), rva = u32(data_, p + 12);
        const uint32_t rawsize = u32(data_, p + 16), rawoff = u32(data_, p + 20);
        sec.flags = u32(data_, p + 36);
        sec.addr = image_base_ + rva;
        sec.size = vsize ? vsize : rawsize;
        sec.offset = rawoff;
        // IMAGE_SCN_MEM_WRITE
        sec.segname = sec.flags & 0x80000000u ? "__DATA" : "__TEXT";
        sections_.push_back(sec);
        Segment seg;
        seg.name = sec.sectname;
        seg.vmaddr = sec.addr;
        seg.vmsize = (sec.size + 0xfff) & ~0xfffu;
        seg.fileoff = rawoff;
        seg.filesize = std::min(rawsize, sec.size);
        if ((uint64_t)seg.fileoff + seg.filesize > data_.size())
            seg.filesize = data_.size() > seg.fileoff ? (uint32_t)(data_.size() - seg.fileoff) : 0;
        seg.initprot = 1 | (sec.flags & 0x80000000u ? 2 : 0) | (sec.flags & 0x20000000u ? 4 : 0);
        segments_.push_back(seg);
    }
    // The headers are mapped too: the CRT reads them (`__ImageBase`, section
    // checks for the SEH and TLS code).
    {
        Segment hdr;
        hdr.name = "PE headers";
        hdr.vmaddr = image_base_;
        hdr.vmsize = (u32(data_, opt + 60) + 0xfff) & ~0xfffu;  // SizeOfHeaders
        hdr.filesize = std::min<uint32_t>(hdr.vmsize, (uint32_t)data_.size());
        hdr.initprot = 1;
        segments_.insert(segments_.begin(), hdr);
    }
    for (const Section& s : sections_)
        if (s.sectname == ".text") text_ = &s;
    if (!text_) fail("no .text section");

    auto file_offset = [&](uint32_t va) -> size_t {
        for (const Segment& s : segments_)
            if (va >= s.vmaddr && va < s.vmaddr + s.filesize) return s.fileoff + (va - s.vmaddr);
        fail("address outside the file data");
    };
    auto cstr = [&](uint32_t va) {
        return std::string(reinterpret_cast<const char*>(data_.data() + file_offset(va)));
    };

    // Imports: each IAT slot is an import "stub" (calls go through it).
    uint32_t rva, size;
    dir(1, &rva, &size);
    for (uint32_t d = image_base_ + rva; rva; d += 20) {
        const uint32_t ilt = u32(data_, file_offset(d)), name = u32(data_, file_offset(d + 12));
        const uint32_t iat = u32(data_, file_offset(d + 16));
        if (!name) break;
        const std::string dll = cstr(image_base_ + name);
        for (uint32_t k = 0;; k++) {
            const uint32_t e = u32(data_, file_offset(image_base_ + (ilt ? ilt : iat) + 4 * k));
            if (!e) break;
            // Named "dll!function" with the DLL's base name in lower case
            // ("kernel32!CreateFileA"), so Windows shims never collide with the
            // Mac ones (ws2_32's socket is stdcall, libc's cdecl).
            Import imp;
            imp.dll = dll;
            std::string base;
            for (char ch : dll.substr(0, dll.rfind('.'))) base += (char)std::tolower((unsigned char)ch);
            if (e & 0x80000000u) {
                const uint32_t ord = e & 0xffff;
                const char* n = ordinal_name(dll, ord);
                imp.name = base + "!" + (n ? std::string(n) : "ord" + std::to_string(ord));
            } else {
                imp.name = base + "!" + cstr(image_base_ + e + 2);
            }
            stubs_[image_base_ + iat + 4 * k] = imp;
        }
    }

    // Exports (the game exports some of its sound engine's classes).
    dir(0, &rva, &size);
    if (rva) {
        const size_t e = file_offset(image_base_ + rva);
        const uint32_t nfuncs = u32(data_, e + 20), nnames = u32(data_, e + 24);
        const uint32_t funcs = u32(data_, e + 28), names = u32(data_, e + 32),
                       ords = u32(data_, e + 36);
        std::set<uint32_t> named;
        for (uint32_t k = 0; k < nnames; k++) {
            const uint32_t n = u32(data_, file_offset(image_base_ + names + 4 * k));
            const uint16_t o = u16(data_, file_offset(image_base_ + ords + 2 * k));
            if (o >= nfuncs) continue;
            const uint32_t f = u32(data_, file_offset(image_base_ + funcs + 4 * o));
            exports_.push_back({cstr(image_base_ + n), image_base_ + f});
            named.insert(o);
        }
        for (uint32_t o = 0; o < nfuncs; o++)
            if (!named.count(o))
                if (const uint32_t f = u32(data_, file_offset(image_base_ + funcs + 4 * o)))
                    exports_.push_back({"ordinal_" + std::to_string(o), image_base_ + f});
    }

    // Base relocations: which words hold addresses (the image is loaded at
    // its preferred base, so none is applied).
    dir(5, &rva, &size);
    if (rva) {
        const size_t end = file_offset(image_base_ + rva) + size;
        for (size_t b = file_offset(image_base_ + rva); b + 8 <= end;) {
            const uint32_t page = u32(data_, b), block = u32(data_, b + 4);
            if (block < 8) break;
            for (uint32_t k = 0; k < (block - 8) / 2; k++) {
                const uint16_t e = u16(data_, b + 8 + 2 * k);
                if (e >> 12 != 3) continue;  // HIGHLOW; the rest is padding
                const uint32_t site = image_base_ + page + (e & 0xfff);
                if (const uint8_t* q = at(site, 4)) {
                    uint32_t v;
                    std::memcpy(&v, q, 4);
                    relocations_[site] = v;
                }
            }
            b += block;
        }
    }

    // Static TLS.
    dir(9, &rva, &size);
    if (rva) {
        const size_t t = file_offset(image_base_ + rva);
        tls_.start = u32(data_, t);
        tls_.end = u32(data_, t + 4);
        tls_.index_addr = u32(data_, t + 8);
        tls_.callbacks = u32(data_, t + 12);
        tls_.zero_fill = u32(data_, t + 16);
    }

    // RTTI vtables: a word in read-only data pointing to a complete object
    // locator (signature 0, a type descriptor whose name starts ".?A", a class
    // hierarchy descriptor in read-only data) is the slot before a vtable,
    // which runs while its words point into code.
    auto word = [&](uint32_t va, uint32_t* v) {
        const uint8_t* p = at(va, 4);
        if (!p) return false;
        std::memcpy(v, p, 4);
        return true;
    };
    auto in_readonly = [&](uint32_t va) {
        for (const Section& s : sections_)
            if (s.segname == "__TEXT" && !(s.flags & 0x20000000u) && va >= s.addr &&
                va < s.addr + s.size)
                return true;
        return false;
    };
    auto type_name = [&](uint32_t td) -> std::string {
        const uint8_t* p = at(td + 8, 4);
        if (!p || std::memcmp(p, ".?A", 3) != 0) return "";
        const uint8_t* q = at(td + 8, 1);
        std::string n;
        for (uint32_t k = 0; k < 512 && (q = at(td + 8 + k, 1)) && *q; k++) n += (char)*q;
        return n;
    };
    std::map<uint32_t, std::string> locators;
    for (const Section& s : sections_) {
        if (s.segname != "__TEXT" || (s.flags & 0x20000000u)) continue;
        for (uint32_t a = s.addr; a + 20 <= s.addr + s.size; a += 4) {
            uint32_t sig, td, chd;
            if (!word(a, &sig) || sig != 0 || !word(a + 12, &td) || !word(a + 16, &chd)) continue;
            if (!in_readonly(chd)) continue;
            const std::string n = type_name(td);
            if (!n.empty()) locators[a] = n;
        }
    }
    for (const Section& s : sections_) {
        if (s.segname != "__TEXT" || (s.flags & 0x20000000u)) continue;
        for (uint32_t a = s.addr; a + 8 <= s.addr + s.size; a += 4) {
            uint32_t col;
            if (!word(a, &col)) continue;
            auto it = locators.find(col);
            if (it == locators.end()) continue;
            Vtable v;
            v.addr = a + 4;
            v.type = it->second;
            for (uint32_t p = v.addr, f; word(p, &f) && code_section(f); p += 4) v.slots.push_back(f);
            if (v.slots.empty()) continue;
            vtable_at_[v.addr] = vtables_.size();
            vtables_.push_back(std::move(v));
        }
    }
}

}  // namespace recomp
