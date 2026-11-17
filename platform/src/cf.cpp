// CoreFoundation imports on the bridge's objects (bridge.h), and the Carbon
// file and process calls the game uses next to them (FSRef paths, process
// bundle location). Strings are UTF-8 inside; lengths are in UTF-16 units as
// CF reports them only for ASCII text, which is what the game passes.
#include <CoreFoundation/CoreFoundation.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "bridge.h"
#include "scimitar/guest_heap.h"
#include "scimitar/imports.h"
#include "scimitar/runtime.h"

namespace scimitar::imports {

namespace {

using bridge::Kind;
using bridge::Object;

// CFTypeIDs the game compares against (any distinct values work).
uint32_t type_id(Kind k) {
    switch (k) {
    case Kind::String: return 7;
    case Kind::Dictionary: return 17;
    case Kind::Array: return 18;
    case Kind::Data: return 20;
    case Kind::Boolean: return 21;
    case Kind::Number: return 22;
    case Kind::URL: return 29;
    case Kind::Bundle: return 31;
    case Kind::Date: return 42;
    case Kind::Locale: return 43;
    case Kind::HIDDevice: return 60;
    default: return 1;
    }
}

uint32_t make_url(Context& c, const std::string& path) {
    Object o;
    o.str = path;
    return bridge::make(c, Kind::URL, "NSURL", std::move(o));
}

uint32_t guest_bytes(Context& c, const void* p, size_t n) {
    const uint32_t a = heap::alloc(*c.mem, (uint32_t)n + 1);
    std::memcpy(c.mem->ptr(a), p, n);
    c.mem->write<uint8_t>(a + (uint32_t)n, 0);
    return a;
}

// Decodes bytes in a CFStringEncoding to UTF-8 (UTF-8, ASCII, MacRoman and
// Latin-1, UTF-16 and UTF-32 little endian).
std::string decode(const uint8_t* p, size_t n, uint32_t enc) {
    CFStringRef s = CFStringCreateWithBytes(nullptr, p, (CFIndex)n, enc, false);
    if (!s) return std::string((const char*)p, n);
    const CFIndex max = CFStringGetMaximumSizeForEncoding(CFStringGetLength(s), kCFStringEncodingUTF8) + 1;
    std::string out((size_t)max, '\0');
    CFStringGetCString(s, out.data(), max, kCFStringEncodingUTF8);
    CFRelease(s);
    out.resize(std::strlen(out.c_str()));
    return out;
}

// Encodes UTF-8 to a CFStringEncoding.
std::vector<uint8_t> encode(const std::string& s, uint32_t enc, uint8_t loss) {
    std::vector<uint8_t> out;
    CFStringRef cs = CFStringCreateWithBytes(nullptr, (const UInt8*)s.data(), (CFIndex)s.size(),
                                             kCFStringEncodingUTF8, false);
    if (!cs) return out;
    CFIndex used = 0;
    const CFRange r = CFRangeMake(0, CFStringGetLength(cs));
    CFStringGetBytes(cs, r, enc, loss, false, nullptr, 0, &used);
    out.resize((size_t)used);
    CFStringGetBytes(cs, r, enc, loss, false, out.data(), used, &used);
    CFRelease(cs);
    return out;
}

std::string info_plist_path(Context& c, uint32_t bundle) {
    return bridge::expect(c, bundle, Kind::Bundle, "CFBundle").str + "/Contents/Info.plist";
}

uint32_t read_plist_bytes(Context& c, const uint8_t* p, size_t n) {
    CFDataRef d = CFDataCreate(nullptr, p, (CFIndex)n);
    CFPropertyListRef pl = CFPropertyListCreateWithData(nullptr, d, kCFPropertyListImmutable, nullptr, nullptr);
    CFRelease(d);
    if (!pl) return 0;
    const uint32_t r = bridge::from_plist(c, pl);
    CFRelease(pl);
    return r;
}

// ---- FSRef: an opaque 80-byte value; ours holds an index into this table ----
std::mutex g_fs_lock;
std::vector<std::string> g_fs_paths;
constexpr uint32_t kFSRefMagic = 0x46535246;  // "FSRF"

void put_fsref(Context& c, uint32_t ref, const std::string& path) {
    std::lock_guard<std::mutex> l(g_fs_lock);
    std::memset(c.mem->ptr(ref), 0, 80);
    c.mem->write<uint32_t>(ref, kFSRefMagic);
    c.mem->write<uint32_t>(ref + 4, (uint32_t)g_fs_paths.size());
    g_fs_paths.push_back(path);
}
}  // namespace

std::string fsref_path(Context& c, uint32_t ref) {
    std::lock_guard<std::mutex> l(g_fs_lock);
    if (c.mem->read<uint32_t>(ref) != kFSRefMagic) fatal("not an FSRef made by FSPathMakeRef: %08x", ref);
    return g_fs_paths.at(c.mem->read<uint32_t>(ref + 4));
}

// ---- allocators ----
// void* CFAllocatorAllocate(CFAllocatorRef, CFIndex size, CFOptionFlags)
void imp_CFAllocatorAllocate(Context& c) { shim_return(c, heap::alloc(*c.mem, arg32(c, 1))); }
void imp_CFAllocatorDeallocate(Context& c) {
    heap::release(*c.mem, arg32(c, 1));
    shim_return(c, 0);
}

// ---- type, memory ----
void imp_CFGetTypeID(Context& c) {
    const Object* o = bridge::get(c, arg32(c, 0));
    shim_return(c, o ? type_id(o->kind) : 0);
}
void imp_CFStringGetTypeID(Context& c) { shim_return(c, type_id(Kind::String)); }
void imp_CFArrayGetTypeID(Context& c) { shim_return(c, type_id(Kind::Array)); }
void imp_CFDictionaryGetTypeID(Context& c) { shim_return(c, type_id(Kind::Dictionary)); }
void imp_CFNumberGetTypeID(Context& c) { shim_return(c, type_id(Kind::Number)); }
void imp_CFRelease(Context& c) {
    bridge::release(c, arg32(c, 0));
    shim_return(c, 0);
}
void imp_CFShow(Context& c) {
    std::fprintf(stderr, "[game CFShow] %s\n", bridge::text(c, arg32(c, 0)).c_str());
    shim_return(c, 0);
}

// ---- arrays, sets, dictionaries, booleans, numbers, data ----
void imp_CFArrayGetCount(Context& c) {
    shim_return(c, (uint32_t)bridge::expect(c, arg32(c, 0), Kind::Array, "CFArrayGetCount").items.size());
}
void imp_CFArrayGetValueAtIndex(Context& c) {
    const Object& a = bridge::expect(c, arg32(c, 0), Kind::Array, "CFArrayGetValueAtIndex");
    if (arg32(c, 1) >= a.items.size()) fatal("CFArrayGetValueAtIndex: index %u out of range", arg32(c, 1));
    shim_return(c, a.items[arg32(c, 1)]);
}
// Sets are arrays here (from IOKit device lists).
void imp_CFSetGetCount(Context& c) {
    shim_return(c, (uint32_t)bridge::expect(c, arg32(c, 0), Kind::Array, "CFSetGetCount").items.size());
}
void imp_CFSetGetValues(Context& c) {
    const Object& a = bridge::expect(c, arg32(c, 0), Kind::Array, "CFSetGetValues");
    for (size_t k = 0; k < a.items.size(); k++) c.mem->write<uint32_t>(arg32(c, 1) + 4 * (uint32_t)k, a.items[k]);
    shim_return(c, 0);
}
void imp_CFBooleanGetValue(Context& c) {
    const Object* o = bridge::get(c, arg32(c, 0));
    shim_return(c, o && o->number != 0 ? 1 : 0);
}
void imp_CFDictionaryCreateMutable(Context& c) {
    Object d;
    d.mutable_ = true;
    shim_return(c, bridge::make(c, Kind::Dictionary, "NSMutableDictionary", std::move(d)));
}
void imp_CFDictionaryGetValue(Context& c) {
    const Object& d = bridge::expect(c, arg32(c, 0), Kind::Dictionary, "CFDictionaryGetValue");
    for (const auto& [k, v] : d.pairs)
        if (bridge::equal(c, k, arg32(c, 1))) return shim_return(c, v);
    shim_return(c, 0);
}
void imp_CFDictionarySetValue(Context& c) {
    Object& d = bridge::expect(c, arg32(c, 0), Kind::Dictionary, "CFDictionarySetValue");
    for (auto& [k, v] : d.pairs)
        if (bridge::equal(c, k, arg32(c, 1))) {
            v = arg32(c, 2);
            return shim_return(c, 0);
        }
    d.pairs.emplace_back(arg32(c, 1), arg32(c, 2));
    shim_return(c, 0);
}
void imp_CFDictionaryContainsValue(Context& c) {
    const Object& d = bridge::expect(c, arg32(c, 0), Kind::Dictionary, "CFDictionaryContainsValue");
    for (const auto& p : d.pairs)
        if (bridge::equal(c, p.second, arg32(c, 1))) return shim_return(c, 1);
    shim_return(c, 0);
}
// CFNumberCreate(CFAllocatorRef, CFNumberType, const void* value)
void imp_CFNumberCreate(Context& c) {
    const uint32_t type = arg32(c, 1), p = arg32(c, 2);
    double v = 0;
    bool integer = true;
    switch (type) {
    case 1: case 7: v = (int8_t)c.mem->read<uint8_t>(p); break;
    case 2: case 8: v = (int16_t)c.mem->read<uint16_t>(p); break;
    case 3: case 9: case 10: case 14: v = (int32_t)c.mem->read<uint32_t>(p); break;
    case 4: case 11: v = (double)(int64_t)c.mem->read<uint64_t>(p); break;
    case 5: case 12: v = c.mem->read<float>(p), integer = false; break;
    case 6: case 13: v = c.mem->read<double>(p), integer = false; break;
    default: fatal("CFNumberCreate: number type %u", type);
    }
    shim_return(c, bridge::make_number(c, v, integer));
}
// Boolean CFNumberGetValue(CFNumberRef, CFNumberType, void* value)
void imp_CFNumberGetValue(Context& c) {
    const Object* o = bridge::get(c, arg32(c, 0));
    const double v = o ? o->number : 0;
    const uint32_t type = arg32(c, 1), p = arg32(c, 2);
    switch (type) {
    case 1: case 7: c.mem->write<uint8_t>(p, (uint8_t)(int8_t)v); break;
    case 2: case 8: c.mem->write<uint16_t>(p, (uint16_t)(int16_t)v); break;
    case 3: case 9: case 10: case 14: c.mem->write<uint32_t>(p, (uint32_t)(int32_t)v); break;
    case 4: case 11: c.mem->write<uint64_t>(p, (uint64_t)(int64_t)v); break;
    case 5: case 12: c.mem->write<float>(p, (float)v); break;
    case 6: case 13: c.mem->write<double>(p, v); break;
    default: fatal("CFNumberGetValue: number type %u", type);
    }
    shim_return(c, o ? 1 : 0);
}
void imp_CFDataCreate(Context& c) {
    Object d;
    d.bytes.assign(c.mem->ptr(arg32(c, 1)), c.mem->ptr(arg32(c, 1)) + arg32(c, 2));
    shim_return(c, bridge::make(c, Kind::Data, "NSData", std::move(d)));
}
// The bytes live in guest memory once asked for (kept with the object).
void imp_CFDataGetBytePtr(Context& c) {
    Object& d = bridge::expect(c, arg32(c, 0), Kind::Data, "CFDataGetBytePtr");
    if (!d.cstr) d.cstr = guest_bytes(c, d.bytes.data(), d.bytes.size());
    shim_return(c, d.cstr);
}
// void CFDataGetBytes(CFDataRef, CFRange {location, length}, UInt8* buffer)
void imp_CFDataGetBytes(Context& c) {
    const Object& d = bridge::expect(c, arg32(c, 0), Kind::Data, "CFDataGetBytes");
    const uint32_t loc = arg32(c, 1), len = arg32(c, 2);
    if ((uint64_t)loc + len > d.bytes.size()) fatal("CFDataGetBytes: range outside the data");
    std::memcpy(c.mem->ptr(arg32(c, 3)), d.bytes.data() + loc, len);
    shim_return(c, 0);
}
// CFPropertyListRef CFPropertyListCreateFromXMLData(CFAllocatorRef, CFDataRef, CFOptionFlags, CFStringRef* error)
void imp_CFPropertyListCreateFromXMLData(Context& c) {
    const Object& d = bridge::expect(c, arg32(c, 1), Kind::Data, "CFPropertyListCreateFromXMLData");
    if (arg32(c, 3)) c.mem->write<uint32_t>(arg32(c, 3), 0);
    shim_return(c, read_plist_bytes(c, d.bytes.data(), d.bytes.size()));
}

// ---- strings ----
// CFStringRef CFStringCreateWithBytes(CFAllocatorRef, const UInt8*, CFIndex, CFStringEncoding, Boolean)
void imp_CFStringCreateWithBytes(Context& c) {
    shim_return(c, bridge::make_string(c, decode(c.mem->ptr(arg32(c, 1)), arg32(c, 2), arg32(c, 3))));
}
void imp_CFStringCreateWithFileSystemRepresentation(Context& c) {
    shim_return(c, bridge::make_string(c, (const char*)c.mem->ptr(arg32(c, 1))));
}
// CFStringRef CFStringCreateWithFormat(CFAllocatorRef, CFDictionaryRef, CFStringRef format, ...)
void imp_CFStringCreateWithFormat(Context& c) {
    Object& f = bridge::expect(c, arg32(c, 2), Kind::String, "CFStringCreateWithFormat");
    shim_return(c, bridge::make_string(c, guest_format(*c.mem, bridge::utf8(c, f), c.esp + 16, false)));
}
// CFStringRef CFStringCreateWithFormatAndArguments(CFAllocatorRef, CFDictionaryRef, CFStringRef, va_list)
void imp_CFStringCreateWithFormatAndArguments(Context& c) {
    Object& f = bridge::expect(c, arg32(c, 2), Kind::String, "CFStringCreateWithFormatAndArguments");
    shim_return(c, bridge::make_string(c, guest_format(*c.mem, bridge::utf8(c, f), arg32(c, 3), false)));
}
void imp_CFStringGetLength(Context& c) { shim_return(c, (uint32_t)bridge::text(c, arg32(c, 0)).size()); }
// Boolean CFStringGetCString(CFStringRef, char* buffer, CFIndex size, CFStringEncoding)
void imp_CFStringGetCString(Context& c) {
    const std::vector<uint8_t> b = encode(bridge::text(c, arg32(c, 0)), arg32(c, 3), 0);
    if (b.size() + 1 > arg32(c, 2)) return shim_return(c, 0);
    std::memcpy(c.mem->ptr(arg32(c, 1)), b.data(), b.size());
    c.mem->write<uint8_t>(arg32(c, 1) + (uint32_t)b.size(), 0);
    shim_return(c, 1);
}
// const char* CFStringGetCStringPtr(CFStringRef, CFStringEncoding): the
// string's bytes, kept with the object, when they are ASCII (the same in
// every 8-bit encoding the game asks for); NULL otherwise, which callers must
// handle (they copy with CFStringGetCString then). Brotherhood's hardware
// detection reads system_profiler's values this way.
void imp_CFStringGetCStringPtr(Context& c) {
    Object* o = bridge::get(c, arg32(c, 0));
    if (!o || o->kind != Kind::String) return shim_return(c, 0);
    for (unsigned char ch : o->str)
        if (ch >= 0x80) return shim_return(c, 0);
    shim_return(c, bridge::utf8(c, *o));
}
// CFStringEncoding CFStringGetSystemEncoding(void): Mac OS Roman, as on a Mac.
void imp_CFStringGetSystemEncoding(Context& c) { shim_return(c, kCFStringEncodingMacRoman); }
// void CFStringGetCharacters(CFStringRef, CFRange, UniChar* buffer)
void imp_CFStringGetCharacters(Context& c) {
    const std::string s = bridge::text(c, arg32(c, 0));
    const uint32_t loc = arg32(c, 1), len = arg32(c, 2), buf = arg32(c, 3);
    const std::vector<uint8_t> u16 = encode(s, kCFStringEncodingUTF16LE, '?');
    for (uint32_t k = 0; k < len && 2 * (loc + k) + 1 < u16.size(); k++)
        c.mem->write<uint16_t>(buf + 2 * k, (uint16_t)(u16[2 * (loc + k)] | u16[2 * (loc + k) + 1] << 8));
    shim_return(c, 0);
}
// CFIndex CFStringGetBytes(CFStringRef, CFRange, CFStringEncoding, UInt8 lossByte,
//                          Boolean isExternal, UInt8* buffer, CFIndex maxBufLen, CFIndex* usedBufLen)
void imp_CFStringGetBytes(Context& c) {
    const std::string s = bridge::text(c, arg32(c, 0));
    const uint32_t loc = arg32(c, 1), len = arg32(c, 2), enc = arg32(c, 3), loss = arg32(c, 4) & 0xff;
    const uint32_t buf = arg32(c, 6), max = arg32(c, 7), used = arg32(c, 8);
    const std::string part = loc < s.size() ? s.substr(loc, len) : std::string();
    std::vector<uint8_t> b = encode(part, enc, (uint8_t)loss);
    if (buf && b.size() > max) b.resize(max);
    if (buf) std::memcpy(c.mem->ptr(buf), b.data(), b.size());
    if (used) c.mem->write<uint32_t>(used, (uint32_t)b.size());
    shim_return(c, (uint32_t)part.size());
}
// CFDataRef CFStringCreateExternalRepresentation(CFAllocatorRef, CFStringRef, CFStringEncoding, UInt8 lossByte)
void imp_CFStringCreateExternalRepresentation(Context& c) {
    Object d;
    d.bytes = encode(bridge::text(c, arg32(c, 1)), arg32(c, 2), arg32(c, 3) & 0xff);
    shim_return(c, bridge::make(c, Kind::Data, "NSData", std::move(d)));
}
void imp_CFStringGetMaximumSizeOfFileSystemRepresentation(Context& c) {
    shim_return(c, 3 * (uint32_t)bridge::text(c, arg32(c, 0)).size() + 1);
}
// Boolean CFStringGetFileSystemRepresentation(CFStringRef, char* buffer, CFIndex max)
void imp_CFStringGetFileSystemRepresentation(Context& c) {
    const std::string s = bridge::text(c, arg32(c, 0));
    if (s.size() + 1 > arg32(c, 2)) return shim_return(c, 0);
    std::memcpy(c.mem->ptr(arg32(c, 1)), s.c_str(), s.size() + 1);
    shim_return(c, 1);
}

// ---- URLs (file paths only) ----
// CFURLRef CFURLCreateWithFileSystemPath(CFAllocatorRef, CFStringRef path, CFURLPathStyle, Boolean isDirectory)
void imp_CFURLCreateWithFileSystemPath(Context& c) { shim_return(c, make_url(c, bridge::text(c, arg32(c, 1)))); }
void imp_CFURLCopyAbsoluteURL(Context& c) {
    std::string p = bridge::text(c, arg32(c, 0));
    if (!p.empty() && p[0] != '/') {
        char cwd[4096];
        if (::getcwd(cwd, sizeof cwd)) p = std::string(cwd) + "/" + p;
    }
    shim_return(c, make_url(c, p));
}
void imp_CFURLCopyFileSystemPath(Context& c) { shim_return(c, bridge::make_string(c, bridge::text(c, arg32(c, 0)))); }
// CFURLRef CFURLCreateCopyAppendingPathComponent(CFAllocatorRef, CFURLRef, CFStringRef, Boolean)
void imp_CFURLCreateCopyAppendingPathComponent(Context& c) {
    std::string p = bridge::text(c, arg32(c, 1));
    if (!p.empty() && p.back() != '/') p += '/';
    shim_return(c, make_url(c, p + bridge::text(c, arg32(c, 2))));
}
// Boolean CFURLGetFileSystemRepresentation(CFURLRef, Boolean resolveAgainstBase, UInt8* buffer, CFIndex max)
void imp_CFURLGetFileSystemRepresentation(Context& c) {
    const std::string p = bridge::text(c, arg32(c, 0));
    if (p.size() + 1 > arg32(c, 3)) return shim_return(c, 0);
    std::memcpy(c.mem->ptr(arg32(c, 2)), p.c_str(), p.size() + 1);
    shim_return(c, 1);
}
// CFURLRef CFURLCreateFromFSRef(CFAllocatorRef, const FSRef*)
void imp_CFURLCreateFromFSRef(Context& c) { shim_return(c, make_url(c, fsref_path(c, arg32(c, 1)))); }

// ---- bundles ----
void imp_CFBundleGetMainBundle(Context& c) { shim_return(c, bridge::main_bundle(c)); }
// CFBundleRef CFBundleCreate(CFAllocatorRef, CFURLRef bundleURL)
void imp_CFBundleCreate(Context& c) {
    Object b;
    b.str = bridge::text(c, arg32(c, 1));
    if (::access((b.str + "/Contents").c_str(), F_OK) != 0) return shim_return(c, 0);
    shim_return(c, bridge::make(c, Kind::Bundle, "NSBundle", std::move(b)));
}
void imp_CFBundleCopyResourcesDirectoryURL(Context& c) {
    shim_return(c, make_url(c, bridge::expect(c, arg32(c, 0), Kind::Bundle, "CFBundle").str + "/Contents/Resources"));
}
void imp_CFBundleCopyPrivateFrameworksURL(Context& c) {
    shim_return(c, make_url(c, bridge::expect(c, arg32(c, 0), Kind::Bundle, "CFBundle").str + "/Contents/Frameworks"));
}
// CFTypeRef CFBundleGetValueForInfoDictionaryKey(CFBundleRef, CFStringRef key): from Info.plist.
// SCIMITAR_GAME_LANGUAGE=fre (eng, ger, ita, spa, rus...) answers the
// "Language" key, from which AC2 picks its text and voices (its text and
// voices apart: games/ac2/language.cpp).
void imp_CFBundleGetValueForInfoDictionaryKey(Context& c) {
    if (const char* lang = std::getenv("SCIMITAR_GAME_LANGUAGE"); lang && *lang &&
        bridge::text(c, arg32(c, 1)) == "Language")
        return shim_return(c, bridge::make_string(c, lang));
    const std::string path = info_plist_path(c, arg32(c, 0));
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return shim_return(c, 0);
    std::vector<uint8_t> bytes;
    for (int ch; (ch = std::fgetc(f)) != EOF;) bytes.push_back((uint8_t)ch);
    std::fclose(f);
    const uint32_t dict = read_plist_bytes(c, bytes.data(), bytes.size());
    const Object* d = bridge::get(c, dict);
    if (d && d->kind == Kind::Dictionary)
        for (const auto& [k, v] : d->pairs)
            if (bridge::equal(c, k, arg32(c, 1))) return shim_return(c, v);
    shim_return(c, 0);
}

// ---- locale, preferences, notifications ----
void imp_CFLocaleCopyCurrent(Context& c) { shim_return(c, bridge::make(c, Kind::Locale, "NSLocale")); }
// CFTypeRef CFLocaleGetValue(CFLocaleRef, CFStringRef key): language or country code
void imp_CFLocaleGetValue(Context& c) {
    const std::string key = bridge::text(c, arg32(c, 1));
    CFLocaleRef l = CFLocaleCopyCurrent();
    CFStringRef k = key.find("Country") != std::string::npos ? kCFLocaleCountryCode : kCFLocaleLanguageCode;
    CFStringRef v = (CFStringRef)CFLocaleGetValue(l, k);
    char buf[64] = "";
    if (v) CFStringGetCString(v, buf, sizeof buf, kCFStringEncodingUTF8);
    CFRelease(l);
    shim_return(c, bridge::make_string(c, buf[0] ? buf : key.find("Country") != std::string::npos ? "US" : "en"));
}
// CFPropertyListRef CFPreferencesCopyAppValue(CFStringRef key, CFStringRef applicationID)
void imp_CFPreferencesCopyAppValue(Context& c) {
    const std::string key = bridge::text(c, arg32(c, 0)), app = bridge::text(c, arg32(c, 1));
    CFStringRef k = CFStringCreateWithCString(nullptr, key.c_str(), kCFStringEncodingUTF8);
    CFStringRef a = CFStringCreateWithCString(nullptr, app.c_str(), kCFStringEncodingUTF8);
    CFPropertyListRef v = CFPreferencesCopyAppValue(k, a);
    CFRelease(k);
    CFRelease(a);
    uint32_t r = 0;
    if (v) {
        r = bridge::from_plist(c, v);
        CFRelease(v);
    }
    shim_return(c, r);
}
// SInt32 CFUserNotificationDisplayAlert(CFTimeInterval timeout, CFOptionFlags, CFURLRef icon,
//     CFURLRef sound, CFURLRef localization, CFStringRef header, CFStringRef message,
//     CFStringRef default, CFStringRef alternate, CFStringRef other, CFOptionFlags* response)
// The timeout is a double (two slots). Shown on stderr; answered with the default button.
void imp_CFUserNotificationDisplayAlert(Context& c) {
    std::fprintf(stderr, "[game alert] %s: %s\n", bridge::text(c, arg32(c, 6)).c_str(),
                 bridge::text(c, arg32(c, 7)).c_str());
    if (arg32(c, 11)) c.mem->write<uint32_t>(arg32(c, 11), 0);
    shim_return(c, 0);
}

// ---- Carbon: FSRef paths and the process bundle ----
// OSStatus FSPathMakeRef(const UInt8* path, FSRef* ref, Boolean* isDirectory)
void imp_FSPathMakeRef(Context& c) {
    const std::string path = host_path((const char*)c.mem->ptr(arg32(c, 0)));
    struct stat st;
    static const bool trace = std::getenv("SCIMITAR_TRACE_FILES") != nullptr;
    const bool found = ::stat(path.c_str(), &st) == 0;
    if (trace) std::fprintf(stderr, "FSPathMakeRef(%s) = %d\n", path.c_str(), found ? 0 : -43);
    if (!found) return shim_return(c, (uint32_t)-43);  // fnfErr
    put_fsref(c, arg32(c, 1), path);
    if (arg32(c, 2)) c.mem->write<uint8_t>(arg32(c, 2), S_ISDIR(st.st_mode) ? 1 : 0);
    shim_return(c, 0);
}
// OSErr GetCurrentProcess(ProcessSerialNumber*)
void imp_GetCurrentProcess(Context& c) {
    c.mem->write<uint32_t>(arg32(c, 0), 0);
    c.mem->write<uint32_t>(arg32(c, 0) + 4, 2);  // kCurrentProcess
    shim_return(c, 0);
}
// OSStatus GetProcessBundleLocation(const ProcessSerialNumber*, FSRef* location)
void imp_GetProcessBundleLocation(Context& c) {
    put_fsref(c, arg32(c, 1), bridge::bundle_path());
    shim_return(c, 0);
}

}  // namespace scimitar::imports

namespace scimitar {
namespace {
using namespace imports;
std::string object_text(GuestMemory& m, uint32_t obj) {
    Context c;
    c.mem = &m;
    return bridge::text(c, obj);
}
const ImportShim kCF[] = {
    {"_CFAllocatorAllocate", imp_CFAllocatorAllocate},
    {"_CFAllocatorDeallocate", imp_CFAllocatorDeallocate},
    {"_CFGetTypeID", imp_CFGetTypeID},
    {"_CFStringGetTypeID", imp_CFStringGetTypeID},
    {"_CFArrayGetTypeID", imp_CFArrayGetTypeID},
    {"_CFDictionaryGetTypeID", imp_CFDictionaryGetTypeID},
    {"_CFNumberGetTypeID", imp_CFNumberGetTypeID},
    {"_CFStringGetCStringPtr", imp_CFStringGetCStringPtr},
    {"_CFStringGetSystemEncoding", imp_CFStringGetSystemEncoding},
    {"_CFRelease", imp_CFRelease},
    {"_CFShow", imp_CFShow},
    {"_CFArrayGetCount", imp_CFArrayGetCount},
    {"_CFArrayGetValueAtIndex", imp_CFArrayGetValueAtIndex},
    {"_CFSetGetCount", imp_CFSetGetCount},
    {"_CFSetGetValues", imp_CFSetGetValues},
    {"_CFBooleanGetValue", imp_CFBooleanGetValue},
    {"_CFDictionaryCreateMutable", imp_CFDictionaryCreateMutable},
    {"_CFDictionaryGetValue", imp_CFDictionaryGetValue},
    {"_CFDictionarySetValue", imp_CFDictionarySetValue},
    {"_CFDictionaryContainsValue", imp_CFDictionaryContainsValue},
    {"_CFNumberCreate", imp_CFNumberCreate},
    {"_CFNumberGetValue", imp_CFNumberGetValue},
    {"_CFDataCreate", imp_CFDataCreate},
    {"_CFDataGetBytePtr", imp_CFDataGetBytePtr},
    {"_CFDataGetBytes", imp_CFDataGetBytes},
    {"_CFPropertyListCreateFromXMLData", imp_CFPropertyListCreateFromXMLData},
    {"_CFStringCreateWithBytes", imp_CFStringCreateWithBytes},
    {"_CFStringCreateWithFileSystemRepresentation", imp_CFStringCreateWithFileSystemRepresentation},
    {"_CFStringCreateWithFormat", imp_CFStringCreateWithFormat},
    {"_CFStringCreateWithFormatAndArguments", imp_CFStringCreateWithFormatAndArguments},
    {"_CFStringGetLength", imp_CFStringGetLength},
    {"_CFStringGetCString", imp_CFStringGetCString},
    {"_CFStringGetCharacters", imp_CFStringGetCharacters},
    {"_CFStringGetBytes", imp_CFStringGetBytes},
    {"_CFStringCreateExternalRepresentation", imp_CFStringCreateExternalRepresentation},
    {"_CFStringGetMaximumSizeOfFileSystemRepresentation", imp_CFStringGetMaximumSizeOfFileSystemRepresentation},
    {"_CFStringGetFileSystemRepresentation", imp_CFStringGetFileSystemRepresentation},
    {"_CFURLCreateWithFileSystemPath", imp_CFURLCreateWithFileSystemPath},
    {"_CFURLCopyAbsoluteURL", imp_CFURLCopyAbsoluteURL},
    {"_CFURLCopyFileSystemPath", imp_CFURLCopyFileSystemPath},
    {"_CFURLCreateCopyAppendingPathComponent", imp_CFURLCreateCopyAppendingPathComponent},
    {"_CFURLGetFileSystemRepresentation", imp_CFURLGetFileSystemRepresentation},
    {"_CFURLCreateFromFSRef", imp_CFURLCreateFromFSRef},
    {"_CFBundleGetMainBundle", imp_CFBundleGetMainBundle},
    {"_CFBundleCreate", imp_CFBundleCreate},
    {"_CFBundleCopyResourcesDirectoryURL", imp_CFBundleCopyResourcesDirectoryURL},
    {"_CFBundleCopyPrivateFrameworksURL", imp_CFBundleCopyPrivateFrameworksURL},
    {"_CFBundleGetValueForInfoDictionaryKey", imp_CFBundleGetValueForInfoDictionaryKey},
    {"_CFLocaleCopyCurrent", imp_CFLocaleCopyCurrent},
    {"_CFLocaleGetValue", imp_CFLocaleGetValue},
    {"_CFPreferencesCopyAppValue", imp_CFPreferencesCopyAppValue},
    {"_CFUserNotificationDisplayAlert", imp_CFUserNotificationDisplayAlert},
    {"_FSPathMakeRef", imp_FSPathMakeRef},
    {"_GetCurrentProcess", imp_GetCurrentProcess},
    {"_GetProcessBundleLocation", imp_GetProcessBundleLocation},
};
SCIMITAR_REGISTER_SHIMS(kCF);
struct InstallFormatter {
    InstallFormatter() { set_object_formatter(object_text); }
} kInstallFormatter;
}  // namespace
}  // namespace scimitar
