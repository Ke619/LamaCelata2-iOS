// Objective-C messages from the game (objc_msgSend and friends), answered
// natively on the bridge's objects (bridge.h).
//
// The game uses the Objective-C 1 runtime layout: its selector references
// (__OBJC,__message_refs) point at selector name strings and its class
// references (__OBJC,__cls_refs) at class name strings. A message is
// dispatched on (receiver, selector name): the receiver is a bridge object, a
// constant string, or a class name. Every implemented method is below; any
// other message stops with the class and selector names, which is how the
// bridge grows. The game sends 123 distinct selectors in all; the window,
// OpenGL view and event ones are not here yet.
#include <CoreFoundation/CoreFoundation.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "bridge.h"
#include "scimitar/guest_heap.h"
#include "scimitar/imports.h"
#include "scimitar/runtime.h"

namespace scimitar {

namespace {
std::vector<std::string> g_arguments = {"scimitar_sf"};
}

void set_process_arguments(const std::vector<std::string>& args) { g_arguments = args; }
const std::vector<std::string>& process_arguments() { return g_arguments; }

namespace imports {

namespace {

using bridge::Kind;
using bridge::Object;

std::string cstring(Context& c, uint32_t a, size_t max = 4096) {
    std::string s;
    for (size_t k = 0; a && k < max; k++) {
        const char ch = (char)c.mem->read<uint8_t>(a + (uint32_t)k);
        if (!ch) break;
        s += ch;
    }
    return s;
}

uint32_t singleton(Context& c, Kind kind, const char* cls) {
    static std::mutex m;
    static std::map<Kind, uint32_t> made;
    std::lock_guard<std::mutex> l(m);
    uint32_t& a = made[kind];
    if (!a) a = bridge::make(c, kind, cls);
    return a;
}

uint32_t dict_get(Context& c, uint32_t dict, uint32_t key) {
    Object& d = bridge::expect(c, dict, Kind::Dictionary, "NSDictionary");
    for (const auto& [k, v] : d.pairs)
        if (bridge::equal(c, k, key)) return v;
    return 0;
}

void dict_set(Context& c, uint32_t dict, uint32_t key, uint32_t value) {
    Object& d = bridge::expect(c, dict, Kind::Dictionary, "NSDictionary");
    for (auto& [k, v] : d.pairs)
        if (bridge::equal(c, k, key)) {
            v = value;
            return;
        }
    d.pairs.emplace_back(key, value);
}

uint32_t plist_file(Context& c, const std::string& path) {
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8*)path.c_str(),
                                                           (CFIndex)path.size(), false);
    CFReadStreamRef st = CFReadStreamCreateWithFile(nullptr, url);
    uint32_t r = 0;
    if (st && CFReadStreamOpen(st)) {
        CFPropertyListRef pl =
            CFPropertyListCreateWithStream(nullptr, st, 0, kCFPropertyListImmutable, nullptr, nullptr);
        if (pl) {
            r = bridge::from_plist(c, pl);
            CFRelease(pl);
        }
        CFReadStreamClose(st);
    }
    if (st) CFRelease(st);
    CFRelease(url);
    return r;
}

// Results: id/int in eax, double in ST0 (objc_msgSend_fpret).
struct Reply {
    bool fp = false;
    uint32_t value = 0;
    double d = 0;
};
Reply id(uint32_t v) { return {false, v, 0}; }
Reply fp(double v) { return {true, 0, v}; }

// Arguments after self and _cmd.
uint32_t A(Context& c, int k) { return arg32(c, 2 + k); }
double AD(Context& c, int k) { return arg_f64(c, 2 + k); }

bool class_method(Context& c, const std::string& cls, const std::string& sel, Reply* out) {
    auto is = [&](const char* c_, const char* s) { return cls == c_ && sel == s; };
    if (cls == "NSAutoreleasePool" && (sel == "alloc" || sel == "new"))
        return *out = id(bridge::make(c, Kind::AutoreleasePool, cls)), true;
    if (cls == "NSRecursiveLock" && (sel == "alloc" || sel == "new")) {
        Object l;
        l.host = std::make_shared<std::recursive_mutex>();
        return *out = id(bridge::make(c, Kind::Lock, cls, std::move(l))), true;
    }
    if (cls == "NSTask" && sel == "alloc") return *out = id(bridge::make(c, Kind::Task, cls)), true;
    if (is("NSProcessInfo", "processInfo")) return *out = id(singleton(c, Kind::ProcessInfo, "NSProcessInfo")), true;
    if (is("NSFileManager", "defaultManager"))
        return *out = id(singleton(c, Kind::FileManager, "NSFileManager")), true;
    if (is("NSNotificationCenter", "defaultCenter"))
        return *out = id(singleton(c, Kind::NotificationCenter, "NSNotificationCenter")), true;
    if (is("NSBundle", "mainBundle")) return *out = id(bridge::main_bundle(c)), true;
    if (is("NSBundle", "bundleWithPath:")) {
        Object b;
        b.str = bridge::text(c, A(c, 0));
        return *out = id(bridge::make(c, Kind::Bundle, "NSBundle", std::move(b))), true;
    }
    if (is("NSString", "stringWithCString:") || is("NSString", "stringWithUTF8String:"))
        return *out = id(bridge::make_string(c, cstring(c, A(c, 0)))), true;
    if (is("NSArray", "arrayWithObject:") || is("NSMutableArray", "arrayWithObject:"))
        return *out = id(bridge::make_array(c, {A(c, 0)}, cls == "NSMutableArray")), true;
    // + stringWithFormat:(NSString*)format, ...: printf rules plus %@, the
    // arguments after the format.
    if (is("NSString", "stringWithFormat:")) {
        const std::string f = bridge::text(c, A(c, 0));
        const uint32_t g = heap::alloc(*c.mem, (uint32_t)f.size() + 1);
        std::memcpy(c.mem->ptr(g), f.c_str(), f.size() + 1);
        const std::string text = guest_format(*c.mem, g, c.esp + 4 + 4 * 3, false);
        heap::release(*c.mem, g);
        return *out = id(bridge::make_string(c, text)), true;
    }
    if (is("NSNumber", "numberWithBool:")) return *out = id(bridge::make_boolean(c, A(c, 0) & 0xff)), true;
    if (is("NSArray", "array") || is("NSMutableArray", "array")) return *out = id(bridge::make_array(c, {}, true)), true;
    if (is("NSDictionary", "dictionaryWithContentsOfFile:") || is("NSMutableDictionary", "dictionaryWithContentsOfFile:"))
        return *out = id(plist_file(c, bridge::text(c, A(c, 0)))), true;
    if (is("NSDictionary", "dictionaryWithObject:forKey:")) {
        Object d;
        d.pairs.emplace_back(A(c, 1), A(c, 0));
        return *out = id(bridge::make(c, Kind::Dictionary, "NSDictionary", std::move(d))), true;
    }
    // + dictionaryWithObjectsAndKeys:(id)firstObject, firstKey, ..., nil
    if (is("NSDictionary", "dictionaryWithObjectsAndKeys:")) {
        Object d;
        for (int k = 0;; k += 2) {
            const uint32_t v = A(c, k);
            if (!v) break;
            d.pairs.emplace_back(A(c, k + 1), v);
        }
        return *out = id(bridge::make(c, Kind::Dictionary, "NSDictionary", std::move(d))), true;
    }
    // SCIMITAR_LANGUAGE=fr, or the ISO code of SCIMITAR_TEXT_LANGUAGE (or
    // SCIMITAR_GAME_LANGUAGE): Brotherhood's first text language
    // (SelectInstalledLanguage, through MacGetSystemLanguage).
    if (is("NSLocale", "preferredLanguages")) {
        const char* lang = std::getenv("SCIMITAR_LANGUAGE");
        if (!lang) {
            const char* game = std::getenv("SCIMITAR_TEXT_LANGUAGE");
            if (!game || !*game) game = std::getenv("SCIMITAR_GAME_LANGUAGE");
            static const char* const kIso[][2] = {
                {"eng", "en"}, {"fre", "fr"}, {"spa", "es"}, {"pol", "pl"}, {"ger", "de"}, {"chi", "zh"},
                {"hun", "hu"}, {"ita", "it"}, {"jap", "ja"}, {"cze", "cs"}, {"kor", "ko"}, {"rus", "ru"},
                {"dut", "nl"}, {"dan", "da"}, {"nor", "nb"}, {"swe", "sv"}};
            for (const auto& [code, iso] : kIso)
                if (game && std::strcmp(game, code) == 0) lang = iso;
        }
        return *out = id(bridge::make_array(c, {bridge::make_string(c, lang ? lang : "en")})), true;
    }
    // + componentsFromLocaleIdentifier: -> {NSLocaleLanguageCode: "en", NSLocaleCountryCode: ...}
    if (is("NSLocale", "componentsFromLocaleIdentifier:")) {
        const std::string idf = bridge::text(c, A(c, 0));
        Object d;
        const size_t u = idf.find_first_of("_-");
        d.pairs.emplace_back(bridge::make_string(c, "kCFLocaleLanguageCodeKey"),
                             bridge::make_string(c, idf.substr(0, u)));
        if (u != std::string::npos)
            d.pairs.emplace_back(bridge::make_string(c, "kCFLocaleCountryCodeKey"),
                                 bridge::make_string(c, idf.substr(u + 1)));
        return *out = id(bridge::make(c, Kind::Dictionary, "NSDictionary", std::move(d))), true;
    }
    if (is("NSDate", "date") || is("NSDate", "dateWithTimeIntervalSinceNow:")) {
        Object d;
        d.number = CFAbsoluteTimeGetCurrent() + (sel == "date" ? 0 : AD(c, 0));
        return *out = id(bridge::make(c, Kind::Date, "NSDate", std::move(d))), true;
    }
    if (is("NSThread", "sleepForTimeInterval:")) {
        std::this_thread::sleep_for(std::chrono::duration<double>(AD(c, 0)));
        return *out = id(0), true;
    }
    return false;
}

bool instance_method(Context& c, uint32_t self, Object& o, const std::string& sel, Reply* out) {
    if (sel == "retain" || sel == "autorelease") return *out = id(self), true;
    if (sel == "release") return bridge::release(c, self), *out = id(0), true;
    if (sel == "dealloc") return *out = id(0), true;
    switch (o.kind) {
    case Kind::AutoreleasePool:
        if (sel == "init") return *out = id(self), true;
        if (sel == "drain") return bridge::release(c, self), *out = id(0), true;
        return false;
    case Kind::Lock: {
        auto m = std::static_pointer_cast<std::recursive_mutex>(o.host);
        if (sel == "init") return *out = id(self), true;
        if (sel == "lock") return m->lock(), *out = id(0), true;
        if (sel == "unlock") return m->unlock(), *out = id(0), true;
        return false;
    }
    case Kind::ProcessInfo:
        if (sel == "arguments") {
            std::vector<uint32_t> items;
            for (const std::string& a : g_arguments) items.push_back(bridge::make_string(c, a));
            return *out = id(bridge::make_array(c, std::move(items))), true;
        }
        return false;
    case Kind::FileManager:
        if (sel == "changeCurrentDirectoryPath:")
            return *out = id(::chdir(bridge::text(c, A(c, 0)).c_str()) == 0), true;
        if (sel == "fileExistsAtPath:")
            return *out = id(::access(bridge::text(c, A(c, 0)).c_str(), F_OK) == 0), true;
        return false;
    case Kind::NotificationCenter:  // the game's observers are for window events
        if (sel == "addObserver:selector:name:object:" || sel == "removeObserver:name:object:")
            return *out = id(0), true;
        return false;
    case Kind::Task:  // helper processes (the launcher) are not started
        if (sel == "init") return *out = id(self), true;
        if (sel == "setLaunchPath:" || sel == "setArguments:" || sel == "setCurrentDirectoryPath:" ||
            sel == "launch")
            return *out = id(0), true;
        return false;
    case Kind::Bundle:
        if (sel == "bundlePath") return *out = id(bridge::make_string(c, o.str)), true;
        if (sel == "resourcePath") return *out = id(bridge::make_string(c, o.str + "/Contents/Resources")), true;
        if (sel == "executablePath")
            return *out = id(bridge::make_string(c, g_arguments.empty() ? o.str : g_arguments[0])), true;
        return false;
    case Kind::String:
        if (sel == "UTF8String" || sel == "cStringUsingEncoding:" || sel == "cString" || sel == "lossyCString")
            return *out = id(bridge::utf8(c, o)), true;
        if (sel == "length") return *out = id((uint32_t)o.str.size()), true;
        if (sel == "intValue") return *out = id((uint32_t)std::atoi(o.str.c_str())), true;
        if (sel == "isEqualToString:") return *out = id(o.str == bridge::text(c, A(c, 0))), true;
        if (sel == "hasPrefix:") {
            const std::string p = bridge::text(c, A(c, 0));
            return *out = id(o.str.compare(0, p.size(), p) == 0), true;
        }
        if (sel == "stringByDeletingLastPathComponent") {
            std::string s = o.str;
            while (s.size() > 1 && s.back() == '/') s.pop_back();
            const size_t p = s.rfind('/');
            s = p == std::string::npos ? "" : p == 0 ? "/" : s.substr(0, p);
            return *out = id(bridge::make_string(c, s)), true;
        }
        // - (BOOL)getCString:(char*)buffer maxLength:(NSUInteger)max encoding:(NSStringEncoding)
        if (sel == "getCString:maxLength:encoding:") {
            if (o.str.size() + 1 > A(c, 1)) return *out = id(0), true;
            std::memcpy(c.mem->ptr(A(c, 0)), o.str.c_str(), o.str.size() + 1);
            return *out = id(1), true;
        }
        // - (void)getCharacters:(unichar*)buffer range:(NSRange){location, length}
        if (sel == "getCharacters:range:") {
            const uint32_t buf = A(c, 0), loc = A(c, 1), len = A(c, 2);
            for (uint32_t i = 0; i < len && loc + i < o.str.size(); i++)
                c.mem->write<uint16_t>(buf + 2 * i, (uint8_t)o.str[loc + i]);
            return *out = id(0), true;
        }
        if (sel == "mutableCopy") return *out = id(bridge::make_string(c, o.str)), true;
        return false;
    case Kind::Array:
        if (sel == "count") return *out = id((uint32_t)o.items.size()), true;
        if (sel == "objectAtIndex:") {
            if (A(c, 0) >= o.items.size())
                fatal("-[NSArray objectAtIndex:%u] out of range (count %zu)", A(c, 0), o.items.size());
            return *out = id(o.items[A(c, 0)]), true;
        }
        if (sel == "addObject:") return o.items.push_back(A(c, 0)), *out = id(0), true;
        if (sel == "removeAllObjects") return o.items.clear(), *out = id(0), true;
        if (sel == "removeObjectsInRange:") {
            const uint32_t loc = A(c, 0), len = A(c, 1);
            if (loc < o.items.size())
                o.items.erase(o.items.begin() + loc,
                              o.items.begin() + std::min<size_t>(o.items.size(), (size_t)loc + len));
            return *out = id(0), true;
        }
        if (sel == "objectEnumerator") {
            Object e;
            e.items = o.items;
            return *out = id(bridge::make(c, Kind::Enumerator, "NSEnumerator", std::move(e))), true;
        }
        if (sel == "mutableCopy") return *out = id(bridge::make_array(c, o.items, true)), true;
        return false;
    case Kind::Enumerator:
        if (sel == "nextObject") return *out = id(o.cursor < o.items.size() ? o.items[o.cursor++] : 0), true;
        return false;
    case Kind::Dictionary:
        if (sel == "objectForKey:" || sel == "valueForKey:") return *out = id(dict_get(c, self, A(c, 0))), true;
        if (sel == "setObject:forKey:") return dict_set(c, self, A(c, 1), A(c, 0)), *out = id(0), true;
        if (sel == "count") return *out = id((uint32_t)o.pairs.size()), true;
        if (sel == "objectEnumerator") {
            Object e;
            for (const auto& p : o.pairs) e.items.push_back(p.second);
            return *out = id(bridge::make(c, Kind::Enumerator, "NSEnumerator", std::move(e))), true;
        }
        if (sel == "mutableCopy") {
            Object d;
            d.pairs = o.pairs;
            d.mutable_ = true;
            return *out = id(bridge::make(c, Kind::Dictionary, "NSMutableDictionary", std::move(d))), true;
        }
        return false;
    case Kind::Number:
    case Kind::Boolean:
        if (sel == "intValue" || sel == "boolValue") return *out = id((uint32_t)(int32_t)o.number), true;
        if (sel == "doubleValue") return *out = fp(o.number), true;
        return false;
    case Kind::Date:
        if (sel == "timeIntervalSinceDate:")
            return *out = fp(o.number - bridge::expect(c, A(c, 0), Kind::Date, "NSDate").number), true;
        return false;
    default:
        return false;
    }
}

void send(Context& c, uint32_t self, uint32_t sel_ref, bool fp_result) {
    if (!self) {  // messages to nil return nil (and 0.0)
        if (fp_result) return shim_return_f64(c, 0);
        return shim_return(c, 0);
    }
    const std::string sel = cstring(c, sel_ref, 256);
    Reply r;
    bool done;
    std::string who;
    if (Object* o = bridge::get(c, self)) {
        done = instance_method(c, self, *o, sel, &r);
        who = "-[" + o->cls;
    } else {
        // Not a bridge object: a class reference (its name string).
        const std::string cls = cstring(c, self, 64);
        done = class_method(c, cls, sel, &r);
        who = "+[" + cls;
    }
    if (!done)
        fatal("Objective-C message not implemented: %s %s] (from %08x)", who.c_str(), sel.c_str(),
              c.mem->read<uint32_t>(c.esp));
    static const bool trace = std::getenv("SCIMITAR_TRACE_OBJC") != nullptr;
    if (trace) {
        const std::string t = r.fp ? std::to_string(r.d) : bridge::text(c, r.value);
        std::fprintf(stderr, "objc: %s %s] -> %08x %s (from %08x)\n", who.c_str(), sel.c_str(), r.value,
                     t.c_str(), c.mem->read<uint32_t>(c.esp));
    }
    if (fp_result || r.fp) return shim_return_f64(c, r.fp ? r.d : (double)(int32_t)r.value);
    shim_return(c, r.value);
}

}  // namespace

// id objc_msgSend(id self, SEL op, ...)
void imp_objc_msgSend(Context& c) { send(c, arg32(c, 0), arg32(c, 1), false); }
// double objc_msgSend_fpret(id self, SEL op, ...): result in ST0
void imp_objc_msgSend_fpret(Context& c) { send(c, arg32(c, 0), arg32(c, 1), true); }

// NSArray* NSSearchPathForDirectoriesInDomains(NSSearchPathDirectory, NSSearchPathDomainMask,
//                                             BOOL expandTilde)
// The user domain's standard folders (the only ones the game asks for).
void imp_NSSearchPathForDirectoriesInDomains(Context& c) {
    const uint32_t dir = arg32(c, 0), domains = arg32(c, 1);
    const bool expand = arg32(c, 2) & 0xff;
    const char* home = std::getenv("HOME");
    const std::string user = expand && home ? home : "~";
    std::string path;
    switch (dir) {
    case 5: path = user + "/Library"; break;                       // NSLibraryDirectory
    case 9: path = user + "/Documents"; break;                     // NSDocumentDirectory
    case 13: path = user + "/Library/Caches"; break;               // NSCachesDirectory
    case 14: path = user + "/Library/Application Support"; break;  // NSApplicationSupportDirectory
    default: break;
    }
    std::vector<uint32_t> items;
    if (!path.empty() && (domains & 1)) items.push_back(bridge::make_string(c, path));
    shim_return(c, bridge::make_array(c, std::move(items)));
}

// void NSLog(NSString* format, ...): %@ arguments print as their text.
void imp_NSLog(Context& c) {
    std::string fmt = bridge::text(c, arg32(c, 0));
    for (size_t p = 0; (p = fmt.find("%@", p)) != std::string::npos;) fmt.replace(p, 2, "%s");
    const uint32_t tmp = heap::alloc(*c.mem, (uint32_t)fmt.size() + 1);
    std::memcpy(c.mem->ptr(tmp), fmt.c_str(), fmt.size() + 1);
    std::fprintf(stderr, "[game NSLog] %s\n", guest_format(*c.mem, tmp, c.esp + 8, false).c_str());
    heap::release(*c.mem, tmp);
    shim_return(c, 0);
}

}  // namespace imports

namespace {
const ImportShim kObjc[] = {
    {"_objc_msgSend", imports::imp_objc_msgSend},
    {"_objc_msgSend_fpret", imports::imp_objc_msgSend_fpret},
    {"_NSSearchPathForDirectoriesInDomains", imports::imp_NSSearchPathForDirectoriesInDomains},
    {"_NSLog", imports::imp_NSLog},
};
SCIMITAR_REGISTER_SHIMS(kObjc);
}  // namespace

}  // namespace scimitar
