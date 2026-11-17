// ADVAPI32's registry: the keys a game creates and the values it writes are
// kept in the port's folder (support_dir()/registry.txt, one value a line:
// key, name, type and data in hex) and read back on the next run. Nothing
// else is there: keys a game only reads (install path, CD key, AC1's) are
// absent, as on a machine without the installer's keys, and the game uses
// its defaults.
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <vector>

#include "win32.h"

namespace scimitar::win32 {
namespace {

struct Value {
    uint32_t type = 0;
    std::vector<uint8_t> data;
};
using Key = std::map<std::string, Value>;  // by lower-case value name

std::mutex g_lock;
std::map<std::string, Key> g_keys;              // by lower-case full path, "hkcu\software\..."
std::map<uint32_t, std::string> g_handles;      // open keys
uint32_t g_next_handle = 0x10100;
bool g_loaded = false;

std::string lower(std::string s) {
    for (char& ch : s) ch = (char)std::tolower((unsigned char)ch);
    return s;
}

std::string file() { return support_dir() + "/registry.txt"; }

void load() {
    if (g_loaded) return;
    g_loaded = true;
    std::ifstream in(file());
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream f(line);
        std::string key, name, type, hex;
        if (!std::getline(f, key, '\t') || !std::getline(f, name, '\t') || !std::getline(f, type, '\t')) continue;
        std::getline(f, hex);
        Value v;
        v.type = (uint32_t)std::strtoul(type.c_str(), nullptr, 10);
        for (size_t k = 0; k + 1 < hex.size(); k += 2) v.data.push_back((uint8_t)std::stoul(hex.substr(k, 2), nullptr, 16));
        g_keys[key][name] = v;
    }
}

void save() {
    std::ofstream out(file());
    for (const auto& [key, values] : g_keys)
        for (const auto& [name, v] : values) {
            out << key << '\t' << name << '\t' << v.type << '\t';
            char b[3];
            for (uint8_t x : v.data) std::snprintf(b, sizeof b, "%02x", x), out << b;
            out << '\n';
        }
}

// The full path of `sub` under the key handle `parent`, or "" for an unknown handle.
std::string path_of(uint32_t parent, const std::string& sub) {
    std::string base;
    switch (parent) {
    case 0x80000000u: base = "hkcr"; break;
    case 0x80000001u: base = "hkcu"; break;
    case 0x80000002u: base = "hklm"; break;
    case 0x80000003u: base = "hku"; break;
    default: {
        auto it = g_handles.find(parent);
        if (it == g_handles.end()) return "";
        base = it->second;
    }
    }
    std::string s = lower(sub);
    while (!s.empty() && s.back() == '\\') s.pop_back();
    return s.empty() ? base : base + "\\" + s;
}

uint32_t open_handle(const std::string& path) {
    const uint32_t h = g_next_handle++;
    g_handles[h] = path;
    return h;
}

}  // namespace

void registry_default_dword(const std::string& key, const std::string& name, uint32_t value) {
    std::lock_guard<std::mutex> l(g_lock);
    load();
    Key& k = g_keys[lower(key)];
    if (k.count(lower(name))) return;
    Value v;
    v.type = 4;  // REG_DWORD
    v.data.resize(4);
    std::memcpy(v.data.data(), &value, 4);
    k[lower(name)] = v;
}

}  // namespace scimitar::win32

namespace scimitar::imports {
using namespace scimitar::win32;

// RegOpenKeyExA(key, subkey, options, access, &result)
W32(advapi32, RegOpenKeyExA) {
    std::lock_guard<std::mutex> l(g_lock);
    load();
    const std::string p = path_of(arg(c, 0), arg(c, 1) ? gstr(arg(c, 1)) : "");
    const bool found = !p.empty() && g_keys.count(p);
    trace("RegOpenKeyExA(%08x, %s)%s", arg(c, 0), arg(c, 1) ? gstr(arg(c, 1)).c_str() : "", found ? "" : ": absent");
    if (!found) return ret(c, 2, 5);  // ERROR_FILE_NOT_FOUND
    wr(arg(c, 4), open_handle(p));
    ret(c, 0, 5);
}

// RegCreateKeyExA(key, subkey, 0, class, options, access, attributes, &result, &disposition)
W32(advapi32, RegCreateKeyExA) {
    std::lock_guard<std::mutex> l(g_lock);
    load();
    const std::string p = path_of(arg(c, 0), arg(c, 1) ? gstr(arg(c, 1)) : "");
    if (p.empty()) return ret(c, 6, 9);  // ERROR_INVALID_HANDLE
    const bool existed = g_keys.count(p) != 0;
    g_keys[p];
    trace("RegCreateKeyExA(%s)%s", p.c_str(), existed ? "" : ": created");
    if (arg(c, 7)) wr(arg(c, 7), open_handle(p));
    if (arg(c, 8)) wr(arg(c, 8), existed ? 2 : 1);  // REG_OPENED_EXISTING_KEY / REG_CREATED_NEW_KEY
    ret(c, 0, 9);
}

// RegQueryValueExA(key, name, reserved, &type, data, &size)
W32(advapi32, RegQueryValueExA) {
    const uint32_t type = arg(c, 3), data = arg(c, 4), size = arg(c, 5);
    const std::string name = arg(c, 1) ? gstr(arg(c, 1)) : "";
    std::lock_guard<std::mutex> l(g_lock);
    load();
    auto h = g_handles.find(arg(c, 0));
    const Value* v = nullptr;
    if (h != g_handles.end()) {
        auto k = g_keys.find(h->second);
        if (k != g_keys.end()) {
            auto it = k->second.find(lower(name));
            if (it != k->second.end()) v = &it->second;
        }
    }
    trace("RegQueryValueExA(%s)%s", name.c_str(), v ? "" : ": absent");
    if (!v) return ret(c, 2, 6);
    if (type) wr(type, v->type);
    const uint32_t n = (uint32_t)v->data.size();
    if (data) {
        if (!size || rd(size) < n) {
            if (size) wr(size, n);
            return ret(c, 234, 6);  // ERROR_MORE_DATA
        }
        std::memcpy(gptr(data), v->data.data(), n);
    }
    if (size) wr(size, n);
    ret(c, 0, 6);
}

// RegSetValueExA(key, name, reserved, type, data, size)
W32(advapi32, RegSetValueExA) {
    const std::string name = arg(c, 1) ? gstr(arg(c, 1)) : "";
    std::lock_guard<std::mutex> l(g_lock);
    load();
    auto h = g_handles.find(arg(c, 0));
    if (h == g_handles.end()) return ret(c, 6, 6);
    Value v;
    v.type = arg(c, 3);
    v.data.assign(gptr(arg(c, 4)), gptr(arg(c, 4)) + arg(c, 5));
    trace("RegSetValueExA(%s\\%s, type %u, %u bytes)", h->second.c_str(), name.c_str(), v.type, arg(c, 5));
    g_keys[h->second][lower(name)] = v;
    save();
    ret(c, 0, 6);
}

W32(advapi32, RegCloseKey) {
    std::lock_guard<std::mutex> l(g_lock);
    g_handles.erase(arg(c, 0));
    ret(c, 0, 1);
}

}  // namespace scimitar::imports
