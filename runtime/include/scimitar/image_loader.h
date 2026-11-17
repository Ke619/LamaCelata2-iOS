#pragma once
// Loads the game executable's segments into guest memory at their original
// addresses (the executable is not position-independent).
#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include "macho.h"
#include "scimitar/guest_layout.h"
#include "scimitar/guest_memory.h"

namespace scimitar {

// Imported data symbols (non-lazy pointer slots bound by dyld) get placeholder
// storage here until the platform layer provides real objects.
// 0x7f000000, or with the window layout 0x07f00000, just below the heap.
inline uint32_t import_data_base() {
    return guest_layout() == GuestLayout::Window ? 0x07f00000u : 0x7f000000u;
}
constexpr uint32_t kImportDataSlot = 64;

struct LoadedImage {
    std::unique_ptr<recomp::Image> image;
    std::map<std::string, uint32_t> import_data;  // import name -> placeholder address
};

LoadedImage load_image(GuestMemory& mem, const std::string& path);

// The image load_image loaded last (the game's), for lookups by name: game code
// the port calls or replaces is found by its symbol, not a fixed address, so
// the same shell serves every game built on the engine.
const recomp::Image* loaded_image();
// Address of a symbol of the loaded image; fatal if it has none.
uint32_t guest_symbol(const char* name);
// The same, or 0 when the image has no such symbol.
uint32_t guest_symbol_or_0(const char* name);

}  // namespace scimitar
