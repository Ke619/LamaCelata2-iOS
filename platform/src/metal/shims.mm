// The game's GL imports on the Metal backend (SCIMITAR_RENDERER=metal): each
// GL shim in platform/src/opengl.cpp hands its call to the m_ function of the
// same name here (metal/shims.h). Guest pointers become host pointers;
// pointers that are offsets into a bound buffer object (pixel transfers with a
// pixel buffer bound) become pointers into its storage.
#include "backend.h"
#include "shims.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "scimitar/arb_store.h"
#include "scimitar/imports.h"
#include "scimitar/metal_gl.h"
#include "scimitar/runtime.h"

namespace scimitar::metal {

namespace {

Backend& B() { return backend(); }
float F(Context& c, int i) { return arg_f32(c, i); }
bool Z(Context& c, int i) { return (arg32(c, i) & 0xff) != 0; }

void warn_once(const char* what) {
    static std::map<std::string, int> seen;
    if (seen[what]++ == 0) std::fprintf(stderr, "metal: %s is not implemented (ignored)\n", what);
}

// A pixel source: an offset into the unpack buffer, or a guest pointer.
const void* unpack(Context& c, uint32_t a) {
    Backend& b = B();
    if (b.st.unpack_buffer) {
        Buffer& buf = b.buffers[b.st.unpack_buffer];
        return buf.block ? b.mem->ptr(buf.block.guest + a) : nullptr;
    }
    return a ? c.mem->ptr(a) : nullptr;
}

// SCIMITAR_TRACE_NAMES=1: every name generated and deleted.
const bool g_trace_names = std::getenv("SCIMITAR_TRACE_NAMES") != nullptr;
const char* const kKindNames[] = {"buffer", "texture", "framebuffer", "renderbuffer", "program", "query"};

void gen(Context& c, Backend::NameKind kind) {
    const int n = (int)arg32(c, 0);
    const uint32_t out = arg32(c, 1);
    for (int k = 0; k < n && out; k++) {
        const GLuint name = B().names[kind].get();
        if (g_trace_names) std::fprintf(stderr, "metal: gen %s %u\n", kKindNames[kind], name);
        c.mem->write<uint32_t>(out + 4 * k, name);
    }
    shim_return(c, 0);
}

std::vector<GLuint> ids(Context& c) {
    std::vector<GLuint> v(arg32(c, 1) ? arg32(c, 0) : 0);
    for (size_t k = 0; k < v.size(); k++) v[k] = c.mem->read<uint32_t>(arg32(c, 1) + 4 * (uint32_t)k);
    return v;
}

void release(const std::vector<GLuint>& v, Backend::NameKind kind) {
    for (GLuint n : v) {
        if (g_trace_names) std::fprintf(stderr, "metal: delete %s %u\n", kKindNames[kind], n);
        B().names[kind].put(n);
    }
}

template <typename Map> void erase(Context& c, Map& m, Backend::NameKind kind) {
    const std::vector<GLuint> v = ids(c);
    for (GLuint n : v) m.erase(n);
    release(v, kind);
}

void set_cap(GLenum cap, bool on) {
    State& s = B().st;
    switch (cap) {
    case GL_BLEND: s.blend = on; break;
    case GL_DEPTH_TEST: s.depth_test = on; break;
    case GL_STENCIL_TEST: s.stencil_test = on; break;
    case GL_CULL_FACE: s.cull = on; break;
    case GL_ALPHA_TEST: s.alpha_test = on; break;
    case GL_POLYGON_OFFSET_FILL: s.polygon_offset = on; break;
    case GL_SCISSOR_TEST: s.scissor_test = on; break;
    case GL_VERTEX_PROGRAM_ARB: s.vp_enabled = on; break;
    case GL_FRAGMENT_PROGRAM_ARB: s.fp_enabled = on; break;
    default: break;  // texture targets, dither, multisample: nothing to do
    }
}

bool cap(GLenum c, bool* known) {
    const State& s = B().st;
    *known = true;
    switch (c) {
    case GL_BLEND: return s.blend;
    case GL_DEPTH_TEST: return s.depth_test;
    case GL_STENCIL_TEST: return s.stencil_test;
    case GL_CULL_FACE: return s.cull;
    case GL_ALPHA_TEST: return s.alpha_test;
    case GL_POLYGON_OFFSET_FILL: return s.polygon_offset;
    case GL_SCISSOR_TEST: return s.scissor_test;
    case GL_VERTEX_PROGRAM_ARB: return s.vp_enabled;
    case GL_FRAGMENT_PROGRAM_ARB: return s.fp_enabled;
    }
    *known = false;
    return false;
}

// glGet*: the state the backend tracks and fixed limits. Returns the count.
int get(GLenum pname, double* v) {
    const State& s = B().st;
    bool known;
    const bool on = cap(pname, &known);
    if (known) return v[0] = on, 1;
    switch (pname) {
    case GL_VIEWPORT: for (int k = 0; k < 4; k++) v[k] = s.viewport[k]; return 4;
    case GL_SCISSOR_BOX: for (int k = 0; k < 4; k++) v[k] = s.scissor[k]; return 4;
    case GL_DEPTH_RANGE: return v[0] = s.depth_near, v[1] = s.depth_far, 2;
    case GL_COLOR_WRITEMASK: for (int k = 0; k < 4; k++) v[k] = (s.color_mask >> k) & 1; return 4;
    case GL_COLOR_CLEAR_VALUE: for (int k = 0; k < 4; k++) v[k] = s.clear_color[k]; return 4;
    case GL_BLEND_COLOR: for (int k = 0; k < 4; k++) v[k] = s.blend_color[k]; return 4;
    case GL_DEPTH_CLEAR_VALUE: return v[0] = s.clear_depth, 1;
    case GL_STENCIL_CLEAR_VALUE: return v[0] = s.clear_stencil, 1;
    case GL_STENCIL_REF: return v[0] = s.stencil_ref, 1;
    case GL_STENCIL_VALUE_MASK: return v[0] = s.stencil_value_mask, 1;
    case GL_STENCIL_WRITEMASK: return v[0] = s.stencil_write_mask, 1;
    case GL_STENCIL_FUNC: return v[0] = s.stencil_func, 1;
    case GL_STENCIL_FAIL: return v[0] = s.stencil_fail, 1;
    case GL_STENCIL_PASS_DEPTH_FAIL: return v[0] = s.stencil_zfail, 1;
    case GL_STENCIL_PASS_DEPTH_PASS: return v[0] = s.stencil_zpass, 1;
    case GL_CULL_FACE_MODE: return v[0] = s.cull_face, 1;
    case GL_FRONT_FACE: return v[0] = s.front_face, 1;
    case GL_DEPTH_FUNC: return v[0] = s.depth_func, 1;
    case GL_DEPTH_WRITEMASK: return v[0] = s.depth_mask, 1;
    case GL_BLEND_SRC: return v[0] = s.blend_src, 1;
    case GL_BLEND_DST: return v[0] = s.blend_dst, 1;
    case GL_BLEND_EQUATION: return v[0] = s.blend_eq, 1;
    case GL_ALPHA_TEST_FUNC: return v[0] = s.alpha_func, 1;
    case GL_ALPHA_TEST_REF: return v[0] = s.alpha_ref, 1;
    case GL_POLYGON_OFFSET_FACTOR: return v[0] = s.offset_factor, 1;
    case GL_POLYGON_OFFSET_UNITS: return v[0] = s.offset_units, 1;
    case GL_ACTIVE_TEXTURE: return v[0] = GL_TEXTURE0 + s.unit, 1;
    case GL_TEXTURE_BINDING_2D: return v[0] = s.units[s.unit].t2d, 1;
    case GL_TEXTURE_BINDING_CUBE_MAP: return v[0] = s.units[s.unit].cube, 1;
    case GL_TEXTURE_BINDING_3D: return v[0] = s.units[s.unit].t3d, 1;
    case GL_ARRAY_BUFFER_BINDING: return v[0] = s.array_buffer, 1;
    case GL_ELEMENT_ARRAY_BUFFER_BINDING: return v[0] = s.element_buffer, 1;
    case GL_PIXEL_PACK_BUFFER_BINDING: return v[0] = s.pack_buffer, 1;
    case GL_PIXEL_UNPACK_BUFFER_BINDING: return v[0] = s.unpack_buffer, 1;
    case GL_FRAMEBUFFER_BINDING_EXT: return v[0] = s.draw_fb, 1;
    case GL_READ_FRAMEBUFFER_BINDING_EXT: return v[0] = s.read_fb, 1;
    case GL_RENDERBUFFER_BINDING_EXT: return v[0] = s.renderbuffer, 1;
    case GL_UNPACK_ALIGNMENT: return v[0] = s.unpack_alignment, 1;
    case GL_PACK_ALIGNMENT: return v[0] = s.pack_alignment, 1;
    case GL_MATRIX_MODE: return v[0] = s.matrix_mode, 1;
    case GL_MAX_TEXTURE_SIZE: case GL_MAX_CUBE_MAP_TEXTURE_SIZE: case GL_MAX_RENDERBUFFER_SIZE_EXT: return v[0] = 16384, 1;
    case GL_MAX_VIEWPORT_DIMS: return v[0] = v[1] = 16384, 2;
    case GL_MAX_3D_TEXTURE_SIZE: return v[0] = 2048, 1;
    case GL_MAX_TEXTURE_UNITS: case GL_MAX_TEXTURE_COORDS_ARB: return v[0] = 8, 1;
    case GL_MAX_TEXTURE_IMAGE_UNITS_ARB: case GL_MAX_VERTEX_ATTRIBS_ARB: return v[0] = 16, 1;
    case GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT: return v[0] = 16, 1;
    case GL_MAX_SAMPLES_EXT: return v[0] = 4, 1;
    case GL_MAX_COLOR_ATTACHMENTS_EXT: case GL_MAX_DRAW_BUFFERS_ARB: return v[0] = 4, 1;
    case GL_MAX_TEXTURE_LOD_BIAS_EXT: return v[0] = 16, 1;
    case GL_SAMPLE_BUFFERS: case GL_SAMPLES: return v[0] = 0, 1;
    case GL_RED_BITS: case GL_GREEN_BITS: case GL_BLUE_BITS: case GL_ALPHA_BITS: case GL_STENCIL_BITS: return v[0] = 8, 1;
    case GL_DEPTH_BITS: return v[0] = 24, 1;
    }
    char m[64];
    std::snprintf(m, sizeof m, "glGet 0x%x", pname);
    warn_once(m);
    return v[0] = 0, 1;
}

std::vector<State> g_attrib_stack;

}  // namespace

#define SHIM(N) void m_##N(Context& c)
#define RET shim_return(c, 0)

SHIM(glActiveTexture) { B().st.unit = (arg32(c, 0) - GL_TEXTURE0) & (kUnits - 1); RET; }
SHIM(glBegin) { warn_once("glBegin (immediate mode)"); RET; }
SHIM(glEnd) { RET; }
SHIM(glVertex3f) { RET; }
SHIM(glTexCoord2f) { RET; }
SHIM(glMatrixMode) { B().st.matrix_mode = arg32(c, 0); RET; }
SHIM(glLoadIdentity) { RET; }
SHIM(glPushMatrix) { RET; }
SHIM(glPopMatrix) { RET; }
SHIM(glRotatef) { RET; }
SHIM(glTranslatef) { RET; }
SHIM(gluPerspective) { RET; }
SHIM(glBeginQuery) {
    @autoreleasepool { B().begin_query(arg32(c, 1)); }
    RET;
}
SHIM(glEndQuery) { B().end_query(); RET; }
static void bind_buffer(GLenum target, GLuint name) {
    State& s = B().st;
    if (name) B().buffers[name], B().names[Backend::kBufferNames].use(name);
    switch (target) {
    case GL_ARRAY_BUFFER: s.array_buffer = name; break;
    case GL_ELEMENT_ARRAY_BUFFER: s.element_buffer = name; break;
    case GL_PIXEL_PACK_BUFFER: s.pack_buffer = name; break;
    case GL_PIXEL_UNPACK_BUFFER: s.unpack_buffer = name; break;
    }
}
SHIM(glBindBuffer) { bind_buffer(arg32(c, 0), arg32(c, 1)); RET; }
SHIM(glBindBufferARB) { bind_buffer(arg32(c, 0), arg32(c, 1)); RET; }
SHIM(glBindFramebufferEXT) {
    State& s = B().st;
    const GLenum t = arg32(c, 0);
    const GLuint f = arg32(c, 1);
    if (t == GL_FRAMEBUFFER_EXT || t == GL_DRAW_FRAMEBUFFER_EXT) s.draw_fb = f;
    if (t == GL_FRAMEBUFFER_EXT || t == GL_READ_FRAMEBUFFER_EXT) s.read_fb = f;
    if (f) B().framebuffers[f], B().names[Backend::kFramebufferNames].use(f);
    RET;
}
SHIM(glBindProgramARB) {
    (arg32(c, 0) == GL_VERTEX_PROGRAM_ARB ? B().st.vp : B().st.fp) = arg32(c, 1);
    B().names[Backend::kProgramNames].use(arg32(c, 1));
    RET;
}
SHIM(glBindRenderbufferEXT) {
    B().st.renderbuffer = arg32(c, 1);
    B().names[Backend::kRenderbufferNames].use(arg32(c, 1));
    RET;
}
SHIM(glBindTexture) {
    TexUnit& u = B().st.units[B().st.unit];
    const GLenum t = arg32(c, 0);
    const GLuint name = arg32(c, 1);
    switch (t) {
    case GL_TEXTURE_1D: u.t1d = name; break;
    case GL_TEXTURE_2D: u.t2d = name; break;
    case GL_TEXTURE_3D: u.t3d = name; break;
    case GL_TEXTURE_CUBE_MAP: u.cube = name; break;
    case GL_TEXTURE_RECTANGLE_ARB: u.rect = name; break;
    }
    if (name) {
        Texture& tex = B().textures[name];
        if (!tex.target) tex.target = t;
        B().names[Backend::kTextureNames].use(name);
    }
    RET;
}
SHIM(glBlendEquation) { B().st.blend_eq = arg32(c, 0); RET; }
SHIM(glBlendFunc) { B().st.blend_src = arg32(c, 0), B().st.blend_dst = arg32(c, 1); RET; }
SHIM(glBlendColor) {
    for (int k = 0; k < 4; k++) B().st.blend_color[k] = F(c, k);
    RET;
}
SHIM(glClear) {
    @autoreleasepool { B().clear(arg32(c, 0)); }
    RET;
}
SHIM(glClearStencil) { B().st.clear_stencil = (int)arg32(c, 0); RET; }
SHIM(glClearColor) {
    for (int k = 0; k < 4; k++) B().st.clear_color[k] = F(c, k);
    RET;
}
SHIM(glClearDepth) { B().st.clear_depth = arg_f64(c, 0); RET; }
SHIM(glCullFace) { B().st.cull_face = arg32(c, 0); RET; }
SHIM(glFrontFace) { B().st.front_face = arg32(c, 0); RET; }
SHIM(glDepthFunc) { B().st.depth_func = arg32(c, 0); RET; }
SHIM(glDepthMask) { B().st.depth_mask = Z(c, 0); RET; }
SHIM(glDepthRange) { B().st.depth_near = arg_f64(c, 0), B().st.depth_far = arg_f64(c, 2); RET; }
SHIM(glColorMask) {
    B().st.color_mask = (uint8_t)(Z(c, 0) | Z(c, 1) << 1 | Z(c, 2) << 2 | Z(c, 3) << 3);
    RET;
}
SHIM(glPolygonMode) { B().st.polygon_mode = arg32(c, 1); RET; }
SHIM(glPolygonOffset) { B().st.offset_factor = F(c, 0), B().st.offset_units = F(c, 1); RET; }
SHIM(glStencilFunc) {
    State& s = B().st;
    s.stencil_func = arg32(c, 0), s.stencil_ref = (int)arg32(c, 1), s.stencil_value_mask = arg32(c, 2);
    RET;
}
SHIM(glStencilMask) { B().st.stencil_write_mask = arg32(c, 0); RET; }
SHIM(glStencilOp) {
    State& s = B().st;
    s.stencil_fail = arg32(c, 0), s.stencil_zfail = arg32(c, 1), s.stencil_zpass = arg32(c, 2);
    RET;
}
SHIM(glAlphaFunc) { B().st.alpha_func = arg32(c, 0), B().st.alpha_ref = F(c, 1); RET; }
SHIM(glViewport) {
    for (int k = 0; k < 4; k++) B().st.viewport[k] = (int)arg32(c, k);
    RET;
}
SHIM(glPixelStorei) {
    State& s = B().st;
    switch (arg32(c, 0)) {
    case GL_UNPACK_ALIGNMENT: s.unpack_alignment = (int)arg32(c, 1); break;
    case GL_PACK_ALIGNMENT: s.pack_alignment = (int)arg32(c, 1); break;
    case GL_UNPACK_ROW_LENGTH: s.unpack_row_length = (int)arg32(c, 1); break;
    }
    RET;
}
SHIM(glReadBuffer) { RET; }
SHIM(glTexEnvf) {
    if (arg32(c, 1) == GL_TEXTURE_LOD_BIAS_EXT) B().st.units[B().st.unit].lod_bias = F(c, 2);
    RET;
}
SHIM(glEnable) { set_cap(arg32(c, 0), true); RET; }
SHIM(glDisable) { set_cap(arg32(c, 0), false); RET; }
SHIM(glEnableVertexAttribArray) { B().st.attribs[arg32(c, 0) & (kAttribs - 1)].enabled = true; RET; }
SHIM(glDisableVertexAttribArray) { B().st.attribs[arg32(c, 0) & (kAttribs - 1)].enabled = false; RET; }
// glVertexAttribPointer(index, size, type, normalized, stride, pointer)
SHIM(glVertexAttribPointer) {
    VertexAttrib& a = B().st.attribs[arg32(c, 0) & (kAttribs - 1)];
    a.size = arg32(c, 1), a.type = arg32(c, 2), a.normalized = Z(c, 3), a.stride = arg32(c, 4);
    a.pointer = arg32(c, 5), a.buffer = B().st.array_buffer;
    RET;
}
// Vertex array objects (APPLE_vertex_array_object, Brotherhood): the arrays
// and the element array binding, swapped in and out of the state on a bind.
// The current (constant) attribute values are not part of a VAO.
namespace {
struct Vao {
    VertexAttrib attribs[kAttribs];
    GLuint element_buffer = 0;
};
std::unordered_map<GLuint, Vao> g_vaos;  // node-based: references stay valid
GLuint g_vao = 0, g_next_vao = 1;
// Makes `vao` the bound one: saves the state's arrays into the old VAO, loads its.
void bind_vao(GLuint vao, bool keep_old) {
    State& s = B().st;
    // The bound one again (often, per draw): saving and loading it changes nothing.
    if (keep_old && vao == g_vao) return;
    if (keep_old) {
        Vao& old = g_vaos[g_vao];
        std::copy(std::begin(s.attribs), std::end(s.attribs), std::begin(old.attribs));
        old.element_buffer = s.element_buffer;
    }
    g_vao = vao;
    const Vao& now = g_vaos[vao];
    for (int k = 0; k < kAttribs; k++) {
        float current[4];
        std::copy(std::begin(s.attribs[k].current), std::end(s.attribs[k].current), current);
        s.attribs[k] = now.attribs[k];
        std::copy(current, current + 4, s.attribs[k].current);
    }
    s.element_buffer = now.element_buffer;
}
}  // namespace
SHIM(glBindVertexArrayAPPLE) {
    bind_vao(arg32(c, 0), true);
    RET;
}
SHIM(glGenVertexArraysAPPLE) {
    const int n = (int)arg32(c, 0);
    for (int k = 0; k < n && arg32(c, 1); k++) c.mem->write<uint32_t>(arg32(c, 1) + 4 * k, g_next_vao++);
    RET;
}
SHIM(glDeleteVertexArraysAPPLE) {
    for (GLuint n : ids(c)) {
        if (!n) continue;
        g_vaos.erase(n);
        if (n == g_vao) bind_vao(0, false);  // deleting the bound VAO binds 0
    }
    RET;
}
// glDrawBuffers(n, bufs): the colour attachments fragment programs write
// (result.color[i]).
SHIM(glDrawBuffers) {
    const uint32_t n = arg32(c, 0);
    B().st.draw_buffers = n > 4 ? 4 : (int)n;
    RET;
}
SHIM(glDrawArrays) {
    @autoreleasepool { B().draw(arg32(c, 0), (int)arg32(c, 1), (int)arg32(c, 2), 0, 0, false); }
    RET;
}
// glDrawElements(mode, count, type, indices)
SHIM(glDrawElements) {
    @autoreleasepool { B().draw(arg32(c, 0), 0, (int)arg32(c, 1), arg32(c, 2), arg32(c, 3), true); }
    RET;
}
SHIM(glFramebufferRenderbufferEXT) { B().framebuffer_attach(arg32(c, 1), 2, arg32(c, 3), 0, 0); RET; }
// glFramebufferTexture2DEXT(target, attachment, textarget, texture, level)
SHIM(glFramebufferTexture2DEXT) { B().framebuffer_attach(arg32(c, 1), 1, arg32(c, 3), arg32(c, 2), (int)arg32(c, 4)); RET; }
SHIM(glRenderbufferStorageEXT) {
    @autoreleasepool { B().renderbuffer_storage(arg32(c, 1), (int)arg32(c, 2), (int)arg32(c, 3), 0); }
    RET;
}
// glRenderbufferStorageMultisampleEXT(target, samples, internal, w, h)
SHIM(glRenderbufferStorageMultisampleEXT) {
    @autoreleasepool { B().renderbuffer_storage(arg32(c, 2), (int)arg32(c, 3), (int)arg32(c, 4), (int)arg32(c, 1)); }
    RET;
}
SHIM(glCheckFramebufferStatusEXT) { shim_return(c, B().check_framebuffer()); }
SHIM(glBlitFramebufferEXT) {
    @autoreleasepool {
        B().blit_framebuffer((int)arg32(c, 0), (int)arg32(c, 1), (int)arg32(c, 2), (int)arg32(c, 3), (int)arg32(c, 4),
                             (int)arg32(c, 5), (int)arg32(c, 6), (int)arg32(c, 7), arg32(c, 8), arg32(c, 9));
    }
    RET;
}
SHIM(glGenBuffersARB) { gen(c, Backend::kBufferNames); }
SHIM(glGenFramebuffersEXT) { gen(c, Backend::kFramebufferNames); }
SHIM(glGenProgramsARB) { gen(c, Backend::kProgramNames); }
SHIM(glGenQueries) { gen(c, Backend::kQueryNames); }
SHIM(glGenRenderbuffersEXT) { gen(c, Backend::kRenderbufferNames); }
SHIM(glGenTextures) { gen(c, Backend::kTextureNames); }
SHIM(glDeleteBuffersARB) {
    const std::vector<GLuint> v = ids(c);
    B().delete_buffers((int)v.size(), v.data());
    release(v, Backend::kBufferNames);
    RET;
}
SHIM(glDeleteTextures) {
    const std::vector<GLuint> v = ids(c);
    B().delete_textures((int)v.size(), v.data());
    release(v, Backend::kTextureNames);
    RET;
}
SHIM(glDeleteFramebuffersEXT) {
    State& s = B().st;
    for (uint32_t k = 0; k < arg32(c, 0); k++) {
        const GLuint id = c.mem->read<uint32_t>(arg32(c, 1) + 4 * k);
        if (s.draw_fb == id) s.draw_fb = 0;
        if (s.read_fb == id) s.read_fb = 0;
    }
    erase(c, B().framebuffers, Backend::kFramebufferNames);
    RET;
}
SHIM(glDeleteProgramsARB) { erase(c, B().programs, Backend::kProgramNames); RET; }
SHIM(glDeleteQueries) { erase(c, B().queries, Backend::kQueryNames); RET; }
SHIM(glDeleteRenderbuffersEXT) { erase(c, B().renderbuffers, Backend::kRenderbufferNames); RET; }
SHIM(glPushAttrib) { g_attrib_stack.push_back(B().st); RET; }
// Restores the server state groups; bindings of buffers, programs and
// framebuffers, vertex arrays and pixel storage are not attribute state.
SHIM(glPopAttrib) {
    if (g_attrib_stack.empty()) return RET;
    State& s = B().st;
    State saved = g_attrib_stack.back();
    g_attrib_stack.pop_back();
    saved.array_buffer = s.array_buffer, saved.element_buffer = s.element_buffer;
    saved.pack_buffer = s.pack_buffer, saved.unpack_buffer = s.unpack_buffer;
    saved.draw_fb = s.draw_fb, saved.read_fb = s.read_fb, saved.renderbuffer = s.renderbuffer;
    saved.vp = s.vp, saved.fp = s.fp;
    std::memcpy(saved.attribs, s.attribs, sizeof s.attribs);
    saved.unpack_alignment = s.unpack_alignment, saved.pack_alignment = s.pack_alignment;
    saved.unpack_row_length = s.unpack_row_length;
    s = saved;
    RET;
}
SHIM(glGetError) {
    const GLenum e = B().error;
    B().error = GL_NO_ERROR;
    shim_return(c, e);
}
SHIM(glGetBooleanv) {
    double v[16];
    const int n = get(arg32(c, 0), v);
    for (int k = 0; k < n && arg32(c, 1); k++) c.mem->write<uint8_t>(arg32(c, 1) + k, v[k] != 0);
    RET;
}
SHIM(glGetFloatv) {
    double v[16];
    const int n = get(arg32(c, 0), v);
    for (int k = 0; k < n && arg32(c, 1); k++) c.mem->write<float>(arg32(c, 1) + 4 * k, (float)v[k]);
    RET;
}
SHIM(glGetIntegerv) {
    double v[16];
    const int n = get(arg32(c, 0), v);
    for (int k = 0; k < n && arg32(c, 1); k++) c.mem->write<int32_t>(arg32(c, 1) + 4 * k, (int32_t)std::lround(v[k]));
    RET;
}
// glGetQueryObjectiv(id, pname, int*)
SHIM(glGetQueryObjectiv) {
    const GLuint id = arg32(c, 0);
    int32_t r = 0;
    @autoreleasepool {
        if (arg32(c, 1) == GL_QUERY_RESULT_AVAILABLE) r = B().query_available(id) ? 1 : 0;
        else if (arg32(c, 1) == GL_QUERY_RESULT) r = (int32_t)B().query_result(id);
    }
    if (arg32(c, 2)) c.mem->write<int32_t>(arg32(c, 2), r);
    RET;
}
SHIM(glFlush) { RET; }

// ---- buffers ----
// glBufferDataARB(target, size, data, usage)
SHIM(glBufferDataARB) {
    B().buffer_data(arg32(c, 0), arg32(c, 1), arg32(c, 2) ? c.mem->ptr(arg32(c, 2)) : nullptr);
    RET;
}
SHIM(glBufferSubData) {
    B().buffer_sub_data(arg32(c, 0), arg32(c, 1), arg32(c, 2), arg32(c, 3) ? c.mem->ptr(arg32(c, 3)) : nullptr);
    RET;
}
SHIM(glMapBufferARB) {
    uint32_t g;
    @autoreleasepool { g = B().map_buffer(arg32(c, 0), arg32(c, 1)); }
    shim_return(c, g);
}
SHIM(glUnmapBufferARB) { shim_return(c, B().unmap_buffer(arg32(c, 0)) ? GL_TRUE : GL_FALSE); }

// ---- programs ----
// glProgramStringARB(target, format, length, string)
SHIM(glProgramStringARB) {
    const std::string text = arb_store::text(*c.mem, arg32(c, 3), arg32(c, 2));
    B().program_string(arg32(c, 0), text);
    RET;
}
// gl{Env,Local}Parameters4fvEXT(target, index, count, const float*)
SHIM(glProgramEnvParameters4fvEXT) {
    B().program_env(arg32(c, 0), arg32(c, 1), arg32(c, 2), arg32(c, 3) ? (const float*)c.mem->ptr(arg32(c, 3)) : nullptr);
    RET;
}
SHIM(glProgramLocalParameters4fvEXT) {
    B().program_local(arg32(c, 0), arg32(c, 1), arg32(c, 2), arg32(c, 3) ? (const float*)c.mem->ptr(arg32(c, 3)) : nullptr);
    RET;
}

// ---- textures ----
#define TEX_CALL(...)                     \
    do {                                  \
        @autoreleasepool { __VA_ARGS__; } \
        RET;                              \
    } while (0)
// glTexImage1D(target, level, internal, w, border, format, type, pixels)
SHIM(glTexImage1D) {
    TEX_CALL(B().tex_image(arg32(c, 0), (int)arg32(c, 1), arg32(c, 2), (int)arg32(c, 3), 1, 1, arg32(c, 5), arg32(c, 6),
                           unpack(c, arg32(c, 7)), false, 0));
}
// glTexImage2D(target, level, internal, w, h, border, format, type, pixels)
SHIM(glTexImage2D) {
    TEX_CALL(B().tex_image(arg32(c, 0), (int)arg32(c, 1), arg32(c, 2), (int)arg32(c, 3), (int)arg32(c, 4), 1, arg32(c, 6),
                           arg32(c, 7), unpack(c, arg32(c, 8)), false, 0));
}
// glTexImage3D(target, level, internal, w, h, d, border, format, type, pixels)
SHIM(glTexImage3D) {
    TEX_CALL(B().tex_image(arg32(c, 0), (int)arg32(c, 1), arg32(c, 2), (int)arg32(c, 3), (int)arg32(c, 4), (int)arg32(c, 5),
                           arg32(c, 7), arg32(c, 8), unpack(c, arg32(c, 9)), false, 0));
}
// glTexSubImage1D(target, level, x, w, format, type, pixels)
SHIM(glTexSubImage1D) {
    TEX_CALL(B().tex_sub_image(arg32(c, 0), (int)arg32(c, 1), (int)arg32(c, 2), 0, 0, (int)arg32(c, 3), 1, 1, arg32(c, 4),
                               arg32(c, 5), unpack(c, arg32(c, 6)), false, 0));
}
// glTexSubImage2D(target, level, x, y, w, h, format, type, pixels)
SHIM(glTexSubImage2D) {
    TEX_CALL(B().tex_sub_image(arg32(c, 0), (int)arg32(c, 1), (int)arg32(c, 2), (int)arg32(c, 3), 0, (int)arg32(c, 4),
                               (int)arg32(c, 5), 1, arg32(c, 6), arg32(c, 7), unpack(c, arg32(c, 8)), false, 0));
}
// glTexSubImage3D(target, level, x, y, z, w, h, d, format, type, pixels)
SHIM(glTexSubImage3D) {
    TEX_CALL(B().tex_sub_image(arg32(c, 0), (int)arg32(c, 1), (int)arg32(c, 2), (int)arg32(c, 3), (int)arg32(c, 4),
                               (int)arg32(c, 5), (int)arg32(c, 6), (int)arg32(c, 7), arg32(c, 8), arg32(c, 9),
                               unpack(c, arg32(c, 10)), false, 0));
}
// glCompressedTexImage1DARB(target, level, internal, w, border, size, data)
SHIM(glCompressedTexImage1DARB) {
    TEX_CALL(B().tex_image(arg32(c, 0), (int)arg32(c, 1), arg32(c, 2), (int)arg32(c, 3), 1, 1, 0, 0, unpack(c, arg32(c, 6)),
                           true, arg32(c, 5)));
}
// glCompressedTexImage2DARB(target, level, internal, w, h, border, size, data)
SHIM(glCompressedTexImage2DARB) {
    TEX_CALL(B().tex_image(arg32(c, 0), (int)arg32(c, 1), arg32(c, 2), (int)arg32(c, 3), (int)arg32(c, 4), 1, 0, 0,
                           unpack(c, arg32(c, 7)), true, arg32(c, 6)));
}
// glCompressedTexImage3DARB(target, level, internal, w, h, d, border, size, data)
SHIM(glCompressedTexImage3DARB) {
    TEX_CALL(B().tex_image(arg32(c, 0), (int)arg32(c, 1), arg32(c, 2), (int)arg32(c, 3), (int)arg32(c, 4), (int)arg32(c, 5),
                           0, 0, unpack(c, arg32(c, 8)), true, arg32(c, 7)));
}
// glCompressedTexSubImage1DARB(target, level, x, w, format, size, data)
SHIM(glCompressedTexSubImage1DARB) {
    TEX_CALL(B().tex_sub_image(arg32(c, 0), (int)arg32(c, 1), (int)arg32(c, 2), 0, 0, (int)arg32(c, 3), 1, 1, arg32(c, 4), 0,
                               unpack(c, arg32(c, 6)), true, arg32(c, 5)));
}
// glCompressedTexSubImage2DARB(target, level, x, y, w, h, format, size, data)
SHIM(glCompressedTexSubImage2DARB) {
    TEX_CALL(B().tex_sub_image(arg32(c, 0), (int)arg32(c, 1), (int)arg32(c, 2), (int)arg32(c, 3), 0, (int)arg32(c, 4),
                               (int)arg32(c, 5), 1, arg32(c, 6), 0, unpack(c, arg32(c, 8)), true, arg32(c, 7)));
}
// glCompressedTexSubImage3DARB(target, level, x, y, z, w, h, d, format, size, data)
SHIM(glCompressedTexSubImage3DARB) {
    TEX_CALL(B().tex_sub_image(arg32(c, 0), (int)arg32(c, 1), (int)arg32(c, 2), (int)arg32(c, 3), (int)arg32(c, 4),
                               (int)arg32(c, 5), (int)arg32(c, 6), (int)arg32(c, 7), arg32(c, 8), 0, unpack(c, arg32(c, 10)),
                               true, arg32(c, 9)));
}
SHIM(glTexParameteri) { B().tex_parameter(arg32(c, 0), arg32(c, 1), (float)(int32_t)arg32(c, 2)); RET; }
SHIM(glTexParameterfv) {
    if (arg32(c, 2)) B().tex_parameter(arg32(c, 0), arg32(c, 1), c.mem->read<float>(arg32(c, 2)));
    RET;
}
// glReadPixels(x, y, w, h, format, type, pixels): an offset with a pack buffer bound
SHIM(glReadPixels) {
    Backend& b = B();
    void* out = nullptr;
    if (b.st.pack_buffer) {
        Buffer& buf = b.buffers[b.st.pack_buffer];
        if (buf.block) out = b.mem->ptr(buf.block.guest + arg32(c, 6));
    } else if (arg32(c, 6)) {
        out = c.mem->ptr(arg32(c, 6));
    }
    @autoreleasepool {
        b.read_pixels((int)arg32(c, 0), (int)arg32(c, 1), (int)arg32(c, 2), (int)arg32(c, 3), arg32(c, 4), arg32(c, 5), out);
    }
    RET;
}

#undef SHIM
#undef RET

bool requested() {
#if TARGET_OS_IPHONE
    return true;  // the only renderer there
#endif
    static const bool r = [] {
        const char* v = std::getenv("SCIMITAR_RENDERER");
        return v && std::strcmp(v, "metal") == 0;
    }();
    return r;
}

void attach_layer(void* layer, GuestMemory* mem, int width, int height) {
    if (!B().init(mem)) fatal("metal: no Metal device");
    B().attach_layer((__bridge CAMetalLayer*)layer, width, height);
}
void set_default_size(int width, int height) { B().set_default_size(width, height); }
void set_drawable_size(int width, int height) { B().drawable_w = width, B().drawable_h = height; }
void set_vsync(bool on) {
    if (std::getenv("SCIMITAR_VERBOSE")) std::fprintf(stderr, "metal: vsync %s\n", on ? "on" : "off");
    B().vsync = on;
}
void set_frame_cap(int fps) { B().frame_min_duration = fps > 0 ? 1.0 / fps : 0.0; }
void present() { B().present(); }
bool read_default(int* width, int* height, std::vector<uint8_t>* rgb) {
    @autoreleasepool { return B().read_default(width, height, rgb); }
}
double compile_ms() { return B().compile_ms.load(); }
void set_forced_anisotropy(int level) { B().forced_aniso = std::clamp(level, 1, 16); }
int forced_anisotropy() { return B().forced_aniso.load(); }

void set_draw_shift(int dx, int dy) {
    B().shift_x = dx, B().shift_y = -dy;
}
void trace_frame() {
    static const bool on = std::getenv("SCIMITAR_TRACE_GL") != nullptr;
    if (!on) return;
    Backend& b = B();
    // GPU time per frame since the last report.
    static double last_ms = 0;
    static uint64_t last_frames = 0;
    const double ms = b.gpu_ms.load();
    const uint64_t frames = b.gpu_frames.load();
    const double per_frame = frames > last_frames ? (ms - last_ms) / (double)(frames - last_frames) : 0;
    last_ms = ms, last_frames = frames;
    std::fprintf(stderr, "metal: %llu draws (%llu skipped), %llu passes, %llu clears, %llu pipelines, %llu renames, "
                         "%llu command buffer splits; GPU %.2f ms a frame\n",
                 (unsigned long long)b.n_draws, (unsigned long long)b.n_skipped, (unsigned long long)b.n_passes,
                 (unsigned long long)b.n_clears, (unsigned long long)b.n_pipelines, (unsigned long long)b.n_renames,
                 (unsigned long long)b.n_splits, per_frame);
}

}  // namespace scimitar::metal
