#pragma once
// Objects the game sees through Foundation (objc_msgSend) and CoreFoundation
// (CF* functions): one model for both, as they are toll-free bridged. Each
// object is a small guest block (its identity) with a host-side description.
// The game's constant strings (CFSTR/@"..." in __DATA,__cfstring) are strings
// too, recognised by address.
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "scimitar/runtime.h"

namespace scimitar::bridge {

enum class Kind {
    String, Array, Dictionary, Number, Boolean, Data, URL, Bundle, Locale, Date,
    Enumerator, ProcessInfo, FileManager, AutoreleasePool, Lock, NotificationCenter,
    Task, HIDDevice, Other
};

struct Object {
    Kind kind = Kind::Other;
    std::string cls;                                   // class name for messages
    std::string str;                                   // String, URL path, Bundle path
    std::vector<uint32_t> items;                       // Array; Enumerator snapshot
    std::vector<std::pair<uint32_t, uint32_t>> pairs;  // Dictionary (key, value)
    double number = 0;                                 // Number, Date (seconds since 2001)
    bool integer = false;                              // Number holds an integer
    bool mutable_ = false;
    std::vector<uint8_t> bytes;                        // Data
    size_t cursor = 0;                                 // Enumerator position
    uint32_t cstr = 0;                                 // String: guest UTF-8 copy
    std::shared_ptr<void> host;                        // Lock: host mutex
};

// Creates an object; returns its guest address.
uint32_t make(Context& c, Kind kind, const std::string& cls, Object o = {});
uint32_t make_string(Context& c, const std::string& utf8);
uint32_t make_array(Context& c, std::vector<uint32_t> items, bool mutable_ = false);
uint32_t make_number(Context& c, double v, bool integer);
uint32_t make_boolean(Context& c, bool v);

// The object at a guest address (bridge objects and constant strings), or null.
Object* get(Context& c, uint32_t a);
Object& expect(Context& c, uint32_t a, Kind kind, const char* what);
void release(Context& c, uint32_t a);

// A string's guest UTF-8 copy (kept as long as the object).
uint32_t utf8(Context& c, Object& s);
// The UTF-8 contents of a string object (empty for anything else).
std::string text(Context& c, uint32_t a);
bool equal(Context& c, uint32_t a, uint32_t b);  // strings and numbers by value

// Converts a host CoreFoundation property list (dictionaries, arrays, strings,
// numbers, booleans, data, dates) to bridge objects.
uint32_t from_plist(Context& c, const void* cf_property_list);

// Where the image's constant strings are (set when imports are bound).
void set_constant_strings(uint32_t begin, uint32_t end);

// The bundle the game runs from (its .app), from the process arguments, and
// its object (NSBundle mainBundle, CFBundleGetMainBundle).
std::string bundle_path();
uint32_t main_bundle(Context& c);

}  // namespace scimitar::bridge
