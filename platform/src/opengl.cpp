// OpenGL, CGL and GLU imports, passed to the host's legacy OpenGL 2.1 (which
// accepts the game's ARB programs, see tests/gl). Guest pointers become host
// pointers (guest memory is host memory), except where GL takes an offset into
// a bound buffer object instead of a pointer (vertex attributes and indices
// with a VBO/IBO bound, pixel transfers with a PBO bound). Mapped buffers get a
// guest copy, written back on unmap. ARB vertex programs that read
// "vertex.attrib[N].secondary" (cgc 2.2 output) lose the meaningless suffix,
// which Apple's parser rejects.
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <OpenGL/glu.h>

#include <pthread.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "scimitar/arb_store.h"
#include "scimitar/guest_heap.h"
#include "scimitar/imports.h"
#include "scimitar/runtime.h"
#include "metal/shims.h"

namespace scimitar::imports {

namespace {

// SCIMITAR_RENDERER=metal: the shims below hand their calls to the Metal backend.
const bool g_metal = metal::requested();
void log_call(const char* name, Context& c, int nargs);
// SCIMITAR_TRACE_GL_THREADS=1: which threads make GL calls (the first call of
// each new thread, with its name). One context means one thread at a time.
const bool g_trace_threads = std::getenv("SCIMITAR_TRACE_GL_THREADS") != nullptr;
void note_thread(const char* name) {
    static std::mutex m;
    static std::map<uint64_t, uint64_t> seen;  // thread -> calls
    uint64_t tid = 0;
    pthread_threadid_np(nullptr, &tid);
    std::lock_guard<std::mutex> l(m);
    if (seen[tid]++ == 0) {
        char tname[64] = {};
        pthread_getname_np(pthread_self(), tname, sizeof tname);
        std::fprintf(stderr, "gl: thread %llu (%s) first call %s; %zu threads so far\n", (unsigned long long)tid, tname,
                     name + 4, seen.size());
    }
}
#define METAL(N)                                \
    if (g_trace_threads) note_thread("imp_" #N); \
    if (g_metal) {                              \
        log_call("imp_" #N, c, 10);             \
        return metal::m_##N(c);                 \
    }

const void* P(Context& c, uint32_t a) { return a ? c.mem->ptr(a) : nullptr; }
void* W(Context& c, uint32_t a) { return a ? c.mem->ptr(a) : nullptr; }
GLfloat F(Context& c, int i) { return arg_f32(c, i); }
GLboolean B(Context& c, int i) { return (GLboolean)(arg32(c, i) & 0xff); }

// The buffers bound to each target, tracked in glBindBuffer(ARB) and
// glDeleteBuffersARB rather than asked with glGetIntegerv on every draw and
// attribute pointer (Apple's GL makes that query expensive). One context,
// driven by the render thread.
GLuint g_array_buffer = 0, g_element_buffer = 0, g_pack_buffer = 0, g_unpack_buffer = 0;
GLuint* buffer_slot(GLenum target) {
    switch (target) {
    case GL_ARRAY_BUFFER: return &g_array_buffer;
    case GL_ELEMENT_ARRAY_BUFFER: return &g_element_buffer;
    case GL_PIXEL_PACK_BUFFER: return &g_pack_buffer;
    case GL_PIXEL_UNPACK_BUFFER: return &g_unpack_buffer;
    default: return nullptr;
    }
}
bool bound(GLenum binding) {
    switch (binding) {
    case GL_ARRAY_BUFFER_BINDING: return g_array_buffer != 0;
    case GL_ELEMENT_ARRAY_BUFFER_BINDING: return g_element_buffer != 0;
    case GL_PIXEL_PACK_BUFFER_BINDING: return g_pack_buffer != 0;
    case GL_PIXEL_UNPACK_BUFFER_BINDING: return g_unpack_buffer != 0;
    }
    GLint b = 0;
    glGetIntegerv(binding, &b);
    return b != 0;
}
// A pointer argument that is a buffer offset when `binding` has a buffer.
const void* PO(Context& c, uint32_t a, GLenum binding) {
    return bound(binding) ? (const void*)(uintptr_t)a : P(c, a);
}

// With SCIMITAR_TRACE_GL: counts of what the game does, printed by
// gl_trace_frame() (the shell calls it when presenting), and program errors.
bool g_trace = std::getenv("SCIMITAR_TRACE_GL") != nullptr;
std::atomic<uint64_t> g_draws{0}, g_clears{0}, g_tex{0}, g_programs{0}, g_program_errors{0}, g_fbo_binds{0};
// With SCIMITAR_TRACE_GL_FRAME=N every GL call of frame N is printed with its
// first argument words; =A-B prints frames A to B (0: from startup).
struct LogFrames {
    uint64_t first = 0, last = 0;
    bool on = false;
};
const LogFrames g_log_frames = [] {
    LogFrames r;
    if (const char* s = std::getenv("SCIMITAR_TRACE_GL_FRAME")) {
        char* end = nullptr;
        r.first = r.last = std::strtoull(s, &end, 0);
        if (end && *end == '-') r.last = std::strtoull(end + 1, nullptr, 0);
        r.on = true;
    }
    return r;
}();
std::atomic<bool> g_log{g_log_frames.on && g_log_frames.first == 0};
void log_call(const char* name, Context& c, int nargs) {
    if (!g_log.load(std::memory_order_relaxed)) return;
    std::fprintf(stderr, "  %s(", name + 4);
    for (int i = 0; i < nargs; i++) std::fprintf(stderr, "%s0x%x", i ? ", " : "", arg32(c, i));
    std::fprintf(stderr, ")\n");
}

// Mapped buffers: (target, host pointer) -> guest copy.
struct Mapping {
    void* host;
    uint32_t guest, size;
    GLenum access;
};
std::mutex g_map_lock;
std::map<GLenum, Mapping> g_maps;

// Write-only maps without the driver. With Apple's multithreaded GL engine,
// glMapBuffer (and the glGetBufferParameteriv for its size) make the render
// thread wait for the GL thread to drain its queue; the game orphans and maps a
// dynamic buffer about 75 times a frame. A write-only map hands out a guest
// staging block instead (kept per buffer), and unmap uploads it with
// glBufferSubData, which queues without waiting. Buffer sizes come from
// glBufferData.
std::map<GLuint, uint32_t> g_buffer_size;                 // buffer -> size from glBufferData
std::map<GLuint, std::pair<uint32_t, uint32_t>> g_staging;  // buffer -> guest block, its size

// Occlusion queries without a wait per read. The game reads about 950 queries a
// frame (available, then result), interleaved with its draws, and every read
// made the render thread wait for the GL thread. The first read of a frame now
// collects every finished query at once; reads are answered from that.
struct Query {
    bool available = false;
    GLuint result = 0;
    uint64_t asked = ~0ull;  // the frame of the last availability read
};
std::map<GLuint, Query> g_queries;
std::vector<std::pair<GLuint, uint64_t>> g_pending_queries;  // ended, not collected: id, frame
GLuint g_active_query = 0;
uint64_t g_frame = 0, g_query_frame = ~0ull;

// Queries that ended in an earlier frame are waited for (the game waits for
// every one anyway), all at once; this frame's are only polled.
void collect_queries() {
    std::vector<std::pair<GLuint, uint64_t>> still;
    for (const auto& [id, frame] : g_pending_queries) {
        if (id == g_active_query) {
            still.push_back({id, frame});
            continue;
        }
        if (frame == g_frame) {
            GLint available = 0;
            glGetQueryObjectiv(id, GL_QUERY_RESULT_AVAILABLE, &available);
            if (!available) {
                still.push_back({id, frame});
                continue;
            }
        }
        GLuint r = 0;
        glGetQueryObjectuiv(id, GL_QUERY_RESULT, &r);
        g_queries[id].available = true, g_queries[id].result = r;
    }
    g_pending_queries.swap(still);
}

// Shadow samplers. 9,282 of the forge fragment programs read texture units 9
// and 10 as SHADOW2D, and the engine sometimes leaves a colour texture bound
// there. NVIDIA's GL of the time compared against it; Apple's samples zero
// ("wrong sampler type (Depth)"), which reads as fully shadowed. So for each
// draw, a shadow unit whose bound texture is not a depth texture gets a 1x1
// depth texture of 1.0 (lit) instead. Tracked: each unit's 2D texture, which
// textures are depth textures, each fragment program's shadow units. All of it
// on the render thread, which owns the context.
GLenum g_unit = 0;                        // active unit, 0-based
GLuint g_bound[32] = {};                  // GL_TEXTURE_2D per unit
std::map<GLuint, bool> g_depth_texture;   // texture -> has a depth format
std::map<GLuint, uint32_t> g_shadow_units;  // fragment program -> unit mask
GLuint g_fragment_program = 0;
GLuint g_lit_shadow = 0;                  // the 1x1 depth texture

// Redundant state filtering. The engine already skips most repeated state,
// but about 10% of its calls still repeat the current value (glTexEnvf nearly
// always), and uniform uploads often resend the same data. Every call is work
// for the GL thread, which is the bottleneck, so repeats are dropped here.
// Measured: no gain (the lookups cost the render thread what the GL thread
// saves), so it is off unless SCIMITAR_GL_FILTER=1. Popping attributes and
// deleting buffers forget what is known.
const bool g_filter = std::getenv("SCIMITAR_GL_FILTER") && std::getenv("SCIMITAR_GL_FILTER")[0] == '1';
std::atomic<uint64_t> g_filtered{0};
struct EnvParams {
    float v[256][4];
    bool valid[256] = {};
};
EnvParams g_env_vertex, g_env_fragment;
std::map<uint64_t, uint32_t> g_texenv;            // (unit, target, pname) -> value bits
std::map<uint32_t, std::array<uint32_t, 6>> g_attrib;  // index -> size, type, norm, stride, pointer, buffer
std::map<int, std::array<uint32_t, 4>> g_scalar;  // which call -> its arguments
void forget_state() {
    std::fill(std::begin(g_env_vertex.valid), std::end(g_env_vertex.valid), false);
    std::fill(std::begin(g_env_fragment.valid), std::end(g_env_fragment.valid), false);
    g_texenv.clear();
    g_attrib.clear();
    g_scalar.clear();
}
// True when this call (identified by `which`) repeats its last arguments.
bool same_scalar(int which, std::array<uint32_t, 4> args) {
    if (!g_filter) return false;
    auto it = g_scalar.find(which);
    if (it != g_scalar.end() && it->second == args) {
        g_filtered++;
        return true;
    }
    g_scalar[which] = args;
    return false;
}

bool is_depth_format(GLint f) {
    return f == GL_DEPTH_COMPONENT || f == GL_DEPTH_COMPONENT16 || f == GL_DEPTH_COMPONENT24 ||
           f == GL_DEPTH_COMPONENT32 || f == GL_DEPTH24_STENCIL8_EXT;
}

uint32_t shadow_units(const std::string& text) {
    uint32_t mask = 0;
    for (size_t p = text.find("SHADOW2D"); p != std::string::npos; p = text.find("SHADOW2D", p + 1)) {
        const size_t open = text.rfind("texture[", p);
        if (open != std::string::npos && p - open < 24) {
            const int unit = std::atoi(text.c_str() + open + 8);
            if (unit >= 0 && unit < 32) mask |= 1u << unit;
        }
    }
    return mask;
}

// Runs `draw` with lit depth textures on shadow units that lack one.
template <typename F> void draw_with_shadow_fallback(F draw) {
    const auto it = g_shadow_units.find(g_fragment_program);
    uint32_t swap = 0;
    if (it != g_shadow_units.end())
        for (uint32_t m = it->second; m; m &= m - 1) {
            const int u = __builtin_ctz(m);
            const auto d = g_depth_texture.find(g_bound[u]);
            if (d == g_depth_texture.end() || !d->second) swap |= 1u << u;
        }
    if (!swap) return draw();
    if (!g_lit_shadow) {
        glGenTextures(1, &g_lit_shadow);
        glActiveTexture(GL_TEXTURE0 + g_unit);
        glBindTexture(GL_TEXTURE_2D, g_lit_shadow);
        const GLfloat one = 1.0f;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, 1, 1, 0, GL_DEPTH_COMPONENT, GL_FLOAT, &one);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_R_TO_TEXTURE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
        glBindTexture(GL_TEXTURE_2D, g_bound[g_unit]);
    }
    for (uint32_t m = swap; m; m &= m - 1) {
        glActiveTexture(GL_TEXTURE0 + __builtin_ctz(m));
        glBindTexture(GL_TEXTURE_2D, g_lit_shadow);
    }
    draw();
    for (uint32_t m = swap; m; m &= m - 1) {
        glActiveTexture(GL_TEXTURE0 + __builtin_ctz(m));
        glBindTexture(GL_TEXTURE_2D, g_bound[__builtin_ctz(m)]);
    }
    glActiveTexture(GL_TEXTURE0 + g_unit);
}

}  // namespace

#define V0(N) void imp_##N(Context& c) { METAL(N) log_call(__func__, c, 0); N(); shim_return(c, 0); }
#define V1(N) void imp_##N(Context& c) { METAL(N) log_call(__func__, c, 1); N(arg32(c, 0)); shim_return(c, 0); }
#define V2(N) void imp_##N(Context& c) { METAL(N) log_call(__func__, c, 2); N(arg32(c, 0), arg32(c, 1)); shim_return(c, 0); }
#define V3(N) void imp_##N(Context& c) { METAL(N) log_call(__func__, c, 3); N(arg32(c, 0), arg32(c, 1), arg32(c, 2)); shim_return(c, 0); }
#define V4(N) void imp_##N(Context& c) { METAL(N) log_call(__func__, c, 4); N(arg32(c, 0), arg32(c, 1), arg32(c, 2), arg32(c, 3)); shim_return(c, 0); }
#define V5(N) void imp_##N(Context& c) { METAL(N) log_call(__func__, c, 5); N(arg32(c, 0), arg32(c, 1), arg32(c, 2), arg32(c, 3), arg32(c, 4)); shim_return(c, 0); }
// (GLsizei n, GLuint* ids) in or out
#define VNP(N) void imp_##N(Context& c) { METAL(N) log_call(__func__, c, 2); N((GLsizei)arg32(c, 0), (GLuint*)W(c, arg32(c, 1))); shim_return(c, 0); }

V1(glBegin)
V2(glBindRenderbufferEXT)
V1(glBlendEquation) V1(glClearStencil) V1(glCullFace)
V1(glDisable) V1(glDisableVertexAttribArray) V1(glEnable)
V1(glEnableVertexAttribArray) V0(glEnd) V4(glFramebufferRenderbufferEXT)
V5(glFramebufferTexture2DEXT) V1(glFrontFace) V0(glLoadIdentity) V1(glMatrixMode) V2(glPixelStorei)
V0(glPopMatrix) V1(glPushAttrib) V0(glPushMatrix)
V1(glReadBuffer) V4(glRenderbufferStorageEXT) V5(glRenderbufferStorageMultisampleEXT)
V3(glStencilFunc) V3(glTexParameteri)
VNP(glDeleteFramebuffersEXT) VNP(glDeleteProgramsARB) VNP(glDeleteQueries)
VNP(glDeleteRenderbuffersEXT) VNP(glDeleteTextures) VNP(glGenBuffersARB) VNP(glGenFramebuffersEXT)
VNP(glGenProgramsARB) VNP(glGenQueries) VNP(glGenRenderbuffersEXT) VNP(glGenTextures)
#undef V0
#undef V1
#undef V2
#undef V3
#undef V4
#undef V5
#undef VNP

void imp_glBindFramebufferEXT(Context& c) { METAL(glBindFramebufferEXT) log_call(__func__, c, 4);
    g_fbo_binds++;
    glBindFramebufferEXT(arg32(c, 0), arg32(c, 1));
    shim_return(c, 0);
}
void imp_glClear(Context& c) { METAL(glClear) log_call(__func__, c, 4);
    g_clears++;
    glClear(arg32(c, 0));
    shim_return(c, 0);
}
void imp_glDrawArrays(Context& c) { METAL(glDrawArrays) log_call(__func__, c, 4);
    g_draws++;
    draw_with_shadow_fallback([&] { glDrawArrays(arg32(c, 0), arg32(c, 1), arg32(c, 2)); });
    shim_return(c, 0);
}
void imp_glBindBuffer(Context& c) { METAL(glBindBuffer) log_call(__func__, c, 2);
    if (GLuint* slot = buffer_slot(arg32(c, 0))) {
        if (g_filter && *slot == arg32(c, 1)) {
            g_filtered++;
            return shim_return(c, 0);
        }
        *slot = arg32(c, 1);
    }
    glBindBuffer(arg32(c, 0), arg32(c, 1));
    shim_return(c, 0);
}
void imp_glBindBufferARB(Context& c) { METAL(glBindBufferARB) log_call(__func__, c, 2);
    if (GLuint* slot = buffer_slot(arg32(c, 0))) {
        if (g_filter && *slot == arg32(c, 1)) {
            g_filtered++;
            return shim_return(c, 0);
        }
        *slot = arg32(c, 1);
    }
    glBindBufferARB(arg32(c, 0), arg32(c, 1));
    shim_return(c, 0);
}
// Vertex array objects (APPLE_vertex_array_object, Brotherhood): the element
// array binding is part of a VAO's state, so the tracked one follows the bound
// VAO; the redundant-call filter forgets attribute pointers on a switch.
namespace {
GLuint g_vao = 0;
std::map<GLuint, GLuint> g_vao_elements;  // VAO -> its element array buffer
}  // namespace
void imp_glBindVertexArrayAPPLE(Context& c) { METAL(glBindVertexArrayAPPLE) log_call(__func__, c, 1);
    g_vao_elements[g_vao] = g_element_buffer;
    g_vao = arg32(c, 0);
    g_element_buffer = g_vao_elements[g_vao];
    g_attrib.clear();
    glBindVertexArrayAPPLE(g_vao);
    shim_return(c, 0);
}
void imp_glGenVertexArraysAPPLE(Context& c) { METAL(glGenVertexArraysAPPLE) log_call(__func__, c, 2);
    glGenVertexArraysAPPLE((GLsizei)arg32(c, 0), (GLuint*)W(c, arg32(c, 1)));
    shim_return(c, 0);
}
void imp_glDeleteVertexArraysAPPLE(Context& c) { METAL(glDeleteVertexArraysAPPLE) log_call(__func__, c, 2);
    const GLsizei n = (GLsizei)arg32(c, 0);
    const GLuint* ids = (const GLuint*)W(c, arg32(c, 1));
    for (GLsizei k = 0; k < n && ids; k++) {
        g_vao_elements.erase(ids[k]);
        if (ids[k] == g_vao) g_vao = 0, g_element_buffer = g_vao_elements[0];  // deleting the bound VAO binds 0
    }
    glDeleteVertexArraysAPPLE(n, ids);
    shim_return(c, 0);
}
// glDrawBuffers(n, const GLenum* bufs): GL 2.0, the colour attachments
// fragment programs write with result.color[i] (Brotherhood's deferred passes).
void imp_glDrawBuffers(Context& c) { METAL(glDrawBuffers) log_call(__func__, c, 2);
    glDrawBuffers((GLsizei)arg32(c, 0), (const GLenum*)P(c, arg32(c, 1)));
    shim_return(c, 0);
}
// Deleting a bound buffer unbinds it.
void imp_glDeleteBuffersARB(Context& c) { METAL(glDeleteBuffersARB) log_call(__func__, c, 2);
    const GLsizei n = (GLsizei)arg32(c, 0);
    const GLuint* ids = (const GLuint*)W(c, arg32(c, 1));
    for (GLsizei k = 0; k < n && ids; k++) {
        for (GLuint* slot : {&g_array_buffer, &g_element_buffer, &g_pack_buffer, &g_unpack_buffer})
            if (*slot == ids[k]) *slot = 0;
        g_buffer_size.erase(ids[k]);
        g_attrib.clear();
        auto st = g_staging.find(ids[k]);
        if (st != g_staging.end()) heap::release(*c.mem, st->second.first), g_staging.erase(st);
    }
    glDeleteBuffersARB(n, ids);
    shim_return(c, 0);
}
void imp_glActiveTexture(Context& c) { METAL(glActiveTexture) log_call(__func__, c, 1);
    g_unit = (arg32(c, 0) - GL_TEXTURE0) & 31;
    glActiveTexture(arg32(c, 0));
    shim_return(c, 0);
}
void imp_glBindTexture(Context& c) { METAL(glBindTexture) log_call(__func__, c, 2);
    if (arg32(c, 0) == GL_TEXTURE_2D) g_bound[g_unit] = arg32(c, 1);
    glBindTexture(arg32(c, 0), arg32(c, 1));
    shim_return(c, 0);
}
void imp_glBindProgramARB(Context& c) { METAL(glBindProgramARB) log_call(__func__, c, 2);
    if (arg32(c, 0) == GL_FRAGMENT_PROGRAM_ARB) g_fragment_program = arg32(c, 1);
    glBindProgramARB(arg32(c, 0), arg32(c, 1));
    shim_return(c, 0);
}
void imp_glViewport(Context& c) { METAL(glViewport)
    log_call(__func__, c, 4);
    static std::atomic<int> empty{0};
    if (g_trace && (!arg32(c, 2) || !arg32(c, 3)) && empty++ == 0) {
        std::fprintf(stderr, "gl: empty viewport\n");
        print_guest_backtrace(c);
    }
    glViewport(arg32(c, 0), arg32(c, 1), arg32(c, 2), arg32(c, 3));
    shim_return(c, 0);
}
void imp_glAlphaFunc(Context& c) { METAL(glAlphaFunc) log_call(__func__, c, 2);
    if (!same_scalar(1, {arg32(c, 0), arg32(c, 1), 0, 0})) glAlphaFunc(arg32(c, 0), F(c, 1));
    shim_return(c, 0);
}
void imp_glBlendFunc(Context& c) { METAL(glBlendFunc) log_call(__func__, c, 2);
    if (!same_scalar(2, {arg32(c, 0), arg32(c, 1), 0, 0})) glBlendFunc(arg32(c, 0), arg32(c, 1));
    shim_return(c, 0);
}
void imp_glDepthFunc(Context& c) { METAL(glDepthFunc) log_call(__func__, c, 1);
    if (!same_scalar(3, {arg32(c, 0), 0, 0, 0})) glDepthFunc(arg32(c, 0));
    shim_return(c, 0);
}
void imp_glPolygonMode(Context& c) { METAL(glPolygonMode) log_call(__func__, c, 2);
    if (!same_scalar(4, {arg32(c, 0), arg32(c, 1), 0, 0})) glPolygonMode(arg32(c, 0), arg32(c, 1));
    shim_return(c, 0);
}
void imp_glStencilMask(Context& c) { METAL(glStencilMask) log_call(__func__, c, 1);
    if (!same_scalar(5, {arg32(c, 0), 0, 0, 0})) glStencilMask(arg32(c, 0));
    shim_return(c, 0);
}
void imp_glStencilOp(Context& c) { METAL(glStencilOp) log_call(__func__, c, 3);
    if (!same_scalar(6, {arg32(c, 0), arg32(c, 1), arg32(c, 2), 0})) glStencilOp(arg32(c, 0), arg32(c, 1), arg32(c, 2));
    shim_return(c, 0);
}
void imp_glPopAttrib(Context& c) { METAL(glPopAttrib) log_call(__func__, c, 0);
    glPopAttrib();
    forget_state();
    shim_return(c, 0);
}
void imp_glBlendColor(Context& c) { METAL(glBlendColor) log_call(__func__, c, 4);
    if (!same_scalar(7, {arg32(c, 0), arg32(c, 1), arg32(c, 2), arg32(c, 3)})) glBlendColor(F(c, 0), F(c, 1), F(c, 2), F(c, 3));
    shim_return(c, 0);
}
void imp_glClearColor(Context& c) { METAL(glClearColor) log_call(__func__, c, 4); glClearColor(F(c, 0), F(c, 1), F(c, 2), F(c, 3)); shim_return(c, 0); }
void imp_glClearDepth(Context& c) { METAL(glClearDepth) log_call(__func__, c, 4); glClearDepth(arg_f64(c, 0)); shim_return(c, 0); }
void imp_glDepthRange(Context& c) { METAL(glDepthRange) log_call(__func__, c, 4); glDepthRange(arg_f64(c, 0), arg_f64(c, 2)); shim_return(c, 0); }
void imp_glColorMask(Context& c) { METAL(glColorMask) log_call(__func__, c, 4); glColorMask(B(c, 0), B(c, 1), B(c, 2), B(c, 3)); shim_return(c, 0); }
void imp_glDepthMask(Context& c) { METAL(glDepthMask) log_call(__func__, c, 1);
    if (!same_scalar(8, {arg32(c, 0) & 0xff, 0, 0, 0})) glDepthMask(B(c, 0));
    shim_return(c, 0);
}
void imp_glPolygonOffset(Context& c) { METAL(glPolygonOffset) log_call(__func__, c, 4); glPolygonOffset(F(c, 0), F(c, 1)); shim_return(c, 0); }
void imp_glRotatef(Context& c) { METAL(glRotatef) log_call(__func__, c, 4); glRotatef(F(c, 0), F(c, 1), F(c, 2), F(c, 3)); shim_return(c, 0); }
void imp_glTranslatef(Context& c) { METAL(glTranslatef) log_call(__func__, c, 4); glTranslatef(F(c, 0), F(c, 1), F(c, 2)); shim_return(c, 0); }
void imp_glVertex3f(Context& c) { METAL(glVertex3f) log_call(__func__, c, 4); glVertex3f(F(c, 0), F(c, 1), F(c, 2)); shim_return(c, 0); }
void imp_glTexCoord2f(Context& c) { METAL(glTexCoord2f) log_call(__func__, c, 4); glTexCoord2f(F(c, 0), F(c, 1)); shim_return(c, 0); }
void imp_glTexEnvf(Context& c) { METAL(glTexEnvf) log_call(__func__, c, 4);
    const uint64_t key = (uint64_t)g_unit << 48 | (uint64_t)(arg32(c, 0) & 0xffff) << 32 | arg32(c, 1);
    if (g_filter) {
        auto it = g_texenv.find(key);
        if (it != g_texenv.end() && it->second == arg32(c, 2)) {
            g_filtered++;
            return shim_return(c, 0);
        }
        g_texenv[key] = arg32(c, 2);
    }
    glTexEnvf(arg32(c, 0), arg32(c, 1), F(c, 2));
    shim_return(c, 0);
}
void imp_glTexParameterfv(Context& c) { METAL(glTexParameterfv) log_call(__func__, c, 4);
    glTexParameterfv(arg32(c, 0), arg32(c, 1), (const GLfloat*)P(c, arg32(c, 2)));
    shim_return(c, 0);
}
void imp_glGetError(Context& c) { METAL(glGetError) log_call(__func__, c, 4); shim_return(c, glGetError()); }
void imp_glGetBooleanv(Context& c) { METAL(glGetBooleanv) log_call(__func__, c, 4); glGetBooleanv(arg32(c, 0), (GLboolean*)W(c, arg32(c, 1))); shim_return(c, 0); }
void imp_glGetFloatv(Context& c) { METAL(glGetFloatv) log_call(__func__, c, 4); glGetFloatv(arg32(c, 0), (GLfloat*)W(c, arg32(c, 1))); shim_return(c, 0); }
void imp_glGetIntegerv(Context& c) { METAL(glGetIntegerv) log_call(__func__, c, 4); glGetIntegerv(arg32(c, 0), (GLint*)W(c, arg32(c, 1))); shim_return(c, 0); }
// glFlush: one context, and presenting flushes; a flush would only make the
// render thread wait for the GL thread.
void imp_glFlush(Context& c) { METAL(glFlush) log_call(__func__, c, 0);
    static const bool keep = std::getenv("SCIMITAR_GL_FLUSH") && std::getenv("SCIMITAR_GL_FLUSH")[0] == '1';
    if (keep) glFlush();
    shim_return(c, 0);
}
void imp_glBeginQuery(Context& c) { METAL(glBeginQuery) log_call(__func__, c, 2);
    const GLuint id = arg32(c, 1);
    g_queries[id] = {};
    g_active_query = id;
    glBeginQuery(arg32(c, 0), id);
    shim_return(c, 0);
}
void imp_glEndQuery(Context& c) { METAL(glEndQuery) log_call(__func__, c, 1);
    glEndQuery(arg32(c, 0));
    if (g_active_query) g_pending_queries.push_back({g_active_query, g_frame});
    g_active_query = 0;
    shim_return(c, 0);
}
void imp_glGetQueryObjectiv(Context& c) { METAL(glGetQueryObjectiv) log_call(__func__, c, 4);
    const GLuint id = arg32(c, 0);
    const GLenum pname = arg32(c, 1);
    GLint* out = (GLint*)W(c, arg32(c, 2));
    static const bool batch = !(std::getenv("SCIMITAR_GL_BATCH_QUERIES") && std::getenv("SCIMITAR_GL_BATCH_QUERIES")[0] == '0');
    auto it = g_queries.find(id);
    if (!batch || it == g_queries.end() || (pname != GL_QUERY_RESULT_AVAILABLE && pname != GL_QUERY_RESULT)) {
        glGetQueryObjectiv(id, pname, out);
        return shim_return(c, 0);
    }
    if (g_query_frame != g_frame) {
        g_query_frame = g_frame;
        collect_queries();
        it = g_queries.find(id);
    }
    Query& q = it->second;
    if (pname == GL_QUERY_RESULT_AVAILABLE) {
        // Asked again within the frame: the caller is waiting for it (loading
        // does), so ask the driver, after a flush so that the GPU gets there.
        if (!q.available && q.asked == g_frame && id != g_active_query) {
            GLint available = 0;
            glGetQueryObjectiv(id, GL_QUERY_RESULT_AVAILABLE, &available);
            if (available) {
                GLuint r = 0;
                glGetQueryObjectuiv(id, GL_QUERY_RESULT, &r);
                q.available = true, q.result = r;
                g_pending_queries.erase(std::remove_if(g_pending_queries.begin(), g_pending_queries.end(),
                                                       [&](const auto& p) { return p.first == id; }),
                                        g_pending_queries.end());
            }
        }
        q.asked = g_frame;
        *out = q.available ? 1 : 0;
    } else {
        if (!q.available && id != g_active_query) {  // the game insists: wait for it
            GLuint r = 0;
            glGetQueryObjectuiv(id, GL_QUERY_RESULT, &r);
            q.available = true, q.result = r;
            g_pending_queries.erase(std::remove_if(g_pending_queries.begin(), g_pending_queries.end(),
                                                   [&](const auto& p) { return p.first == id; }),
                                    g_pending_queries.end());
        }
        *out = (GLint)q.result;
    }
    shim_return(c, 0);
}
void imp_glCheckFramebufferStatusEXT(Context& c) { METAL(glCheckFramebufferStatusEXT) log_call(__func__, c, 4); shim_return(c, glCheckFramebufferStatusEXT(arg32(c, 0))); }
// glBlitFramebufferEXT(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter)
void imp_glBlitFramebufferEXT(Context& c) { METAL(glBlitFramebufferEXT) log_call(__func__, c, 4);
    glBlitFramebufferEXT(arg32(c, 0), arg32(c, 1), arg32(c, 2), arg32(c, 3), arg32(c, 4), arg32(c, 5),
                         arg32(c, 6), arg32(c, 7), arg32(c, 8), arg32(c, 9));
    shim_return(c, 0);
}

// ---- buffers ----
// glBufferDataARB(target, GLsizeiptrARB size, const void* data, usage): sizes are 32 bits
void imp_glBufferDataARB(Context& c) { METAL(glBufferDataARB) log_call(__func__, c, 4);
    if (GLuint* slot = buffer_slot(arg32(c, 0)))
        if (*slot) g_buffer_size[*slot] = arg32(c, 1);
    glBufferDataARB(arg32(c, 0), (GLsizeiptrARB)arg32(c, 1), P(c, arg32(c, 2)), arg32(c, 3));
    shim_return(c, 0);
}
void imp_glBufferSubData(Context& c) { METAL(glBufferSubData) log_call(__func__, c, 4);
    glBufferSubData(arg32(c, 0), (GLintptr)arg32(c, 1), (GLsizeiptr)arg32(c, 2), P(c, arg32(c, 3)));
    shim_return(c, 0);
}
// void* glMapBufferARB(target, access) -> a guest copy of the mapped range
void imp_glMapBufferARB(Context& c) { METAL(glMapBufferARB) log_call(__func__, c, 4);
    const GLenum target = arg32(c, 0), access = arg32(c, 1);
    static const bool staged = !(std::getenv("SCIMITAR_GL_STAGED_MAPS") && std::getenv("SCIMITAR_GL_STAGED_MAPS")[0] == '0');
    if (access == GL_WRITE_ONLY_ARB && staged) {
        const GLuint* slot = buffer_slot(target);
        const GLuint buffer = slot ? *slot : 0;
        const auto sz = g_buffer_size.find(buffer);
        if (buffer && sz != g_buffer_size.end() && sz->second) {
            auto& st = g_staging[buffer];
            if (st.second < sz->second) {
                heap::release(*c.mem, st.first);
                st = {heap::alloc(*c.mem, sz->second, 64), sz->second};
            }
            if (st.first) {
                std::lock_guard<std::mutex> l(g_map_lock);
                g_maps[target] = {nullptr, st.first, sz->second, access};
                return shim_return(c, st.first);
            }
        }
    }
    GLint size = 0;
    glGetBufferParameterivARB(target, GL_BUFFER_SIZE_ARB, &size);
    void* host = glMapBufferARB(target, access);
    if (!host || size <= 0) return shim_return(c, 0);
    const uint32_t g = heap::alloc(*c.mem, (uint32_t)size, 64);
    if (access != GL_WRITE_ONLY_ARB) std::memcpy(c.mem->ptr(g), host, (size_t)size);
    std::lock_guard<std::mutex> l(g_map_lock);
    g_maps[target] = {host, g, (uint32_t)size, access};
    shim_return(c, g);
}
void imp_glUnmapBufferARB(Context& c) { METAL(glUnmapBufferARB) log_call(__func__, c, 4);
    const GLenum target = arg32(c, 0);
    Mapping m{};
    {
        std::lock_guard<std::mutex> l(g_map_lock);
        auto it = g_maps.find(target);
        if (it != g_maps.end()) m = it->second, g_maps.erase(it);
    }
    if (!m.host && m.guest) {  // a staged write-only map
        glBufferSubDataARB(target, 0, m.size, c.mem->ptr(m.guest));
        return shim_return(c, GL_TRUE);
    }
    if (m.host && m.access != GL_READ_ONLY_ARB) std::memcpy(m.host, c.mem->ptr(m.guest), m.size);
    const GLboolean r = glUnmapBufferARB(target);
    heap::release(*c.mem, m.guest);
    shim_return(c, r);
}

// ---- drawing ----
// glVertexAttribPointer(index, size, type, normalized, stride, pointer)
void imp_glVertexAttribPointer(Context& c) { METAL(glVertexAttribPointer) log_call(__func__, c, 4);
    if (g_filter) {
        const std::array<uint32_t, 6> a = {arg32(c, 1), arg32(c, 2), arg32(c, 3) & 0xff, arg32(c, 4), arg32(c, 5), g_array_buffer};
        auto it = g_attrib.find(arg32(c, 0));
        if (it != g_attrib.end() && it->second == a) {
            g_filtered++;
            return shim_return(c, 0);
        }
        g_attrib[arg32(c, 0)] = a;
    }
    glVertexAttribPointer(arg32(c, 0), (GLint)arg32(c, 1), arg32(c, 2), B(c, 3), (GLsizei)arg32(c, 4),
                          PO(c, arg32(c, 5), GL_ARRAY_BUFFER_BINDING));
    shim_return(c, 0);
}
// glDrawElements(mode, count, type, indices)
void imp_glDrawElements(Context& c) { METAL(glDrawElements) log_call(__func__, c, 4);
    g_draws++;
    const void* indices = PO(c, arg32(c, 3), GL_ELEMENT_ARRAY_BUFFER_BINDING);
    draw_with_shadow_fallback([&] { glDrawElements(arg32(c, 0), (GLsizei)arg32(c, 1), arg32(c, 2), indices); });
    shim_return(c, 0);
}

// ---- programs ----
void imp_glProgramStringARB(Context& c) { METAL(glProgramStringARB) log_call(__func__, c, 4);
    const GLenum target = arg32(c, 0), format = arg32(c, 1);
    std::string text = arb_store::text(*c.mem, arg32(c, 3), arg32(c, 2));
    for (size_t p = text.find("vertex.attrib["); p != std::string::npos; p = text.find("vertex.attrib[", p + 1)) {
        const size_t close = text.find(']', p);
        if (close != std::string::npos && text.compare(close + 1, 10, ".secondary") == 0) text.erase(close + 1, 10);
    }
    glProgramStringARB(target, format, (GLsizei)text.size(), text.data());
    if (target == GL_FRAGMENT_PROGRAM_ARB) {
        if (const uint32_t mask = shadow_units(text)) g_shadow_units[g_fragment_program] = mask;
        else g_shadow_units.erase(g_fragment_program);
    }
    g_programs++;
    if (g_trace) {
        GLint pos = -1;
        glGetIntegerv(GL_PROGRAM_ERROR_POSITION_ARB, &pos);
        if (pos != -1) {
            g_program_errors++;
            std::fprintf(stderr, "gl: program rejected at %d: %s\n", pos,
                         (const char*)glGetString(GL_PROGRAM_ERROR_STRING_ARB));
        }
    }
    shim_return(c, 0);
}
// gl{Env,Local}Parameters4fvEXT(target, index, count, const float*)
void imp_glProgramEnvParameters4fvEXT(Context& c) { METAL(glProgramEnvParameters4fvEXT) log_call(__func__, c, 4);
    if (g_filter) {
        const GLenum target = arg32(c, 0);
        const uint32_t index = arg32(c, 1), count = arg32(c, 2);
        EnvParams* e = target == GL_VERTEX_PROGRAM_ARB ? &g_env_vertex : target == GL_FRAGMENT_PROGRAM_ARB ? &g_env_fragment : nullptr;
        const float* data = (const float*)P(c, arg32(c, 3));
        if (e && data && index + count <= 256 && count) {
            // Send only the changed range, if any.
            uint32_t first = count, last = 0;
            for (uint32_t k = 0; k < count; k++)
                if (!e->valid[index + k] || std::memcmp(e->v[index + k], data + 4 * k, 16) != 0) {
                    if (first == count) first = k;
                    last = k;
                    std::memcpy(e->v[index + k], data + 4 * k, 16);
                    e->valid[index + k] = true;
                }
            if (first == count) {
                g_filtered++;
                return shim_return(c, 0);
            }
            glProgramEnvParameters4fvEXT(target, index + first, (GLsizei)(last - first + 1), data + 4 * first);
            return shim_return(c, 0);
        }
    }
    glProgramEnvParameters4fvEXT(arg32(c, 0), arg32(c, 1), (GLsizei)arg32(c, 2), (const GLfloat*)P(c, arg32(c, 3)));
    shim_return(c, 0);
}
void imp_glProgramLocalParameters4fvEXT(Context& c) { METAL(glProgramLocalParameters4fvEXT) log_call(__func__, c, 4);
    glProgramLocalParameters4fvEXT(arg32(c, 0), arg32(c, 1), (GLsizei)arg32(c, 2), (const GLfloat*)P(c, arg32(c, 3)));
    shim_return(c, 0);
}

// ---- textures and pixels (pointers are offsets with a pixel buffer bound) ----
#define UNPACK(k) PO(c, arg32(c, k), GL_PIXEL_UNPACK_BUFFER_BINDING)
void imp_glTexImage1D(Context& c) { METAL(glTexImage1D) log_call(__func__, c, 4);
    glTexImage1D(arg32(c, 0), arg32(c, 1), arg32(c, 2), arg32(c, 3), arg32(c, 4), arg32(c, 5), arg32(c, 6), UNPACK(7));
    shim_return(c, 0);
}
void imp_glTexImage2D(Context& c) { METAL(glTexImage2D) log_call(__func__, c, 4);
    g_tex++;
    if (arg32(c, 0) == GL_TEXTURE_2D && arg32(c, 1) == 0) g_depth_texture[g_bound[g_unit]] = is_depth_format((GLint)arg32(c, 2));
    glTexImage2D(arg32(c, 0), arg32(c, 1), arg32(c, 2), arg32(c, 3), arg32(c, 4), arg32(c, 5), arg32(c, 6),
                 arg32(c, 7), UNPACK(8));
    shim_return(c, 0);
}
void imp_glTexImage3D(Context& c) { METAL(glTexImage3D) log_call(__func__, c, 4);
    glTexImage3D(arg32(c, 0), arg32(c, 1), arg32(c, 2), arg32(c, 3), arg32(c, 4), arg32(c, 5), arg32(c, 6),
                 arg32(c, 7), arg32(c, 8), UNPACK(9));
    shim_return(c, 0);
}
void imp_glTexSubImage1D(Context& c) { METAL(glTexSubImage1D) log_call(__func__, c, 4);
    glTexSubImage1D(arg32(c, 0), arg32(c, 1), arg32(c, 2), arg32(c, 3), arg32(c, 4), arg32(c, 5), UNPACK(6));
    shim_return(c, 0);
}
void imp_glTexSubImage2D(Context& c) { METAL(glTexSubImage2D) log_call(__func__, c, 4);
    glTexSubImage2D(arg32(c, 0), arg32(c, 1), arg32(c, 2), arg32(c, 3), arg32(c, 4), arg32(c, 5), arg32(c, 6),
                    arg32(c, 7), UNPACK(8));
    shim_return(c, 0);
}
void imp_glTexSubImage3D(Context& c) { METAL(glTexSubImage3D) log_call(__func__, c, 4);
    glTexSubImage3D(arg32(c, 0), arg32(c, 1), arg32(c, 2), arg32(c, 3), arg32(c, 4), arg32(c, 5), arg32(c, 6),
                    arg32(c, 7), arg32(c, 8), arg32(c, 9), UNPACK(10));
    shim_return(c, 0);
}
void imp_glCompressedTexImage1DARB(Context& c) { METAL(glCompressedTexImage1DARB) log_call(__func__, c, 4);
    glCompressedTexImage1DARB(arg32(c, 0), arg32(c, 1), arg32(c, 2), arg32(c, 3), arg32(c, 4), arg32(c, 5), UNPACK(6));
    shim_return(c, 0);
}
void imp_glCompressedTexImage2DARB(Context& c) { METAL(glCompressedTexImage2DARB) log_call(__func__, c, 4);
    g_tex++;
    glCompressedTexImage2DARB(arg32(c, 0), arg32(c, 1), arg32(c, 2), arg32(c, 3), arg32(c, 4), arg32(c, 5),
                              arg32(c, 6), UNPACK(7));
    shim_return(c, 0);
}
void imp_glCompressedTexImage3DARB(Context& c) { METAL(glCompressedTexImage3DARB) log_call(__func__, c, 4);
    glCompressedTexImage3DARB(arg32(c, 0), arg32(c, 1), arg32(c, 2), arg32(c, 3), arg32(c, 4), arg32(c, 5),
                              arg32(c, 6), arg32(c, 7), UNPACK(8));
    shim_return(c, 0);
}
void imp_glCompressedTexSubImage1DARB(Context& c) { METAL(glCompressedTexSubImage1DARB) log_call(__func__, c, 4);
    glCompressedTexSubImage1DARB(arg32(c, 0), arg32(c, 1), arg32(c, 2), arg32(c, 3), arg32(c, 4), arg32(c, 5), UNPACK(6));
    shim_return(c, 0);
}
void imp_glCompressedTexSubImage2DARB(Context& c) { METAL(glCompressedTexSubImage2DARB) log_call(__func__, c, 4);
    glCompressedTexSubImage2DARB(arg32(c, 0), arg32(c, 1), arg32(c, 2), arg32(c, 3), arg32(c, 4), arg32(c, 5),
                                 arg32(c, 6), arg32(c, 7), UNPACK(8));
    shim_return(c, 0);
}
void imp_glCompressedTexSubImage3DARB(Context& c) { METAL(glCompressedTexSubImage3DARB) log_call(__func__, c, 4);
    glCompressedTexSubImage3DARB(arg32(c, 0), arg32(c, 1), arg32(c, 2), arg32(c, 3), arg32(c, 4), arg32(c, 5),
                                 arg32(c, 6), arg32(c, 7), arg32(c, 8), arg32(c, 9), UNPACK(10));
    shim_return(c, 0);
}
#undef UNPACK
// glReadPixels(x, y, w, h, format, type, pixels): an offset with a pack buffer bound
void imp_glReadPixels(Context& c) { METAL(glReadPixels) log_call(__func__, c, 4);
    glReadPixels(arg32(c, 0), arg32(c, 1), arg32(c, 2), arg32(c, 3), arg32(c, 4), arg32(c, 5),
                 bound(GL_PIXEL_PACK_BUFFER_BINDING) ? (void*)(uintptr_t)arg32(c, 6) : W(c, arg32(c, 6)));
    shim_return(c, 0);
}

// ---- GLU ----
void imp_gluPerspective(Context& c) { METAL(gluPerspective) log_call(__func__, c, 4);
    gluPerspective(arg_f64(c, 0), arg_f64(c, 2), arg_f64(c, 4), arg_f64(c, 6));
    shim_return(c, 0);
}
void imp_gluErrorString(Context& c) { log_call(__func__, c, 4);
    static std::map<uint32_t, uint32_t> strings;
    static std::mutex m;
    std::lock_guard<std::mutex> l(m);
    uint32_t& s = strings[arg32(c, 0)];
    if (!s) {
        const char* t = (const char*)gluErrorString(arg32(c, 0));
        const std::string v = t ? t : "unknown error";
        s = heap::alloc(*c.mem, (uint32_t)v.size() + 1);
        std::memcpy(c.mem->ptr(s), v.c_str(), v.size() + 1);
    }
    shim_return(c, s);
}

// ---- CGL: guest handles for host contexts and pixel formats ----
namespace {
std::mutex g_cgl_lock;
std::map<uint32_t, void*> g_cgl;  // guest handle -> CGLContextObj or CGLPixelFormatObj
uint32_t g_next_cgl = 0x0c610001;
uint32_t cgl_handle(void* p) {
    std::lock_guard<std::mutex> l(g_cgl_lock);
    for (const auto& [h, v] : g_cgl)
        if (v == p) return h;
    g_cgl[g_next_cgl] = p;
    return g_next_cgl++;
}
void* cgl_object(uint32_t h) {
    std::lock_guard<std::mutex> l(g_cgl_lock);
    auto it = g_cgl.find(h);
    return it == g_cgl.end() ? nullptr : it->second;
}
}  // namespace

// CGLPixelFormatObj CGLGetPixelFormat(CGLContextObj): a guest handle; 0 (the
// current context) means the calling thread's context.
void imp_CGLGetPixelFormat(Context& c) { log_call(__func__, c, 4);
    CGLContextObj ctx = arg32(c, 0) ? (CGLContextObj)cgl_object(arg32(c, 0)) : CGLGetCurrentContext();
    shim_return(c, ctx ? cgl_handle(CGLGetPixelFormat(ctx)) : 0);
}
// CGLError CGLCreateContext(CGLPixelFormatObj, CGLContextObj share, CGLContextObj* out)
void imp_CGLCreateContext(Context& c) { log_call(__func__, c, 4);
    CGLPixelFormatObj pf = (CGLPixelFormatObj)cgl_object(arg32(c, 0));
    CGLContextObj share = arg32(c, 1) ? (CGLContextObj)cgl_object(arg32(c, 1)) : nullptr;
    CGLContextObj ctx = nullptr;
    const CGLError e = CGLCreateContext(pf, share, &ctx);
    if (e == kCGLNoError) c.mem->write<uint32_t>(arg32(c, 2), cgl_handle(ctx));
    shim_return(c, (uint32_t)e);
}
void imp_CGLSetCurrentContext(Context& c) { log_call(__func__, c, 4);
    shim_return(c, (uint32_t)CGLSetCurrentContext(arg32(c, 0) ? (CGLContextObj)cgl_object(arg32(c, 0)) : nullptr));
}
// CGLError CGLSetOption(CGLGlobalOption, GLint): process-wide CGL options
// (Brotherhood asks for kCGLGOUseBuildCache...); none matters here.
void imp_CGLSetOption(Context& c) { log_call(__func__, c, 2); shim_return(c, 0); }
void imp_CGLReleaseContext(Context& c) { log_call(__func__, c, 4);
    if (void* ctx = cgl_object(arg32(c, 0))) CGLReleaseContext((CGLContextObj)ctx);
    shim_return(c, 0);
}

}  // namespace scimitar::imports

namespace scimitar {

void gl_frame_presented(uint64_t frame) {
    imports::g_frame = frame;
    // Apple's multithreaded GL engine moves the driver's work (state validation,
    // command encoding) off the render thread, which spends about a third of
    // its time there otherwise. SCIMITAR_GL_MP=0 leaves it off.
    static bool mp_done = false;
    if (!mp_done) {
        mp_done = true;
        const char* mp = std::getenv("SCIMITAR_GL_MP");
        if (!(mp && mp[0] == '0'))
            if (CGLContextObj ctx = CGLGetCurrentContext()) {
                const CGLError e = CGLEnable(ctx, kCGLCEMPEngine);
                if (std::getenv("SCIMITAR_VERBOSE")) std::fprintf(stderr, "gl: multithreaded engine %s\n", e ? "unavailable" : "on");
            }
    }
    const auto& lf = imports::g_log_frames;
    imports::g_log = lf.on && frame + 1 >= lf.first && frame + 1 <= lf.last;
    if (lf.on && frame >= lf.first && frame <= lf.last) std::fprintf(stderr, "gl: end of frame %llu\n", (unsigned long long)frame);
}

void gl_trace_frame() {
    if (!imports::g_trace) return;
    std::fprintf(stderr, "gl: %llu draws, %llu clears, %llu texture uploads, %llu framebuffer binds, %llu programs "
                         "(%llu rejected), %llu redundant calls dropped, error now 0x%x\n",
                 (unsigned long long)imports::g_draws.load(), (unsigned long long)imports::g_clears.load(),
                 (unsigned long long)imports::g_tex.load(), (unsigned long long)imports::g_fbo_binds.load(),
                 (unsigned long long)imports::g_programs.load(), (unsigned long long)imports::g_program_errors.load(),
                 (unsigned long long)imports::g_filtered.load(),
                 glGetError());
}

namespace {
using namespace imports;
#define S(N) {"_" #N, imp_##N},
const ImportShim kGL[] = {
    S(glActiveTexture) S(glBegin) S(glBeginQuery) S(glBindBuffer) S(glBindBufferARB)
    S(glBindFramebufferEXT) S(glBindProgramARB) S(glBindRenderbufferEXT) S(glBindTexture)
    S(glBlendEquation) S(glBlendFunc) S(glClear) S(glClearStencil) S(glCullFace) S(glDepthFunc)
    S(glDisable) S(glDisableVertexAttribArray) S(glDrawArrays) S(glEnable) S(glEnableVertexAttribArray)
    S(glEnd) S(glEndQuery) S(glFlush) S(glFramebufferRenderbufferEXT) S(glFramebufferTexture2DEXT)
    S(glFrontFace) S(glLoadIdentity) S(glMatrixMode) S(glPixelStorei) S(glPolygonMode) S(glPopAttrib)
    S(glPopMatrix) S(glPushAttrib) S(glPushMatrix) S(glReadBuffer) S(glRenderbufferStorageEXT)
    S(glRenderbufferStorageMultisampleEXT) S(glStencilFunc) S(glStencilMask) S(glStencilOp)
    S(glTexParameteri) S(glViewport) S(glDeleteBuffersARB) S(glDeleteFramebuffersEXT)
    S(glDeleteProgramsARB) S(glDeleteQueries) S(glDeleteRenderbuffersEXT) S(glDeleteTextures)
    S(glGenBuffersARB) S(glGenFramebuffersEXT) S(glGenProgramsARB) S(glGenQueries)
    S(glGenRenderbuffersEXT) S(glGenTextures) S(glAlphaFunc) S(glBlendColor) S(glClearColor)
    S(glClearDepth) S(glDepthRange) S(glColorMask) S(glDepthMask) S(glPolygonOffset) S(glRotatef)
    S(glTranslatef) S(glVertex3f) S(glTexCoord2f) S(glTexEnvf) S(glTexParameterfv) S(glGetError)
    S(glGetBooleanv) S(glGetFloatv) S(glGetIntegerv) S(glGetQueryObjectiv) S(glCheckFramebufferStatusEXT)
    S(glBlitFramebufferEXT) S(glBufferDataARB) S(glBufferSubData) S(glMapBufferARB) S(glUnmapBufferARB)
    S(glVertexAttribPointer) S(glDrawElements) S(glProgramStringARB) S(glProgramEnvParameters4fvEXT)
    S(glProgramLocalParameters4fvEXT) S(glTexImage1D) S(glTexImage2D) S(glTexImage3D) S(glTexSubImage1D)
    S(glTexSubImage2D) S(glTexSubImage3D) S(glCompressedTexImage1DARB) S(glCompressedTexImage2DARB)
    S(glCompressedTexImage3DARB) S(glCompressedTexSubImage1DARB) S(glCompressedTexSubImage2DARB)
    S(glCompressedTexSubImage3DARB) S(glReadPixels) S(gluPerspective) S(gluErrorString)
    S(CGLGetPixelFormat) S(CGLCreateContext) S(CGLSetCurrentContext) S(CGLReleaseContext) S(CGLSetOption)
    S(glBindVertexArrayAPPLE) S(glGenVertexArraysAPPLE) S(glDeleteVertexArraysAPPLE) S(glDrawBuffers)
};
#undef S
SCIMITAR_REGISTER_SHIMS(kGL);
}  // namespace
}  // namespace scimitar
