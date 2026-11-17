#include "scimitar/image_loader.h"

#include <algorithm>
#include <cstring>

#include "scimitar/runtime.h"

namespace scimitar {

namespace {
const recomp::Image* g_image = nullptr;
}

const recomp::Image* loaded_image() { return g_image; }

uint32_t guest_symbol_or_0(const char* name) { return g_image ? g_image->symbol(name) : 0; }

uint32_t guest_symbol(const char* name) {
    const uint32_t a = guest_symbol_or_0(name);
    if (!a) fatal("the game has no symbol %s", name);
    return a;
}

LoadedImage load_image(GuestMemory& mem, const std::string& path) {
    LoadedImage out;
    out.image = std::make_unique<recomp::Image>(path);
    const recomp::Image& img = *out.image;
    g_image = &img;

    for (const recomp::Segment& s : img.segments()) {
        if (s.name == "__PAGEZERO" || s.name == "__LINKEDIT" || s.vmsize == 0) continue;
        mem.commit(s.vmaddr, s.vmsize);
        const uint32_t n = std::min(s.filesize, s.vmsize);
        if (s.fileoff + n > img.data().size()) fatal("segment %s beyond end of file", s.name.c_str());
        std::memcpy(mem.ptr(s.vmaddr), img.data().data() + s.fileoff, n);
    }

    // Bind pointer slots: weak symbols the image defines point at the
    // definition; anything else gets zeroed placeholder storage.
    const uint32_t kImportDataBase = import_data_base();
    uint32_t next = kImportDataBase;
    for (const auto& [slot, imp] : img.pointer_slots()) {
        uint32_t target = imp.internal;
        if (!target) {
            auto it = out.import_data.find(imp.name);
            if (it == out.import_data.end()) {
                if (next == kImportDataBase) mem.commit(kImportDataBase, 0x100000);
                it = out.import_data.emplace(imp.name, next).first;
                next += kImportDataSlot;
                if (next >= kImportDataBase + 0x100000) fatal("too many imported data symbols");
            }
            target = it->second;
        }
        mem.write<uint32_t>(slot, target);
    }
    return out;
}

}  // namespace scimitar
