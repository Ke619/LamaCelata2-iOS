// The Foundation/CoreFoundation object model (bridge.h).
#include "bridge.h"

#include <CoreFoundation/CoreFoundation.h>

#include <cstring>
#include <map>
#include <mutex>

#include "scimitar/guest_heap.h"
#include "scimitar/imports.h"

namespace scimitar::bridge {

namespace {

std::recursive_mutex g_lock;
std::map<uint32_t, Object> g_objects;
uint32_t g_cf_begin = 0, g_cf_end = 0;

}  // namespace

void set_constant_strings(uint32_t begin, uint32_t end) {
    g_cf_begin = begin;
    g_cf_end = end;
}

uint32_t make(Context& c, Kind kind, const std::string& cls, Object o) {
    const uint32_t a = heap::alloc(*c.mem, 16);
    std::memset(c.mem->ptr(a), 0, 16);
    o.kind = kind;
    o.cls = cls;
    std::lock_guard<std::recursive_mutex> l(g_lock);
    g_objects[a] = std::move(o);
    return a;
}

uint32_t make_string(Context& c, const std::string& utf8) {
    Object o;
    o.str = utf8;
    return make(c, Kind::String, "NSString", std::move(o));
}

uint32_t make_array(Context& c, std::vector<uint32_t> items, bool mutable_) {
    Object o;
    o.items = std::move(items);
    o.mutable_ = mutable_;
    return make(c, Kind::Array, mutable_ ? "NSMutableArray" : "NSArray", std::move(o));
}

uint32_t make_number(Context& c, double v, bool integer) {
    Object o;
    o.number = v;
    o.integer = integer;
    return make(c, Kind::Number, "NSNumber", std::move(o));
}

uint32_t make_boolean(Context& c, bool v) {
    static uint32_t yes = 0, no = 0;
    uint32_t& b = v ? yes : no;
    if (!b) {
        Object o;
        o.number = v;
        b = make(c, Kind::Boolean, "NSNumber", std::move(o));
    }
    return b;
}

Object* get(Context& c, uint32_t a) {
    std::lock_guard<std::recursive_mutex> l(g_lock);
    auto it = g_objects.find(a);
    if (it != g_objects.end()) return &it->second;
    // A constant string: {isa, flags, const char* str, long length}.
    if (a >= g_cf_begin && a + 16 <= g_cf_end && (a - g_cf_begin) % 16 == 0) {
        Object o;
        o.kind = Kind::String;
        o.cls = "NSString";
        const uint32_t s = c.mem->read<uint32_t>(a + 8), n = c.mem->read<uint32_t>(a + 12);
        o.str.assign((const char*)c.mem->ptr(s), n);
        o.cstr = s;  // already NUL-terminated in __cstring
        return &(g_objects[a] = std::move(o));
    }
    return nullptr;
}

Object& expect(Context& c, uint32_t a, Kind kind, const char* what) {
    Object* o = get(c, a);
    if (!o || o->kind != kind) fatal("%s: %08x is not the expected object", what, a);
    return *o;
}

// Objects are never freed: the game mixes CoreFoundation and Objective-C
// reference counting (CFRelease, release, autorelease pools) in ways this
// bridge does not track, and it makes few objects, so keeping them is safer
// than freeing one still in use.
void release(Context&, uint32_t) {}

uint32_t utf8(Context& c, Object& s) {
    if (!s.cstr) {
        s.cstr = heap::alloc(*c.mem, (uint32_t)s.str.size() + 1);
        std::memcpy(c.mem->ptr(s.cstr), s.str.c_str(), s.str.size() + 1);
    }
    return s.cstr;
}

std::string text(Context& c, uint32_t a) {
    const Object* o = get(c, a);
    return o && (o->kind == Kind::String || o->kind == Kind::URL) ? o->str : std::string();
}

bool equal(Context& c, uint32_t a, uint32_t b) {
    if (a == b) return true;
    const Object* x = get(c, a);
    const Object* y = get(c, b);
    if (!x || !y || x->kind != y->kind) return false;
    if (x->kind == Kind::String) return x->str == y->str;
    if (x->kind == Kind::Number || x->kind == Kind::Boolean) return x->number == y->number;
    return false;
}

uint32_t from_plist(Context& c, const void* p) {
    CFTypeRef v = (CFTypeRef)p;
    const CFTypeID t = CFGetTypeID(v);
    if (t == CFStringGetTypeID()) {
        CFStringRef s = (CFStringRef)v;
        const CFIndex n = CFStringGetMaximumSizeForEncoding(CFStringGetLength(s), kCFStringEncodingUTF8) + 1;
        std::string buf((size_t)n, '\0');
        CFStringGetCString(s, buf.data(), n, kCFStringEncodingUTF8);
        buf.resize(std::strlen(buf.c_str()));
        return make_string(c, buf);
    }
    if (t == CFBooleanGetTypeID()) return make_boolean(c, CFBooleanGetValue((CFBooleanRef)v));
    if (t == CFNumberGetTypeID()) {
        CFNumberRef n = (CFNumberRef)v;
        double d = 0;
        CFNumberGetValue(n, kCFNumberDoubleType, &d);
        return make_number(c, d, !CFNumberIsFloatType(n));
    }
    if (t == CFDataGetTypeID()) {
        Object o;
        CFDataRef d = (CFDataRef)v;
        o.bytes.assign(CFDataGetBytePtr(d), CFDataGetBytePtr(d) + CFDataGetLength(d));
        return make(c, Kind::Data, "NSData", std::move(o));
    }
    if (t == CFDateGetTypeID()) {
        Object o;
        o.number = CFDateGetAbsoluteTime((CFDateRef)v);
        return make(c, Kind::Date, "NSDate", std::move(o));
    }
    if (t == CFArrayGetTypeID()) {
        CFArrayRef a = (CFArrayRef)v;
        std::vector<uint32_t> items;
        for (CFIndex k = 0; k < CFArrayGetCount(a); k++) items.push_back(from_plist(c, CFArrayGetValueAtIndex(a, k)));
        return make_array(c, std::move(items));
    }
    if (t == CFDictionaryGetTypeID()) {
        CFDictionaryRef d = (CFDictionaryRef)v;
        const CFIndex n = CFDictionaryGetCount(d);
        std::vector<const void*> keys((size_t)n), values((size_t)n);
        CFDictionaryGetKeysAndValues(d, keys.data(), values.data());
        Object o;
        for (CFIndex k = 0; k < n; k++) o.pairs.emplace_back(from_plist(c, keys[k]), from_plist(c, values[k]));
        return make(c, Kind::Dictionary, "NSDictionary", std::move(o));
    }
    return 0;
}

uint32_t main_bundle(Context& c) {
    static uint32_t b = 0;
    std::lock_guard<std::recursive_mutex> l(g_lock);
    if (!b) {
        Object o;
        o.str = bundle_path();
        b = make(c, Kind::Bundle, "NSBundle", std::move(o));
    }
    return b;
}

std::string bundle_path() {
    const auto& args = process_arguments();
    std::string exe = args.empty() ? "" : args[0];
    // .../Name.app/Contents/MacOS/binary -> .../Name.app
    for (int k = 0; k < 3; k++) {
        const size_t s = exe.rfind('/');
        if (s == std::string::npos) return exe;
        exe.resize(s);
    }
    return exe;
}

}  // namespace scimitar::bridge
