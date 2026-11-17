// Binds the executable's non-lazy pointer slots that dyld would fill with
// imported symbols (the loader gave them zeroed placeholders). Imported
// functions the code takes the address of (malloc as a callback...) get host
// functions running their shims; imported variables get real values.
#include <runetype.h>

#include <cstdio>
#include <cstring>
#include <random>

#include "bridge.h"
#include "scimitar/guest_heap.h"
#include "scimitar/image_loader.h"
#include "scimitar/imports.h"
#include "scimitar/runtime.h"

namespace scimitar {

uint32_t guest_file(GuestMemory& m, FILE* f);  // stdio.cpp

namespace {

// _DefaultRuneLocale in the i386 layout (3164 bytes): magic, encoding, two
// function pointers, invalid rune, then the runetype/maplower/mapupper tables
// for the first 256 characters; extension ranges empty.
uint32_t rune_locale(GuestMemory& m) {
    const uint32_t p = heap::alloc(m, 3164);
    std::memset(m.ptr(p), 0, 3164);
    std::memcpy(m.ptr(p), "RuneMagA", 8);
    std::memcpy(m.ptr(p + 8), "NONE", 4);
    m.write<uint32_t>(p + 48, (uint32_t)_DefaultRuneLocale.__invalid_rune);
    for (uint32_t k = 0; k < 256; k++) {
        m.write<uint32_t>(p + 52 + 4 * k, _DefaultRuneLocale.__runetype[k]);
        m.write<uint32_t>(p + 1076 + 4 * k, (uint32_t)_DefaultRuneLocale.__maplower[k]);
        m.write<uint32_t>(p + 2100 + 4 * k, (uint32_t)_DefaultRuneLocale.__mapupper[k]);
    }
    return p;
}

}  // namespace

void bind_imports(GuestMemory& m, LoadedImage& image) {
    for (const recomp::Section& s : image.image->sections())
        if (s.sectname == "__cfstring") bridge::set_constant_strings(s.addr, s.addr + s.size);
    size_t functions = 0;
    Context c;  // for creating bridge objects
    c.mem = &m;
    for (const auto& [slot, imp] : image.image->pointer_slots()) {
        if (imp.internal) continue;
        if (const ImportShim* s = find_import_shim(imp.name)) {
            m.write<uint32_t>(slot, register_host_function(s->fn, s->dyld_name));
            functions++;
            continue;
        }
        const uint32_t storage = m.read<uint32_t>(slot);  // the placeholder
        const std::string& n = imp.name;
        if (n == "___stdinp") m.write<uint32_t>(storage, guest_file(m, stdin));
        else if (n == "___stdoutp") m.write<uint32_t>(storage, guest_file(m, stdout));
        else if (n == "___stderrp") m.write<uint32_t>(storage, guest_file(m, stderr));
        else if (n == "__DefaultRuneLocale") m.write<uint32_t>(slot, rune_locale(m));
        else if (n == "___mb_cur_max") m.write<uint32_t>(storage, 1);
        else if (n == "_mach_task_self_") m.write<uint32_t>(storage, 0x103);
        else if (n == "___stack_chk_guard") m.write<uint32_t>(storage, std::random_device{}() | 1);
        // CoreFoundation/Cocoa constants are objects of the bridge; their
        // string values are the names CF uses for them.
        else if (n == "_kCFBooleanTrue") m.write<uint32_t>(storage, bridge::make_boolean(c, true));
        else if (n == "_kCFLocaleCountryCode") m.write<uint32_t>(storage, bridge::make_string(c, "kCFLocaleCountryCodeKey"));
        else if (n == "_NSLocaleLanguageCode") m.write<uint32_t>(storage, bridge::make_string(c, "kCFLocaleLanguageCodeKey"));
        else if (n == "_NSDefaultRunLoopMode") m.write<uint32_t>(storage, bridge::make_string(c, "kCFRunLoopDefaultMode"));
        else if (n == "_NSFullScreenModeSetting") m.write<uint32_t>(storage, bridge::make_string(c, "NSFullScreenModeSetting"));
        else if (n == "_NSViewGlobalFrameDidChangeNotification")
            m.write<uint32_t>(storage, bridge::make_string(c, "NSViewGlobalFrameDidChangeNotification"));
        // kCFAllocatorDefault and kIOMasterPortDefault are 0, as on macOS;
        // the callback structs for CFDictionaryCreateMutable are not read.
    }
    if (std::getenv("SCIMITAR_VERBOSE"))
        std::fprintf(stderr, "bound %zu imported functions referenced by address\n", functions);
}

}  // namespace scimitar
