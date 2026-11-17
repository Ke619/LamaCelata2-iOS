// The OpenGL, CGL and GLU imports where there is no desktop OpenGL (iOS): every
// GL call goes to the Metal backend (platform/src/metal), as opengl.cpp does on
// the Mac with SCIMITAR_RENDERER=metal. The game asks CGL for its pixel format
// and a second context, which it never uses (docs/METAL.md); here both are
// handles that stand for nothing.
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "scimitar/guest_heap.h"
#include "scimitar/imports.h"
#include "scimitar/runtime.h"
#include "metal/shims.h"

namespace scimitar::imports {

#define SCIMITAR_FORWARD(N) \
    void imp_##N(Context& c) { metal::m_##N(c); }
SCIMITAR_METAL_GL(SCIMITAR_FORWARD)
#undef SCIMITAR_FORWARD

// const GLubyte* gluErrorString(GLenum): GLU's words for the errors it knows.
void imp_gluErrorString(Context& c) {
    static std::map<uint32_t, uint32_t> strings;
    static std::mutex m;
    std::lock_guard<std::mutex> l(m);
    uint32_t& s = strings[arg32(c, 0)];
    if (!s) {
        const char* t;
        switch (arg32(c, 0)) {
        case 0: t = "no error"; break;
        case 0x0500: t = "invalid enumerant"; break;
        case 0x0501: t = "invalid value"; break;
        case 0x0502: t = "invalid operation"; break;
        case 0x0503: t = "stack overflow"; break;
        case 0x0504: t = "stack underflow"; break;
        case 0x0505: t = "out of memory"; break;
        case 0x0506: t = "invalid framebuffer operation"; break;
        default: t = "unknown error"; break;
        }
        s = heap::alloc(*c.mem, (uint32_t)std::strlen(t) + 1);
        std::memcpy(c.mem->ptr(s), t, std::strlen(t) + 1);
    }
    shim_return(c, s);
}

constexpr uint32_t kPixelFormat = 0x0c610001, kSecondContext = 0x0c610002;
// CGLPixelFormatObj CGLGetPixelFormat(CGLContextObj)
void imp_CGLGetPixelFormat(Context& c) { shim_return(c, kPixelFormat); }
// CGLError CGLCreateContext(CGLPixelFormatObj, CGLContextObj share, CGLContextObj* out)
void imp_CGLCreateContext(Context& c) {
    if (arg32(c, 2)) c.mem->write<uint32_t>(arg32(c, 2), kSecondContext);
    shim_return(c, 0);
}
void imp_CGLSetCurrentContext(Context& c) { shim_return(c, 0); }
void imp_CGLReleaseContext(Context& c) { shim_return(c, 0); }
void imp_CGLSetOption(Context& c) { shim_return(c, 0); }

}  // namespace scimitar::imports

namespace scimitar {

void gl_frame_presented(uint64_t) {}
void gl_trace_frame() {}

namespace {
using namespace imports;
#define S(N) {"_" #N, imp_##N},
const ImportShim kGL[] = {
    SCIMITAR_METAL_GL(S)
    S(gluErrorString) S(CGLGetPixelFormat) S(CGLCreateContext) S(CGLSetCurrentContext) S(CGLReleaseContext) S(CGLSetOption)
};
#undef S
SCIMITAR_REGISTER_SHIMS(kGL);
}  // namespace
}  // namespace scimitar
