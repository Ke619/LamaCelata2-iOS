#include "native_constants.h"

namespace scimitar::imports {
void imp_glProgramEnvParameters4fvEXT(Context& c);
}

namespace scimitar::native {

// glProgramEnvParameters4fvEXT(target, index, n, data) through the import
// shim, as the game's cdecl call makes it.
void imp_glProgramEnvParameters4fvEXT_call(Context& c, uint32_t target, uint32_t index, uint32_t n, uint32_t data) {
    const uint32_t esp = c.esp;
    c.esp -= 20;
    c.mem->write<uint32_t>(c.esp, 0);  // return address (unused by the shim)
    c.mem->write<uint32_t>(c.esp + 4, target);
    c.mem->write<uint32_t>(c.esp + 8, index);
    c.mem->write<uint32_t>(c.esp + 12, n);
    c.mem->write<uint32_t>(c.esp + 16, data);
    imports::imp_glProgramEnvParameters4fvEXT(c);
    c.esp = esp;
}

}  // namespace scimitar::native
